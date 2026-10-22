#if defined(WOWDECOMP_ARCH_32)

#include <cstdint>
#include <windows.h>
#include <cstdlib>
#include <new>
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

// STATIC: 0x00D4159C
static OsTimeManager* s_OsTimeMgr;

// VERIFY: ff120c63 manual
// OFFSET: 0x0086AB30
OsTimeManager::TimingMethod OsTimeManager::Calibrate() {
    if (!QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&this->m_performanceFrequency))) {
        this->m_timingTestError = 1;
        return SystemMethod1;
    }

    if (!this->m_performanceFrequency) {
        this->m_timingTestError = 2;
        return SystemMethod1;
    }

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    DWORD priorityClass = GetPriorityClass(process);
    int32_t threadPriority = GetThreadPriority(thread);
    SetPriorityClass(process, HIGH_PRIORITY_CLASS);
    SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
    OsSleep(0);

    this->m_timingTestError = 0;

    DWORD tick = GetTickCount();
    DWORD startTick;
    do {
        startTick = GetTickCount();
    } while (tick == startTick);

    LARGE_INTEGER startCount;
    QueryPerformanceCounter(&startCount);

    uint32_t processors = OsGetNumberOfProcessors();
    if (processors > 1) {
        int64_t lastCount = startCount.QuadPart;

        DWORD_PTR processAffinity;
        DWORD_PTR systemAffinity;
        GetProcessAffinityMask(process, &processAffinity, &systemAffinity);

        for (uint32_t i = 0; i < 512; i++) {
            SetThreadAffinityMask(thread, 1 << (i % processors));
            OsSleep(0);

            LARGE_INTEGER count;
            QueryPerformanceCounter(&count);

            if (count.QuadPart <= lastCount) {
                this->m_timingTestError = 4;
                break;
            }

            lastCount = count.QuadPart;
        }

        SetThreadAffinityMask(thread, processAffinity);
        OsSleep(0);
    }

    if (!this->m_timingTestError) {
        DWORD now;
        do {
            now = GetTickCount();
        } while (now - startTick < 250);

        DWORD endTick;
        do {
            endTick = GetTickCount();
        } while (now == endTick);

        LARGE_INTEGER endCount;
        QueryPerformanceCounter(&endCount);

        int64_t elapsed = static_cast<int64_t>(static_cast<double>(endCount.QuadPart - startCount.QuadPart) / static_cast<double>(this->m_performanceFrequency) * 1000.0);
        if (abs(static_cast<int32_t>(endTick - static_cast<uint32_t>(elapsed) - startTick)) >= 5) {
            this->m_timingTestError = 3;
        }
    }

    SetPriorityClass(process, priorityClass);
    SetThreadPriority(thread, threadPriority);
    OsSleep(0);

    return this->m_timingTestError ? SystemMethod1 : SystemMethod2;
}

// VERIFY: af8d11ce auto
// OFFSET: 0x0086AD50
uint32_t OsTimeGetTestError() {
    return s_OsTimeMgr->m_timingTestError;
}

// VERIFY: d51c349e auto
// OFFSET: 0x0086AD60
OsTimeManager::TimingMethod OsTimeGetTimingMethod() {
    return s_OsTimeMgr->m_timingMethod;
}

// VERIFY: de22f91c auto
// OFFSET: 0x0086AD70
const char* GetTimingMethodName(OsTimeManager::TimingMethod method) {
    switch (method) {
        case OsTimeManager::NotSet:
            return "[Not Set]";

        case OsTimeManager::BestAvailable:
            return "[Best Available]";

        case OsTimeManager::SystemMethod1:
            return "GetTickCount";

        case OsTimeManager::SystemMethod2:
            return "QueryPerformanceCounter";

        default:
            return "[Unknown]";
    }
}

// VERIFY: 6979dacf manual
// OFFSET: 0x0086ADC0
uint64_t OsTimeManager::Snapshot() {
    switch (this->m_timingMethod) {
        case SystemMethod1:
            break;

        case SystemMethod2: {
            LARGE_INTEGER count;
            QueryPerformanceCounter(&count);
            return static_cast<int64_t>(static_cast<double>(count.QuadPart) * this->m_scaleToMs + this->m_timeBegin);
        }

        default:
            break;
    }

    int64_t ticks = GetTickCount();
    return static_cast<int64_t>(static_cast<double>(ticks) * this->m_scaleToMs + this->m_timeBegin);
}

// VERIFY: 354d2f2e manual
// OFFSET: 0x0086AE20
uint32_t OsGetAsyncTimeMs() {
    return static_cast<uint32_t>(s_OsTimeMgr->Snapshot());
}

// VERIFY: 963f69c6 manual
// OFFSET: 0x0086AE30
uint64_t OsGetAsyncTimeClocks() {
    switch (s_OsTimeMgr->m_timingMethod) {
        case OsTimeManager::SystemMethod1:
            break;

        case OsTimeManager::SystemMethod2: {
            LARGE_INTEGER count;
            QueryPerformanceCounter(&count);
            return count.QuadPart;
        }

        default:
            break;
    }

    return GetTickCount();
}

// VERIFY: b5084b6e manual
// OFFSET: 0x0086AE70
int64_t OsGetAsyncClocksPerSecond() {
    switch (s_OsTimeMgr->m_timingMethod) {
        case OsTimeManager::SystemMethod1:
            break;

        case OsTimeManager::SystemMethod2:
            return s_OsTimeMgr->m_performanceFrequency;

        default:
            break;
    }

    return 1000;
}

// VERIFY: e025f0a1 auto
// OFFSET: 0x0086AEA0
OsTimeManager::OsTimeManager(OsTimeManager::TimingMethod method) {
    this->m_timingMethod = NotSet;

    OsTimeManager::TimingMethod calibrated = this->Calibrate();
    if (method != BestAvailable && calibrated != method) {
        if (method == SystemMethod2 && calibrated == SystemMethod1) {
            this->m_timingTestError = 5;
        }

        calibrated = method;
    }

    this->m_timingMethod = calibrated;

    int64_t clocksPerSecond;
    switch (calibrated) {
        case SystemMethod1:
            clocksPerSecond = 1000;
            break;

        case SystemMethod2:
            clocksPerSecond = this->m_performanceFrequency;
            break;

        default:
            clocksPerSecond = 1000;
            break;
    }

    this->m_scaleToMs = 1000.0 / static_cast<double>(clocksPerSecond);
    this->m_timeBegin = 0.0;
}

// VERIFY: 46e9a018 manual
// OFFSET: 0x0086AF20
void OsTimeManagerCreate(OsTimeManager::TimingMethod method) {
    s_OsTimeMgr = new (SMemAlloc(sizeof(OsTimeManager), __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY)) OsTimeManager(method);
}

// VERIFY: e138f351 manual
// OFFSET: 0x0086AF60
void OsTimeManagerDestroy() {
    if (s_OsTimeMgr) {
        delete s_OsTimeMgr;
        s_OsTimeMgr = nullptr;
    }
}

// BACKLOG F5: not in 12340.
static volatile bool g_osFastSleep;
static volatile DWORD g_osFastSleepSince;

// VERIFY: 3caadb5b auto
// OFFSET: 0x0086B280
void OsSleep(uint32_t milliseconds) {
    // BACKLOG F5 (wxl-loading fast_load): spin through the loading screen's waits; a screen up 60 s is not a
    // normal load, so stop spinning then.
    if (g_osFastSleep && GetTickCount() - g_osFastSleepSince < 60000) {
        return;
    }

    Sleep(milliseconds);
}

void OsSetFastSleep(bool on) {
    g_osFastSleepSince = GetTickCount();
    g_osFastSleep = on;
}

// CUSTOM: OsWaitUntilClocks (the precise frame cap) skips its wait under fast load, as OsSleep does
bool OsFastSleepActive() {
    return g_osFastSleep && GetTickCount() - g_osFastSleepSince < 60000;
}

#endif
