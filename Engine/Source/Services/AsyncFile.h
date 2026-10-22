#ifndef ENGINE_SOURCE_SERVICES_ASYNCFILE_H
#define ENGINE_SOURCE_SERVICES_ASYNCFILE_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Services/Types.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Storm/Source/SLock.h"
#include "Engine/Source/Storm/Source/SThread.h"

class CAsyncQueue : public TSLinkedNode<CAsyncQueue> {
    public:
    /* 0008 */ TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> m_readList;
    /* 0014 */ TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> m_staleList;
    /* 0020 */ int32_t m_frameLimited;
    // Not stock: auto-reset wake for the queue's reader threads (stock polled with OsSleep(1)).
    SEvent m_wakeEvent{0, 0};

    ~CAsyncQueue();
};

class CAsyncThread : public TSLinkedNode<CAsyncThread> {
    public:
    /* 0008 */ SThread m_thread;
    /* 000C */ CAsyncQueue* m_queue;
    /* 0010 */ CAsyncObject* m_currentObject;
#if defined(WOWDECOMP_ARCH_64)
    // MODIFIED: the thread's place in its queue's pool; a disk thread at or past the active count idles
    uint32_t m_index;
#endif
};

#if defined(WOWDECOMP_ARCH_64)
// MODIFIED: performance diagnostics. Where loading costs the main thread, smoothed per frame like GetSceneTimings
// (GetLoadTimings() in Lua). Window values cover the last complete 5 s window; peaks the current and last window.
struct AsyncLoadStats {
    double m_postMs[ASYNC_KIND_COUNT]; // main-thread ms per frame in post-load callbacks, by ASYNC_KIND_*
    double m_postPeak;                 // the most post-load ms any one frame spent
    double m_waitMs;                   // main-thread ms per frame blocked in AsyncFileReadWait
    double m_waitPeak;                 // the longest single AsyncFileReadWait, ms
    double m_prepareMs;                // reader-thread ms per second in prepare callbacks (off the main thread)
    double m_readMBps;                 // MB per second the reader threads read
    uint32_t m_waits;                  // AsyncFileReadWait calls per window
    uint32_t m_loads;                  // post-load callbacks per window
    uint32_t m_queued;                 // objects waiting for a reader thread (stale lists included)
    uint32_t m_reading;                // reader threads reading now
    uint32_t m_posted;                 // read objects waiting for their post-load callback
    uint32_t m_threads;                // active disk reader threads
};

// the counts (queued, reading, posted, threads) are taken at the call; the rest is kept current by the poll event
const AsyncLoadStats& AsyncFileReadGetStats();
// disk reader threads (stock 1, at most 8); more are created as needed, surplus ones idle
void AsyncFileReadSetDiskThreads(uint32_t count);
// main-thread ms per frame for post-load callbacks outside loading screens; 0 = asyncHandlerTimeout (stock 100)
void AsyncFileReadSetPostloadBudget(uint32_t ms);
#endif

class AsyncFile {
    public:
    static int32_t Cancel(CAsyncObject* object, void (*callback)(CAsyncObject*));
    static void EnterQueueLock();
    static void LeaveQueueLock();
};

void AsyncFileReadAddPollHandler(void (*handler)());
void AsyncFileReadAddStatusHandler(int32_t (*handler)());
CAsyncObject* AsyncFileReadAllocObject();
CAsyncQueue* AsyncFileReadCreateQueue();
void AsyncFileReadCreateThread(CAsyncQueue* queue, const char* name);
void AsyncFileReadDestroy();
void AsyncFileReadDestroyObject(CAsyncObject* object);
void AsyncFileReadIncreasePriority(CAsyncObject* object);
void AsyncFileReadIngameProgressCallback(void (*startCallback)(), void (*progressCallback)(float, int32_t));
void AsyncFileReadInitialize(uint32_t threadSleep, uint32_t handlerTimeout);
bool AsyncFileReadIsReading();
void AsyncFileReadLinkObject(CAsyncObject* object, int32_t urgent);
void AsyncFileReadLinkStaleObject(CAsyncObject* object, int32_t urgent);
void AsyncFileReadObject(CAsyncObject* object, int32_t urgent);
int32_t AsyncFileReadPollHandler(const void* data, void* param);
void AsyncFileReadSetProgressCallback(void (*callback)(float, void*), void* param);
uint32_t STORMAPI AsyncFileReadThread(void* param);
void AsyncFileReadUnloadPostList();
void AsyncFileReadWait(CAsyncObject* object);
void AsyncFileReadWaitAll();
// Not stock (worldLoadSort / texUploadLimit).
extern int32_t g_asyncLoadSort;
extern int32_t g_asyncTexUploadLimit;
void AsyncFileReadSetViewPos(float x, float y);

// Main thread: objects allocated while one is alive are tagged with this world x/y (nests, restores the outer one).
extern bool g_asyncLoadOriginSet;
extern float g_asyncLoadOrigin[2];

struct AsyncLoadOrigin {
    bool m_set = g_asyncLoadOriginSet;
    float m_x = g_asyncLoadOrigin[0];
    float m_y = g_asyncLoadOrigin[1];

    AsyncLoadOrigin(float x, float y) {
        g_asyncLoadOriginSet = true;
        g_asyncLoadOrigin[0] = x;
        g_asyncLoadOrigin[1] = y;
    }

    ~AsyncLoadOrigin() {
        g_asyncLoadOriginSet = this->m_set;
        g_asyncLoadOrigin[0] = this->m_x;
        g_asyncLoadOrigin[1] = this->m_y;
    }
};

void DefaultAsyncObjectCleanupCallback(CAsyncObject* object);
int32_t NetQueuePromoteLocalFiles(const void* data, void* param);

#endif
