#include <cstddef>
#include <atomic>
#include <chrono>
#include <cstdint>
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "Engine/Source/Base/Prop.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Services/Texture.h"

// STATIC: 0x00AC3438
static const char* s_asyncQueueNames[3] = { "Disk Queue", "Net Geometry Queue", "Net Texture Queue" };

// STATIC: 0x00AC3444
static uint32_t s_handlerTimeout = 100;

// STATIC: 0x00AC3448
static TSList<CAsyncQueue, TSGetLink<CAsyncQueue> > s_asyncQueueList;

// STATIC: 0x00AC3454
static TSList<CAsyncThread, TSGetLink<CAsyncThread> > s_asyncThreadList;

namespace AsyncFileRead {
    // STATIC: 0x00AC3460
    TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> s_asyncFileReadFreeList;
}

// STATIC: 0x00AC346C
static TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> s_asyncFileReadPostList;

// STATIC: 0x00B4A1EC
static int32_t s_queueLocked;

// STATIC: 0x00B4A1F0
static void (*s_ingameStartCallback)();

// STATIC: 0x00B4A1F4
static void (*s_ingameProgressCallback)(float, int32_t);

// STATIC: 0x00B4A1F8
static int32_t s_progressCount;

// STATIC: 0x00B4A1FC
static void* s_asyncProgressCallbackParams;

// STATIC: 0x00B4A200
static void (*s_asyncProgressCallback)(float, void*);

// STATIC: 0x00B4A204
static CAsyncObject* s_asyncWaitObject;

// STATIC: 0x00B4A208
static HPROPCONTEXT s_propContext;

// STATIC: 0x00B4A20C
static CAsyncQueue* s_asyncQueues[3];

// STATIC: 0x00B4A218
static uint32_t s_threadSleep;

namespace AsyncFileRead {
    // STATIC: 0x00B4A21C
    SEvent s_shutdownEvent(1, 0);
}

// STATIC: 0x00B4A220
static TSGrowableArray<void (*)()> s_asyncPollHandlers;

// STATIC: 0x00B4A230
static TSGrowableArray<int32_t (*)()> s_asyncStatusHandlers;

// STATIC: 0x00B4A240
SCritSect g_queueLock;

// Not stock (Legion worldLoadSort): disk readers take the queued load nearest the camera first (see AsyncPickNearest).
int32_t g_asyncLoadSort = 1;
bool g_asyncLoadOriginSet;
float g_asyncLoadOrigin[2];
static float s_asyncViewPos[2];

// Not stock (Legion componentTexLoadLimit-style texUploadLimit): at most this many BLP textures are created (GPU
// texture alloc + upload) per frame by the poll handler; the rest wait in this list for the next frames. 0 = no cap.
int32_t g_asyncTexUploadLimit = 24;
static TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> s_asyncFileReadDeferList;

void AsyncFileReadSetViewPos(float x, float y) {
    g_queueLock.Enter();
    s_asyncViewPos[0] = x;
    s_asyncViewPos[1] = y;
    g_queueLock.Leave();
}

// Not stock. Caller holds g_queueLock. Among the first entries sharing the head's priority, return the one nearest
// the camera. Objects without a position, the waited-on object and anything queued > 120 frames ago keep stock
// (head-first) order, so nothing starves. ponytail: scans at most 64 entries per pop; a heap if queues get huge.
static CAsyncObject* AsyncPickNearest(CAsyncQueue* queue, CAsyncObject* head) {
    if (!g_asyncLoadSort || head == s_asyncWaitObject || !head->m_hasLoadPos || g_theGxDevicePtr->m_frameCount - head->m_linkFrame > 120) {
        return head;
    }

    CAsyncObject* best = head;
    float bestDist = 3.4e38f;
    uint32_t scanned = 0;

    for (CAsyncObject* object = head; !queue->m_readList.IsEnd(object) && object->m_priority == head->m_priority && scanned < 64; object = queue->m_readList.RawNext(object), scanned++) {
        if (!object->m_hasLoadPos) {
            return object;
        }

        float dx = object->m_loadX - s_asyncViewPos[0];
        float dy = object->m_loadY - s_asyncViewPos[1];
        float dist = dx * dx + dy * dy;

        if (dist < bestDist) {
            bestDist = dist;
            best = object;
        }
    }

    return best;
}

// VERIFY: 5a8ab5a9 auto
// OFFSET: 0x004B50A0
int32_t CAsyncObject::IsPending() {
    return !this->m_isCurrent && !this->m_isRead && !this->m_isProcessed;
}

// VERIFY: 6305e7e3 auto
// OFFSET: 0x004B9910
void AsyncFileReadSetProgressCallback(void (*callback)(float, void*), void* param) {
    s_asyncProgressCallback = callback;
    s_asyncProgressCallbackParams = param;
}

// VERIFY: fe99c992 auto
// OFFSET: 0x004B9930
void AsyncFileReadIngameProgressCallback(void (*startCallback)(), void (*progressCallback)(float, int32_t)) {
    s_ingameStartCallback = startCallback;
    s_ingameProgressCallback = progressCallback;
}

// VERIFY: 08e4e19e auto
// OFFSET: 0x004B9950
void AsyncFile::EnterQueueLock() {
    s_queueLocked = 1;
    g_queueLock.Enter();
}

// VERIFY: ee2b2778 auto
// OFFSET: 0x004B9970
void AsyncFile::LeaveQueueLock() {
    g_queueLock.Leave();
    s_queueLocked = 0;
}

// Not stock: the disk queue's default reader count, clamp(cores / 2, 2, 4).
static uint32_t AsyncFileReadAutoDiskThreads() {
    Blizzard::Hardware::GetProcessorFeatures();
    uint32_t count = System_Hardware::s_processorFeatures.m_processorCount / 2;
    return count < 2 ? 2 : count > 4 ? 4 : count;
}

#if defined(WOWDECOMP_ARCH_64)
// MODIFIED: performance (Badgermilk0 feature/async-assets). The disk queue's reader pool is sized live by
// asyncReadThreads (AsyncFileReadSetDiskThreads). After a read the thread runs the object's prepare callback, work that
// used to run in the post-load callback on the main thread. The poll event spends at most s_postloadBudget ms of a frame
// on post-load callbacks (stock 100 ms), and an object the main thread waits on is processed before the others.
static const uint32_t ASYNC_MAX_DISK_THREADS = 8;

static const char* s_diskThreadNames[ASYNC_MAX_DISK_THREADS] = {
    "Disk Queue", "Disk Queue 2", "Disk Queue 3", "Disk Queue 4", "Disk Queue 5", "Disk Queue 6", "Disk Queue 7", "Disk Queue 8"
};

static uint32_t s_diskThreadCount;
// asyncReadThreads is set (InitializeGlobal) before the engine context creates the queues; kept for
// AsyncFileReadInitialize. 0 = auto
static uint32_t s_diskThreadsWanted = 0;
static std::atomic<uint32_t> s_diskThreadsActive { 1 };
static uint32_t s_postloadBudget = 8;

// load statistics; the reader threads' parts are atomic
static AsyncLoadStats s_stats;
static uint64_t s_postTicks[ASYNC_KIND_COUNT];
static uint64_t s_waitTicks;
static std::atomic<uint64_t> s_readBytes;
static std::atomic<uint64_t> s_prepareTicks;

struct AsyncLoadWindow {
    uint64_t m_start;
    uint64_t m_readBytes;
    uint64_t m_prepareTicks;
    double m_postPeak;
    double m_waitPeak;
    uint32_t m_waits;
    uint32_t m_loads;
};

static AsyncLoadWindow s_window;
static double s_lastPostPeak;
static double s_lastWaitPeak;

// nanoseconds on the steady clock (QueryPerformanceCounter)
static uint64_t AsyncNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static const double ASYNC_MS_PER_TICK = 1.0e-6;

// once per poll event (once per frame): smooths the frame's post-load and wait time, rolls the 5 s window
static void AsyncLoadStatsFold() {
    const double k = 0.02;
    double frameMs = 0.0;

    for (uint32_t i = 0; i < ASYNC_KIND_COUNT; i++) {
        double ms = static_cast<double>(s_postTicks[i]) * ASYNC_MS_PER_TICK;
        s_stats.m_postMs[i] += (ms - s_stats.m_postMs[i]) * k;
        frameMs += ms;
        s_postTicks[i] = 0;
    }

    double waitMs = static_cast<double>(s_waitTicks) * ASYNC_MS_PER_TICK;
    s_stats.m_waitMs += (waitMs - s_stats.m_waitMs) * k;
    s_waitTicks = 0;

    if (frameMs > s_window.m_postPeak) {
        s_window.m_postPeak = frameMs;
    }

    uint64_t now = AsyncNow();

    if (!s_window.m_start) {
        s_window.m_start = now;
        s_window.m_readBytes = s_readBytes.load();
        s_window.m_prepareTicks = s_prepareTicks.load();
    }

    double elapsedMs = static_cast<double>(now - s_window.m_start) * ASYNC_MS_PER_TICK;

    if (elapsedMs >= 5000.0) {
        uint64_t bytes = s_readBytes.load();
        uint64_t prepare = s_prepareTicks.load();
        double seconds = elapsedMs / 1000.0;
        s_stats.m_readMBps = static_cast<double>(bytes - s_window.m_readBytes) / (1024.0 * 1024.0) / seconds;
        s_stats.m_prepareMs = static_cast<double>(prepare - s_window.m_prepareTicks) * ASYNC_MS_PER_TICK / seconds;
        s_stats.m_waits = s_window.m_waits;
        s_stats.m_loads = s_window.m_loads;
        s_lastPostPeak = s_window.m_postPeak;
        s_lastWaitPeak = s_window.m_waitPeak;

        s_window = AsyncLoadWindow();
        s_window.m_start = now;
        s_window.m_readBytes = bytes;
        s_window.m_prepareTicks = prepare;
    }

    s_stats.m_postPeak = s_window.m_postPeak > s_lastPostPeak ? s_window.m_postPeak : s_lastPostPeak;
    s_stats.m_waitPeak = s_window.m_waitPeak > s_lastWaitPeak ? s_window.m_waitPeak : s_lastWaitPeak;
}

const AsyncLoadStats& AsyncFileReadGetStats() {
    s_stats.m_queued = 0;
    s_stats.m_reading = 0;
    s_stats.m_posted = 0;
    s_stats.m_threads = s_diskThreadsActive.load();

    g_queueLock.Enter();

    for (CAsyncQueue* queue = s_asyncQueueList.Head(); !s_asyncQueueList.IsEnd(queue); queue = s_asyncQueueList.RawNext(queue)) {
        for (CAsyncObject* object = queue->m_readList.Head(); !queue->m_readList.IsEnd(object); object = queue->m_readList.RawNext(object)) {
            s_stats.m_queued++;
        }

        for (CAsyncObject* object = queue->m_staleList.Head(); !queue->m_staleList.IsEnd(object); object = queue->m_staleList.RawNext(object)) {
            s_stats.m_queued++;
        }
    }

    for (CAsyncThread* thread = s_asyncThreadList.Head(); !s_asyncThreadList.IsEnd(thread); thread = s_asyncThreadList.RawNext(thread)) {
        if (thread->m_currentObject) {
            s_stats.m_reading++;
        }
    }

    for (CAsyncObject* object = s_asyncFileReadPostList.Head(); !s_asyncFileReadPostList.IsEnd(object); object = s_asyncFileReadPostList.RawNext(object)) {
        s_stats.m_posted++;
    }

    for (CAsyncObject* object = s_asyncFileReadDeferList.Head(); !s_asyncFileReadDeferList.IsEnd(object); object = s_asyncFileReadDeferList.RawNext(object)) {
        s_stats.m_posted++;
    }

    g_queueLock.Leave();

    return s_stats;
}

void AsyncFileReadSetPostloadBudget(uint32_t ms) {
    s_postloadBudget = ms;
}
#endif

// VERIFY: ca9687a9 manual
// OFFSET: 0x004B9AB0
void AsyncFileReadUnloadPostList() {
    CAsyncObject* object;

    // Not stock: deferred textures (texUploadLimit) are unloaded with the rest.
    while ((object = s_asyncFileReadDeferList.Head()) != nullptr) {
        s_asyncFileReadPostList.LinkToTail(object);
    }

    while ((object = s_asyncFileReadPostList.Head()) != nullptr) {
        object->m_link.Unlink();

        if (object->m_userPostUnloadCallback) {
            reinterpret_cast<void (*)(void*)>(object->m_userPostUnloadCallback)(object->m_userArg);
        }
    }
}

// VERIFY: acd49bbd manual
// OFFSET: 0x004B9B20
// Not stock: the stock poll body, with a texture-creation cap when `capped` (the per-frame EVENT_ID_POLL path only;
// AsyncFileReadWait / WaitAll (loading screens) poll uncapped so they always drain). Runs post-load callbacks for up to
// `timeout` ms (at least one). x64: the object the main thread waits on goes first, and with `stopAfterWait` the pass
// ends once it has run (Badgermilk0 feature/async-assets).
static void AsyncFileReadPoll(bool capped, uint32_t timeout, int32_t stopAfterWait) {
    uint32_t start = OsGetAsyncTimeMs();
    int32_t texLimit = capped ? g_asyncTexUploadLimit : 0;
    int32_t texCount = 0;

    // Textures deferred on earlier frames go first, in their original order.
    g_queueLock.Enter();

    while (CAsyncObject* deferred = s_asyncFileReadDeferList.Tail()) {
        s_asyncFileReadPostList.LinkToHead(deferred);
    }

    g_queueLock.Leave();

    while (true) {
        g_queueLock.Enter();

        CAsyncObject* object = s_asyncFileReadPostList.Head();

#if defined(WOWDECOMP_ARCH_64)
        // m_isRead is set as the reader links the object to the post list and m_isProcessed as it leaves it
        CAsyncObject* waitObject = s_asyncWaitObject;

        if (waitObject && waitObject->m_isRead && !waitObject->m_isProcessed) {
            object = waitObject;
        }
#endif

        if (!object) {
            g_queueLock.Leave();
            break;
        }

        if (texLimit > 0 && object != s_asyncWaitObject && object->m_userPostloadCallback == reinterpret_cast<void*>(AsyncTextureSuccessCallback) && texCount++ >= texLimit) {
            s_asyncFileReadDeferList.LinkToTail(object);
            g_queueLock.Leave();
            continue;
        }

        object->m_link.Unlink();

        int32_t wasWaitObject = object == s_asyncWaitObject;

        if (wasWaitObject) {
            s_asyncWaitObject = nullptr;
        }

        object->m_isProcessed = 1;
#if defined(WOWDECOMP_ARCH_64)
        uint32_t kind = object->m_kind < ASYNC_KIND_COUNT ? object->m_kind : ASYNC_KIND_OTHER;
        uint64_t callbackStart = AsyncNow();
#endif

        // Not stock (worldLoadSort): loads issued by this postload (skins, textures, WMO groups) inherit its position.
        bool hasPos = object->m_hasLoadPos;
        float x = object->m_loadX;
        float y = object->m_loadY;
        g_queueLock.Leave();

        if (hasPos) {
            AsyncLoadOrigin origin(x, y);
            reinterpret_cast<void (*)(void*)>(object->m_userPostloadCallback)(object->m_userArg);
        } else {
            reinterpret_cast<void (*)(void*)>(object->m_userPostloadCallback)(object->m_userArg);
        }

#if defined(WOWDECOMP_ARCH_64)
        s_postTicks[kind] += AsyncNow() - callbackStart;
        s_window.m_loads++;
#endif

        s_progressCount--;

        if (wasWaitObject && stopAfterWait) {
            break;
        }

        if (OsGetAsyncTimeMs() - start > timeout) {
            break;
        }
    }

    for (uint32_t i = 0; i < s_asyncPollHandlers.Count(); i++) {
        s_asyncPollHandlers[i]();
    }
}

int32_t AsyncFileReadPollHandler(const void* data, void* param) {
    AsyncFileReadPoll(true, s_handlerTimeout, 0);
    return 1;
}

#if defined(WOWDECOMP_ARCH_64)
// MODIFIED: performance; the EVENT_ID_POLL handler: AsyncFileReadPollHandler within the frame budget
// (asyncPostloadBudget). Loading screens and the map's own load loops call AsyncFileReadPollHandler directly and keep
// the full asyncHandlerTimeout
static int32_t AsyncFileReadPollEvent(const void* data, void* param) {
    AsyncLoadStatsFold();
    AsyncFileReadPoll(true, s_postloadBudget ? s_postloadBudget : s_handlerTimeout, 0);
    return 1;
}
#endif

// VERIFY: 132a0c31 manual
// OFFSET: 0x004B9C60
void AsyncFileReadAddPollHandler(void (*handler)()) {
    for (uint32_t i = 0; i < s_asyncPollHandlers.Count(); i++) {
        if (s_asyncPollHandlers[i] == handler) {
            return;
        }
    }

    s_asyncPollHandlers.New(handler);
}

// VERIFY: e6dbc58a manual
// OFFSET: 0x004B9D20
void AsyncFileReadAddStatusHandler(int32_t (*handler)()) {
    for (uint32_t i = 0; i < s_asyncStatusHandlers.Count(); i++) {
        if (s_asyncStatusHandlers[i] == handler) {
            return;
        }
    }

    s_asyncStatusHandlers.New(handler);
}

// VERIFY: 75b60811 manual
// OFFSET: 0x004B9DE0
void AsyncFileReadDestroyObject(CAsyncObject* object) {
    g_queueLock.Enter();

    if (object->m_isCurrent) {
        DebugLog("AsyncFileReadDestroyObject()");
        g_queueLock.Leave();

        while (object->m_isCurrent) {
            OsSleep(1);
        }

        g_queueLock.Enter();
    }

    object->m_link.Unlink();

    if (object->m_file) {
        SFileCloseFile(object->m_file);
    }

    AsyncFileRead::s_asyncFileReadFreeList.LinkToHead(object);
    g_queueLock.Leave();
}

// VERIFY: 37b4ae03 auto
// OFFSET: 0x004B9E90
void DefaultAsyncObjectCleanupCallback(CAsyncObject* object) {
    AsyncFileReadDestroyObject(object);
}

// VERIFY: 3f12de16 manual
// OFFSET: 0x004B9EA0
int32_t AsyncFile::Cancel(CAsyncObject* object, void (*callback)(CAsyncObject*)) {
    g_queueLock.Enter();

    if (object->m_isCurrent) {
        object->m_userArg = object;
        object->m_userPostUnloadCallback = reinterpret_cast<void*>(callback ? callback : DefaultAsyncObjectCleanupCallback);
        object->m_userPostloadCallback = object->m_userPostUnloadCallback;
#if defined(WOWDECOMP_ARCH_64)
        // MODIFIED: a reader thread that has not started the prepare callback yet skips it
        object->m_prepareCallback = nullptr;
#endif
        g_queueLock.Leave();

        return 0;
    }

    SFileCloseFile(object->m_file);
    object->m_link.Unlink();
    AsyncFileRead::s_asyncFileReadFreeList.LinkToHead(object);
    g_queueLock.Leave();

    return 1;
}

// VERIFY: 8a117547 manual
// OFFSET: 0x004B9F40
int32_t NetQueuePromoteLocalFiles(const void* data, void* param) {
    // STATIC: 0x00B4A268
    static uint32_t s_lastPromoteTime;

    uint32_t time = OsGetAsyncTimeMs();

    if (time - s_lastPromoteTime >= 250) {
        s_lastPromoteTime = time;
        g_queueLock.Enter();

        for (int32_t i = 1; i < 3; i++) {
            CAsyncObject* object = s_asyncQueues[i]->m_readList.Head();

            while (!s_asyncQueues[i]->m_readList.IsEnd(object)) {
                CAsyncObject* next = s_asyncQueues[i]->m_readList.RawNext(object);

                if (SFile::FileIsLocal(object->m_file, 6) == 1) {
                    s_asyncQueues[0]->m_readList.LinkToTail(object);
                    object->m_queue = s_asyncQueues[0];
                    s_asyncQueues[0]->m_wakeEvent.Set();
                }

                object = next;
            }
        }

        g_queueLock.Leave();
    }

    return 1;
}

// VERIFY: d0d6eee1 manual
// OFFSET: 0x004BA060
void AsyncFileReadWait(CAsyncObject* object) {
    // STATIC: 0x00B4A26C
    static int32_t s_waiting;

    if (s_waiting) {
        SErrDisplayAppFatal("AsyncFileReadWait(): s_waiting != FALSE");
    }

    s_waiting++;
    g_queueLock.Enter();

    if (object->m_isProcessed) {
        g_queueLock.Leave();
        // MODIFIED: port fix; this return left s_waiting raised, so the next wait failed with the fatal error above
        s_waiting--;
        return;
    }

    s_asyncWaitObject = object;

    if (!object->m_isCurrent && !object->m_isRead) {
        object->m_link.Unlink();
        object->m_queue->m_readList.LinkToHead(object);
    }

    g_queueLock.Leave();

    if (SFile::IsStreamingMode()) {
        SFile::SetFilePriority(object->m_file, 1, 0);
    }

    char name[256];
    SFile::GetActualFileName(object->m_file, name, sizeof(name));
    DebugLog("AsyncFileReadWait():%s\n", name);

    if (s_ingameStartCallback) {
        s_ingameStartCallback();
    }

#if defined(WOWDECOMP_ARCH_64)
    uint64_t waitStart = AsyncNow();
#endif

    do {
        if (s_ingameProgressCallback) {
            s_ingameProgressCallback(0.0f, 0);
        }

#if defined(WOWDECOMP_ARCH_64)
        // MODIFIED: performance. The waited-on object runs as soon as it is read, and the wait returns right after it;
        // other read objects get the frame budget per pass instead of asyncHandlerTimeout (100 ms) before each check
        AsyncFileReadPoll(false, s_postloadBudget ? s_postloadBudget : s_handlerTimeout, 1);
#else
        AsyncFileReadPoll(false, s_handlerTimeout, 0);
#endif

        if (!s_asyncWaitObject) {
            break;
        }

        OsSleep(1);
    } while (true);

#if defined(WOWDECOMP_ARCH_64)
    uint64_t waitTicks = AsyncNow() - waitStart;
    double waitMs = static_cast<double>(waitTicks) * ASYNC_MS_PER_TICK;
    s_waitTicks += waitTicks;
    s_window.m_waits++;

    if (waitMs > s_window.m_waitPeak) {
        s_window.m_waitPeak = waitMs;
    }
#endif

    s_waiting--;
}

// VERIFY: 82068864 manual
// OFFSET: 0x004BA170
CAsyncObject* AsyncFileReadAllocObject() {
    g_queueLock.Enter();

    CAsyncObject* object = AsyncFileRead::s_asyncFileReadFreeList.Head();

    if (!object) {
        object = AsyncFileRead::s_asyncFileReadFreeList.NewNode(1, 0, 0);
    }

    object->m_link.Unlink();
    g_queueLock.Leave();

    object->m_file = nullptr;
    object->m_buffer = nullptr;
    object->m_size = 0;
    object->m_userArg = nullptr;
    object->m_userPostloadCallback = nullptr;
    object->m_userPostUnloadCallback = nullptr;
    object->m_queue = nullptr;
    object->m_isProcessed = 0;
    object->m_isRead = 0;
    object->m_isCurrent = 0;
    object->m_char24 = 0;
    object->m_char25 = 0;
    object->m_queuedFrame = 0;
    object->m_priority = 126;
    object->m_hasLoadPos = g_asyncLoadOriginSet;
    object->m_loadX = g_asyncLoadOrigin[0];
    object->m_loadY = g_asyncLoadOrigin[1];
    object->m_linkFrame = 0;
#if defined(WOWDECOMP_ARCH_64)
    object->m_prepareCallback = nullptr;
    object->m_prepared = 0;
    object->m_kind = ASYNC_KIND_OTHER;
#endif

    return object;
}

// VERIFY: d372c124 manual
// OFFSET: 0x004BA310
CAsyncQueue::~CAsyncQueue() {
}

// VERIFY: 69785110 manual
// OFFSET: 0x004BA3D0
void AsyncFileReadLinkObject(CAsyncObject* object, int32_t urgent) {
    if (!object->m_queue) {
        return;
    }

    object->m_link.Unlink();

    for (CAsyncObject* other = object->m_queue->m_readList.Head(); !object->m_queue->m_readList.IsEnd(other); other = object->m_queue->m_readList.RawNext(other)) {
        if (object->m_priority <= other->m_priority && (urgent || object->m_priority != other->m_priority)) {
            object->m_queue->m_readList.LinkNode(object, 2, other);
            object->m_char25 = 0;
            return;
        }
    }

    object->m_queue->m_readList.LinkToTail(object);
    object->m_char25 = 0;
}

// VERIFY: c818c689 manual
// OFFSET: 0x004BA530
void AsyncFileReadLinkStaleObject(CAsyncObject* object, int32_t urgent) {
    object->m_link.Unlink();

    for (CAsyncObject* other = object->m_queue->m_staleList.Head(); !object->m_queue->m_staleList.IsEnd(other); other = object->m_queue->m_staleList.RawNext(other)) {
        if (object->m_priority <= other->m_priority && (urgent || object->m_priority != other->m_priority)) {
            object->m_queue->m_staleList.LinkNode(object, 2, other);
            object->m_char25 = 1;
            return;
        }
    }

    object->m_queue->m_staleList.LinkToTail(object);
    object->m_char25 = 1;
}

// VERIFY: 28f905c7 manual
// OFFSET: 0x004BA680
uint32_t STORMAPI AsyncFileReadThread(void* param) {
    CAsyncThread* thread = static_cast<CAsyncThread*>(param);
    CAsyncQueue* queue = thread->m_queue;
    PropSelectContext(s_propContext);

    while (AsyncFileRead::s_shutdownEvent.Wait(0)) {
        uint32_t count = 0;

#if defined(WOWDECOMP_ARCH_64)
        // MODIFIED: performance; a disk thread past the active count (asyncReadThreads lowered) idles. It never waits on
        // the wake event, so it never swallows a wake meant for an active reader
        if (queue == s_asyncQueues[0] && thread->m_index >= s_diskThreadsActive.load(std::memory_order_relaxed)) {
            OsSleep(5);
            continue;
        }
#endif

        while (true) {
            g_queueLock.Enter();

            // Not stock: stop draining the queue once shutdown is signalled, so the quit join is not held up by every
            // queued read (the leftovers are dropped with the queues in AsyncFileReadDestroy).
            if (!AsyncFileRead::s_shutdownEvent.Wait(0)) {
                break;
            }

            CAsyncObject* object = queue->m_readList.Head();

            if (object) {
                if (queue->m_frameLimited && g_theGxDevicePtr->m_frameCount - object->m_queuedFrame > 3) {
                    AsyncFileReadLinkStaleObject(object, 1);
                    g_queueLock.Leave();
                    continue;
                }

                if (!queue->m_frameLimited) {
                    object = AsyncPickNearest(queue, object);
                }
            } else {
                object = queue->m_staleList.Head();

                if (!object) {
                    break;
                }
            }

            object->m_link.Unlink();

            // Not stock: pass the wake on so a sibling reader on this queue takes the next request in parallel.
            if (queue->m_readList.Head() || queue->m_staleList.Head()) {
                queue->m_wakeEvent.Set();
            }

            object->m_queue = nullptr;
            object->m_isCurrent = 1;
            thread->m_currentObject = object;
            g_queueLock.Leave();

#if defined(WOWDECOMP_ARCH_64)
            int32_t readOk = 0;
#endif

            for (int32_t retries = 10; retries; ) {
                if (SFile::IsStreamingMode() && object->m_file->m_type) {
                    SFile::MakeFileLocal(object->m_file, (object->m_priority > 127) + 1, true);
                }

                if (SFileReadFile(object->m_file, object->m_buffer, object->m_size, nullptr, nullptr, 0)) {
#if defined(WOWDECOMP_ARCH_64)
                    readOk = 1;
#endif
                    break;
                }

                if (!--retries) {
                    char name[512];
                    SFile::GetActualFileName(object->m_file, name, sizeof(name));
                    char error[512];
                    SErrGetErrorStr(SErrGetLastError(), error, sizeof(error));
                    DebugLog("AsyncFileReadThread() failed reading %s: %s", name, error);
                }
            }

#if defined(WOWDECOMP_ARCH_64)
            // MODIFIED: performance; the prepare callback runs here, off the main thread (see CAsyncObject). A Cancel
            // during the read clears it; one during the callback only swaps the post-load callback for a cleanup
            if (readOk) {
                s_readBytes.fetch_add(object->m_size, std::memory_order_relaxed);

                g_queueLock.Enter();
                void (*prepare)(CAsyncObject*) = object->m_prepareCallback;
                g_queueLock.Leave();

                if (prepare) {
                    uint64_t prepareStart = AsyncNow();
                    prepare(object);
                    s_prepareTicks.fetch_add(AsyncNow() - prepareStart, std::memory_order_relaxed);
                }
            }
#endif

            g_queueLock.Enter();
            s_asyncFileReadPostList.LinkToTail(object);
            thread->m_currentObject = nullptr;
            object->m_isCurrent = 0;
            object->m_isRead = 1;
            g_queueLock.Leave();

            if (s_threadSleep && ++count == s_threadSleep) {
                OsSleep(1);
                count = 0;
            }
        }

        g_queueLock.Leave();

        // Not stock (stock: OsSleep(1) poll): sleep until a request is queued. The timeout is a safety net for
        // anything that queues without signalling, and bounds how long shutdown waits to be noticed.
        queue->m_wakeEvent.Wait(10);
    }

    return 0;
}

// VERIFY: 1d3b9267 manual
// OFFSET: 0x004BA8E0
CAsyncQueue* AsyncFileReadCreateQueue() {
    return s_asyncQueueList.NewNode(2, 0, 0);
}

// VERIFY: 3de6d82d auto
// OFFSET: 0x004BA980
void AsyncFileReadCreateThread(CAsyncQueue* queue, const char* name) {
    CAsyncThread* thread = s_asyncThreadList.NewNode(2, 0, 0);
    thread->m_queue = queue;
    thread->m_currentObject = nullptr;
#if defined(WOWDECOMP_ARCH_64)
    thread->m_index = 0;
#endif
    SThread::Create(AsyncFileReadThread, thread, thread->m_thread, const_cast<char*>(name), 0);
}

// VERIFY: e94485f6 auto
// OFFSET: 0x004BAA40
void AsyncFileReadInitialize(uint32_t threadSleep, uint32_t handlerTimeout) {
    s_threadSleep = threadSleep;
    s_handlerTimeout = handlerTimeout;

    if (threadSleep > 100) {
        s_threadSleep = 100;
    }

    if (handlerTimeout < 20) {
        s_handlerTimeout = 20;
    }

#if defined(WOWDECOMP_ARCH_64)
    // MODIFIED: performance; the frame's post-load pass keeps to s_postloadBudget (AsyncFileReadPollEvent)
    EventRegisterEx(EVENT_ID_POLL, AsyncFileReadPollEvent, nullptr, EVENT_PRIORITY_NORMAL);
#else
    EventRegisterEx(EVENT_ID_POLL, AsyncFileReadPollHandler, nullptr, EVENT_PRIORITY_NORMAL);
#endif

    if (SFile::IsStreamingMode()) {
        EventRegisterEx(EVENT_ID_IDLE, NetQueuePromoteLocalFiles, nullptr, EVENT_PRIORITY_BELOW_NORMAL);
    }

    s_asyncWaitObject = nullptr;
    s_asyncProgressCallback = nullptr;
    s_asyncProgressCallbackParams = nullptr;
    s_ingameProgressCallback = nullptr;
    s_ingameStartCallback = nullptr;
    s_propContext = PropGetSelectedContext();
    AsyncFileRead::s_shutdownEvent.Reset();

    int32_t queues = SFile::IsStreamingMode() ? 3 : 1;

    // Not stock: the disk queue gets clamp(cores / 2, 2, 4) readers instead of one (x64: asyncReadThreads,
    // AsyncFileReadSetDiskThreads). Archive reads are positional (ReadFile + OVERLAPPED offset) and SFile locks per
    // handle, so readers on different requests do not serialise.
    uint32_t diskThreads = AsyncFileReadAutoDiskThreads();

    for (int32_t i = 0; i < queues; i++) {
        s_asyncQueues[i] = AsyncFileReadCreateQueue();

#if defined(WOWDECOMP_ARCH_64)
        if (i == 0) {
            continue;
        }
#endif

        for (uint32_t t = 0; t < (i == 0 ? diskThreads : 1); t++) {
            AsyncFileReadCreateThread(s_asyncQueues[i], s_asyncQueueNames[i]);
        }
    }

    if (SFile::IsStreamingMode()) {
        s_asyncQueues[2]->m_frameLimited = 1;
    }

#if defined(WOWDECOMP_ARCH_64)
    s_diskThreadCount = 0;
    AsyncFileReadSetDiskThreads(s_diskThreadsWanted);
#endif

    s_queueLocked = 0;
}

#if defined(WOWDECOMP_ARCH_64)
// MODIFIED: performance; see s_diskThreadNames. 0 = auto (clamp(cores / 2, 2, 4)). Threads are created up to the
// largest count asked for and never destroyed before AsyncFileReadDestroy; lowering the count idles the surplus ones
void AsyncFileReadSetDiskThreads(uint32_t count) {
    s_diskThreadsWanted = count;

    if (!s_asyncQueues[0]) {
        return;
    }

    if (!count) {
        count = AsyncFileReadAutoDiskThreads();
    } else if (count > ASYNC_MAX_DISK_THREADS) {
        count = ASYNC_MAX_DISK_THREADS;
    }

    while (s_diskThreadCount < count) {
        CAsyncThread* thread = s_asyncThreadList.NewNode(2, 0, 0);
        thread->m_queue = s_asyncQueues[0];
        thread->m_currentObject = nullptr;
        thread->m_index = s_diskThreadCount;
        SThread::Create(AsyncFileReadThread, thread, thread->m_thread, const_cast<char*>(s_diskThreadNames[s_diskThreadCount]), 0);
        s_diskThreadCount++;
    }

    s_diskThreadsActive = count;
    s_asyncQueues[0]->m_wakeEvent.Set();
}
#endif

// VERIFY: 35423985 auto
// OFFSET: 0x004BAB50
void AsyncFileReadObject(CAsyncObject* object, int32_t urgent) {
    CAsyncQueue* queue = s_asyncQueues[0];

    if (SFile::IsStreamingMode()) {
        int32_t local = SFile::FileIsLocal(object->m_file, 6);

        if (!local || local == 2) {
            object->m_char24 = 1;
            queue = object->m_priority > 127 ? s_asyncQueues[2] : s_asyncQueues[1];
        }
    }

    g_queueLock.Enter();
    object->m_queue = queue;
    object->m_linkFrame = g_theGxDevicePtr->m_frameCount;

    if (object == s_asyncWaitObject) {
        object->m_priority = object->m_priority > 127 ? 128 : 0;
        object->m_queuedFrame = g_theGxDevicePtr->m_frameCount;
        queue->m_readList.LinkToHead(object);
        object->m_char25 = 0;
    } else if (queue->m_frameLimited) {
        AsyncFileReadLinkStaleObject(object, urgent);
    } else {
        AsyncFileReadLinkObject(object, urgent);
    }

    queue->m_wakeEvent.Set();
    g_queueLock.Leave();

    if (SFile::IsStreamingMode()) {
        SFile::SetFilePriority(object->m_file, 0, 0);
    }
}

// VERIFY: 2f496060 auto
// OFFSET: 0x004BAC20
void AsyncFileReadIncreasePriority(CAsyncObject* object) {
    object->m_queuedFrame = g_theGxDevicePtr->m_frameCount;

    // Not stock: relink under the queue lock. Stock had one reader thread; several readers popping the read list
    // while this relinks would corrupt it.
    g_queueLock.Enter();

    if (object->m_char25) {
        AsyncFileReadLinkObject(object, 1);
    }

    g_queueLock.Leave();
}

// VERIFY: 3aa538d7 manual
// OFFSET: 0x004BAC50
void AsyncFileReadDestroy() {
    for (CAsyncThread* thread = s_asyncThreadList.Head(); !s_asyncThreadList.IsEnd(thread); thread = s_asyncThreadList.RawNext(thread)) {
        AsyncFileRead::s_shutdownEvent.Set();
        GxTeardownLog("AsyncFileReadDestroy: join reader thread");

        // Not stock (stock waits INFINITE): a reader stuck in a file read must not hang the quit. Its queues and
        // objects are left alone (leaked) so it never touches freed memory; the process is exiting anyway.
        if (thread->m_thread.Wait(2000)) {
            GxTeardownLog("AsyncFileReadDestroy: reader thread did not exit in 2 s, skipping the rest");
            return;
        }
    }

    GxTeardownLog("AsyncFileReadDestroy: readers joined");

    AsyncFileReadUnloadPostList();
    AsyncFileRead::s_asyncFileReadFreeList.Clear();

    for (CAsyncQueue* queue = s_asyncQueueList.Head(); !s_asyncQueueList.IsEnd(queue); queue = s_asyncQueueList.RawNext(queue)) {
    }

    s_asyncQueueList.Clear();
    s_asyncQueues[0] = nullptr;
    s_asyncQueues[1] = nullptr;
    s_asyncQueues[2] = nullptr;

    for (CAsyncThread* thread = s_asyncThreadList.Head(); !s_asyncThreadList.IsEnd(thread); thread = s_asyncThreadList.RawNext(thread)) {
    }

    s_asyncThreadList.Clear();
    s_asyncPollHandlers.SetCount(0);
    s_asyncStatusHandlers.SetCount(0);
    s_asyncProgressCallback = nullptr;
    s_asyncProgressCallbackParams = nullptr;
    s_ingameProgressCallback = nullptr;
    s_ingameStartCallback = nullptr;

    if (SFile::IsStreamingMode()) {
        EventUnregisterEx(EVENT_ID_IDLE, NetQueuePromoteLocalFiles, nullptr, 2);
    }

#if defined(WOWDECOMP_ARCH_64)
    EventUnregisterEx(EVENT_ID_POLL, AsyncFileReadPollEvent, nullptr, 0xFFFFFFFF);
    s_diskThreadCount = 0;
#else
    EventUnregisterEx(EVENT_ID_POLL, AsyncFileReadPollHandler, nullptr, 0xFFFFFFFF);
#endif
}

// VERIFY: e7566958 auto
// OFFSET: 0x004BAD80
bool AsyncFileReadIsReading() {
    g_queueLock.Enter();

    bool reading = false;

    for (CAsyncThread* thread = s_asyncThreadList.Head(); !s_asyncThreadList.IsEnd(thread); thread = s_asyncThreadList.RawNext(thread)) {
        if (thread->m_currentObject) {
            reading = true;
        }
    }

    for (CAsyncQueue* queue = s_asyncQueueList.Head(); !s_asyncQueueList.IsEnd(queue); queue = s_asyncQueueList.RawNext(queue)) {
        if (queue->m_readList.Head()) {
            reading = true;
        }
    }

    if (s_asyncFileReadPostList.Head() || s_asyncFileReadDeferList.Head()) {
        reading = true;
    }

    g_queueLock.Leave();

    return reading;
}

// VERIFY: 3e612f72 auto
// OFFSET: 0x004BAE10
void AsyncFileReadWaitAll() {
    DebugLog("AsyncFileReadWaitAll() Enter\n");

    int32_t total = 0;

    for (uint32_t i = 0; i < s_asyncStatusHandlers.Count(); i++) {
        total += s_asyncStatusHandlers[i]();
    }

    s_progressCount = total;

    while (AsyncFileReadIsReading()) {
        AsyncFileReadPoll(false, s_handlerTimeout, 0);

        if (s_asyncProgressCallback) {
            float progress = 1.0f;

            if (total) {
                progress = static_cast<float>(total - s_progressCount) / total;

                if (progress < 0.0f) {
                    progress = 0.0f;
                } else if (progress > 1.0f) {
                    progress = 1.0f;
                }
            }

            s_asyncProgressCallback(progress, s_asyncProgressCallbackParams);
        }

        OsSleep(1);
    }

    s_asyncProgressCallback = nullptr;
    DebugLog("AsyncFileReadWaitAll() Leave\n");
}
