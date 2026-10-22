// Radio: internet radio streams played inside the client through BASS (un4seen), driven from Lua.
//
// In-tree port of wxl-radio (/root/wxl-radio, itself a port of mod-twow-radio). Same Lua surface, so the
// EverlookBroadcasting / ServerRadio addons run unchanged:
//
//   PlayRadioStream(url)    tune to an http(s) stream          StopRadioStream()
//   SetRadioVolume(0..1)    radio volume only                  IsRadioPlaying()  -> boolean
//   GetRadioTitle()         -> the stream's current song       SetRadioAutoPause(n)  n > 0.5 = on
//   SetRadioStopOnMinimize / SetRadioStopOnBackground          old names for SetRadioAutoPause
//
// No events are fired. The Lua functions only record what is wanted; one worker thread owns the stream, so
// connecting never freezes the game. Generated static plays while tuning (RadioStatic.h).
//
// bass.dll is loaded at run time (no import lib) from kBassPath beside the exe: the 32-bit client shares the
// wxl-radio extension's copy, the 64-bit one needs un4seen's x64 build in an x64 subfolder of it. Without it
// the functions are not registered and the addon reports the radio as not loaded.
#include "WoW/Source/Ui/Radio.h"
#include "WoW/Source/Ui/RadioStatic.h"
#include "WoW/Source/Ui/RadioStreamTitle.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/lua-5.1/src/lua.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace Radio
{
    namespace
    {
#if defined(_WIN64)
        constexpr char kBassPath[] = "Extensions\\wxl-radio\\x64\\bass.dll";
#else
        constexpr char kBassPath[] = "Extensions\\wxl-radio\\bass.dll";
#endif

        // ---- BASS 2.4, the dozen calls used ------------------------------------------------------------
        constexpr DWORD kSampleFloat   = 0x100;  // BASS_SAMPLE_FLOAT
        constexpr DWORD kAttribVol     = 2;      // BASS_ATTRIB_VOL
        constexpr DWORD kSyncMeta      = 4;      // BASS_SYNC_META: the stream sent new metadata
        constexpr DWORD kTagMeta       = 5;      // BASS_TAG_META: ICY metadata, "StreamTitle='...';"
        constexpr DWORD kCfgNetPrebuf  = 15;     // BASS_CONFIG_NET_PREBUF
        constexpr DWORD kCfgNetPlaylist= 21;     // BASS_CONFIG_NET_PLAYLIST: follow .pls/.m3u links
        constexpr DWORD kActivePlaying = 1, kActiveStalled = 2;

        using SyncProc   = void CALLBACK(DWORD sync, DWORD channel, DWORD data, void* user);
        using StreamProc = DWORD CALLBACK(DWORD stream, void* buffer, DWORD length, void* user);

        struct Bass
        {
            BOOL  (WINAPI* Init)(int device, DWORD freq, DWORD flags, HWND win, const void* clsid);
            BOOL  (WINAPI* SetConfig)(DWORD option, DWORD value);
            DWORD (WINAPI* StreamCreateURL)(const char* url, DWORD offset, DWORD flags, void* proc, void* user);
            DWORD (WINAPI* StreamCreate)(DWORD freq, DWORD channels, DWORD flags, StreamProc* proc, void* user);
            BOOL  (WINAPI* StreamFree)(DWORD stream);
            BOOL  (WINAPI* ChannelSlideAttribute)(DWORD channel, DWORD attrib, float value, DWORD ms);
            BOOL  (WINAPI* ChannelPlay)(DWORD channel, BOOL restart);
            BOOL  (WINAPI* ChannelPause)(DWORD channel);
            BOOL  (WINAPI* ChannelStop)(DWORD channel);
            DWORD (WINAPI* ChannelIsActive)(DWORD channel);
            BOOL  (WINAPI* ChannelSetAttribute)(DWORD channel, DWORD attrib, float value);
            DWORD (WINAPI* ChannelSetSync)(DWORD channel, DWORD type, uint64_t param, SyncProc* proc, void* user);
            const char* (WINAPI* ChannelGetTags)(DWORD channel, DWORD tags);
        } b{};

        /// Loads bass.dll from the client's folder (kBassPath) and fills the table. False if anything is missing.
        bool LoadBass()
        {
            char path[MAX_PATH];
            const DWORD n = GetModuleFileNameA(nullptr, path, sizeof path);
            char* slash = n && n < sizeof path ? std::strrchr(path, '\\') : nullptr;
            if (!slash || slash + 1 + sizeof kBassPath > path + sizeof path) return false;
            std::strcpy(slash + 1, kBassPath);
            const HMODULE dll = LoadLibraryA(path);
            if (!dll) return false;

            const struct { void* slot; const char* name; } imports[] = {
                { &b.Init, "BASS_Init" }, { &b.SetConfig, "BASS_SetConfig" },
                { &b.StreamCreateURL, "BASS_StreamCreateURL" }, { &b.StreamFree, "BASS_StreamFree" },
                { &b.StreamCreate, "BASS_StreamCreate" }, { &b.ChannelSlideAttribute, "BASS_ChannelSlideAttribute" },
                { &b.ChannelPlay, "BASS_ChannelPlay" }, { &b.ChannelPause, "BASS_ChannelPause" },
                { &b.ChannelStop, "BASS_ChannelStop" }, { &b.ChannelIsActive, "BASS_ChannelIsActive" },
                { &b.ChannelSetAttribute, "BASS_ChannelSetAttribute" }, { &b.ChannelSetSync, "BASS_ChannelSetSync" },
                { &b.ChannelGetTags, "BASS_ChannelGetTags" },
            };
            for (const auto& i : imports)
            {
                const FARPROC f = GetProcAddress(dll, i.name);
                if (!f) return false;
                *static_cast<FARPROC*>(i.slot) = f;
            }
            return true;
        }

        // ---- state shared between the client's main thread, the worker and BASS's own threads ---------------
        CRITICAL_SECTION g_lock;                 // guards g_url and g_title
        struct Lock { Lock() { EnterCriticalSection(&g_lock); } ~Lock() { LeaveCriticalSection(&g_lock); } };

        char                  g_url[512]   = "";   // the station wanted; empty = stopped
        char                  g_title[256] = "";
        std::atomic<unsigned> g_request{ 0 };      // bumped on every play/stop; the worker acts on a change
        std::atomic<DWORD>    g_stream{ 0 };       // the live BASS stream, 0 = none
        std::atomic<DWORD>    g_static{ 0 };       // the tuning-static stream, made once and kept
        std::atomic<float>    g_volume{ 0.5f };
        std::atomic<bool>     g_autoPause{ true };
        std::atomic<bool>     g_quit{ false };     // the client is closing: the worker stands down
        HANDLE                g_worker = nullptr;

        constexpr DWORD kTickMs         = 100;
        constexpr int   kUnfocusedTicks = 2;      // alt-tab flicker shorter than this does not pause
        constexpr DWORD kResumeWithinMs = 8000;   // paused longer than this: tune in again instead of resuming
        constexpr DWORD kFadeMs         = 600;    // static out, station in
        constexpr float kStaticLevel    = 0.5f;   // static loudness as a share of the radio volume
        constexpr DWORD kStaticRate     = 44100;
        DWORD           g_staticOffAt   = 0;      // worker only: tick at which the faded-out static is paused

        DWORD CALLBACK StaticProc(DWORD, void* buffer, DWORD length, void*)
        {
            static StaticGen gen;
            FillStatic(gen, static_cast<float*>(buffer), length / sizeof(float), static_cast<float>(kStaticRate));
            return length;
        }

        void StartStatic()
        {
            if (!g_static) g_static = b.StreamCreate(kStaticRate, 1, kSampleFloat, &StaticProc, nullptr);
            const DWORD s = g_static;
            if (!s) return;
            g_staticOffAt = 0;
            b.ChannelSetAttribute(s, kAttribVol, 0.0f);
            b.ChannelPlay(s, FALSE);
            b.ChannelSlideAttribute(s, kAttribVol, kStaticLevel * g_volume.load(), 150);
        }

        /// Any thread: the static dies away; the worker pauses it once it is silent.
        void FadeStatic() { if (const DWORD s = g_static) b.ChannelSlideAttribute(s, kAttribVol, 0.0f, kFadeMs); }

        void ReadTitle(DWORD stream)
        {
            const char* meta = b.ChannelGetTags(stream, kTagMeta);
            Lock l;
            ParseStreamTitle(meta, g_title, sizeof g_title);
        }

        void CALLBACK OnMeta(DWORD, DWORD channel, DWORD, void*) { ReadTitle(channel); }

        void CloseStream()
        {
            if (const DWORD s = g_stream.exchange(0)) { b.ChannelStop(s); b.StreamFree(s); }
            Lock l;
            g_title[0] = 0;
        }

        /// Blocks while connecting, which is why it runs on the worker.
        void OpenStream(const char* url)
        {
            static bool bassUp = false;
            if (!bassUp)
            {
                if (!b.Init(-1, 44100, 0, nullptr, nullptr)) return;   // no sound device: tried again on the next play
                b.SetConfig(kCfgNetPlaylist, 1);
                b.SetConfig(kCfgNetPrebuf, 0);
                bassUp = true;
            }
            StartStatic();
            const DWORD s = b.StreamCreateURL(url, 0, kSampleFloat, nullptr, nullptr);
            FadeStatic();
            g_staticOffAt = GetTickCount() + kFadeMs + kTickMs;
            if (!s) return;
            if (g_quit) { b.StreamFree(s); return; }   // connected while the client was closing
            b.ChannelSetAttribute(s, kAttribVol, 0.0f);
            b.ChannelSetSync(s, kSyncMeta, 0, &OnMeta, nullptr);
            ReadTitle(s);
            b.ChannelPlay(s, FALSE);
            b.ChannelSlideAttribute(s, kAttribVol, g_volume.load(), kFadeMs);   // a later SetRadioVolume overrides the slide
            g_stream = s;
        }

        DWORD WINAPI Worker(void*)
        {
            unsigned seen = 0;
            int      unfocused = 0;
            bool     paused = false;
            DWORD    pausedAt = 0;

            for (;; Sleep(kTickMs))
            {
                if (g_quit) return 0;
                // The game counts as left when the foreground window belongs to another process; a minimised
                // game is never in the foreground, so this covers both cases the original told apart.
                DWORD pid = 0;
                if (const HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
                unfocused = pid == GetCurrentProcessId() ? 0 : unfocused + 1;
                const bool wantPause = g_autoPause && unfocused >= kUnfocusedTicks;

                bool retune = false;
                if (wantPause != paused)
                {
                    paused = wantPause;
                    const DWORD s = g_stream;
                    if (paused) { if (s) b.ChannelPause(s); pausedAt = GetTickCount(); }
                    else retune = !(s && GetTickCount() - pausedAt < kResumeWithinMs && b.ChannelPlay(s, FALSE));
                }

                if (g_staticOffAt && static_cast<int32_t>(GetTickCount() - g_staticOffAt) >= 0)
                {
                    b.ChannelPause(g_static);
                    g_staticOffAt = 0;
                }

                const unsigned request = g_request;
                if (request == seen && !retune) continue;
                seen = request;

                char url[sizeof g_url];
                { Lock l; std::memcpy(url, g_url, sizeof url); }
                CloseStream();
                if (url[0] && !paused) OpenStream(url);   // asked for while away: tuned in on return (retune)
            }
        }

        // ---- Lua -------------------------------------------------------------------------------------------
        void Request(const char* url)
        {
            { Lock l; std::strncpy(g_url, url, sizeof g_url - 1); }   // g_url[511] stays 0
            ++g_request;
            if (!url[0]) FadeStatic();   // stopped mid-connect: do not hiss until the connect gives up
            if (!g_worker) g_worker = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);   // main thread only
        }

        int32_t Script_PlayRadioStream(lua_State* L)
        {
            const char* url = lua_tolstring(L, 1, nullptr);
            if (IsStreamUrl(url) && std::strlen(url) < sizeof g_url) Request(url);
            return 0;
        }

        int32_t Script_StopRadioStream(lua_State*) { Request(""); return 0; }

        int32_t Script_SetRadioVolume(lua_State* L)
        {
            const double v = lua_tonumber(L, 1);
            const float vol = v >= 1.0 ? 1.0f : v > 0.0 ? static_cast<float>(v) : 0.0f;   // NaN -> 0
            g_volume = vol;
            if (const DWORD s = g_stream) b.ChannelSetAttribute(s, kAttribVol, vol);
            return 0;
        }

        int32_t Script_IsRadioPlaying(lua_State* L)
        {
            const DWORD s = g_stream;
            const DWORD active = s ? b.ChannelIsActive(s) : 0;
            lua_pushboolean(L, active == kActivePlaying || active == kActiveStalled);
            return 1;
        }

        int32_t Script_GetRadioTitle(lua_State* L)
        {
            char title[sizeof g_title];
            { Lock l; std::memcpy(title, g_title, sizeof title); }
            lua_pushstring(L, title);
            return 1;
        }

        int32_t Script_SetRadioAutoPause(lua_State* L) { g_autoPause = lua_tonumber(L, 1) > 0.5; return 0; }

        const struct { const char* name; lua_CFunction fn; } s_ScriptFunctions[] = {
            { "PlayRadioStream", &Script_PlayRadioStream },   { "StopRadioStream", &Script_StopRadioStream },
            { "SetRadioVolume", &Script_SetRadioVolume },     { "IsRadioPlaying", &Script_IsRadioPlaying },
            { "GetRadioTitle", &Script_GetRadioTitle },       { "SetRadioAutoPause", &Script_SetRadioAutoPause },
            { "SetRadioStopOnMinimize", &Script_SetRadioAutoPause }, { "SetRadioStopOnBackground", &Script_SetRadioAutoPause },
        };

        /// bass.dll is looked for once, on the first UI load.
        bool Available()
        {
            static const bool ok = LoadBass() && (InitializeCriticalSection(&g_lock), true);
            return ok;
        }
    }
}

using namespace Radio;

void RadioRegisterScriptFunctions()
{
    if (!Available()) return;
    for (const auto& f : s_ScriptFunctions) FrameScript_RegisterFunction(f.name, f.fn);
}

void RadioUnregisterScriptFunctions()
{
    if (!Available()) return;
    for (const auto& f : s_ScriptFunctions) FrameScript_UnregisterFunction(f.name);
}

/// The client is quitting: silence the radio now instead of letting it play until the process is gone.
void RadioShutdown()
{
    if (!Available()) return;
    g_quit = true;
    if (const DWORD s = g_stream.exchange(0)) { b.ChannelStop(s); b.StreamFree(s); }
    if (const DWORD s = g_static) b.ChannelStop(s);
}
