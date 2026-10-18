#ifndef ENGINE_SOURCE_OS_TIMEMANAGER_H
#define ENGINE_SOURCE_OS_TIMEMANAGER_H

#include <cstdint>

class OsTimeManager {
    public:
    enum TimingMethod {
        NotSet = -1,
        BestAvailable = 0,
        SystemMethod1 = 1,
        SystemMethod2 = 2,
    };

    /* 0000 */ double m_scaleToMs;
    /* 0008 */ OsTimeManager::TimingMethod m_timingMethod;
    /* 000C */ uint32_t m_timingTestError;
    /* 0010 */ int64_t m_performanceFrequency;
    /* 0018 */ double m_timeBegin;

    OsTimeManager(OsTimeManager::TimingMethod method);
    OsTimeManager::TimingMethod Calibrate();
    uint64_t Snapshot();
};

uint32_t OsTimeGetTestError();
OsTimeManager::TimingMethod OsTimeGetTimingMethod();
const char* GetTimingMethodName(OsTimeManager::TimingMethod method);
uint32_t OsGetAsyncTimeMs();
uint64_t OsGetAsyncTimeClocks();
int64_t OsGetAsyncClocksPerSecond();
void OsTimeManagerCreate(OsTimeManager::TimingMethod method);
void OsTimeManagerDestroy();
void OsSleep(uint32_t milliseconds);
// BACKLOG F5: set by the loading screen while CVar fastLoad is on; OsSleep then returns at once (60 s cap).
void OsSetFastSleep(bool on);

#endif
