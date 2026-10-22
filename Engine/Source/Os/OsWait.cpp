#include "Engine/Source/Os/TimeManager.h"
#include <windows.h>
#include <timeapi.h>

// CUSTOM (Badgermilk0 port). Precise waits for the frame cap. Sleep() rounds up to the system timer period (15.6 ms unless
// raised), so the stock millisecond frame cap overslept and stuttered. The process asks for a 1 ms timer period at
// startup (OsTimerResolutionBegin), and OsWaitUntilClocks sleeps on a high-resolution waitable timer where Windows has
// one (10 1803 and later), finishing the last fraction of a millisecond by yielding.

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static HANDLE s_waitTimer;
static bool s_waitTimerTried;
static bool s_timerResolutionRaised;

void OsTimerResolutionBegin() {
    if (!s_timerResolutionRaised && timeBeginPeriod(1) == TIMERR_NOERROR) {
        s_timerResolutionRaised = true;
    }
}

void OsTimerResolutionEnd() {
    if (s_timerResolutionRaised) {
        timeEndPeriod(1);
        s_timerResolutionRaised = false;
    }

    if (s_waitTimer) {
        CloseHandle(s_waitTimer);
        s_waitTimer = nullptr;
    }
}

static HANDLE WaitTimer() {
    if (!s_waitTimerTried) {
        s_waitTimerTried = true;
        s_waitTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }

    return s_waitTimer;
}

void OsWaitUntilClocks(uint64_t deadline) {
    int64_t frequency = OsGetAsyncClocksPerSecond();

    if (frequency <= 0 || OsFastSleepActive()) {
        return;
    }

    // yield through the last half millisecond: a timer can wake a little late, a yield costs no more than it waits
    int64_t spin = frequency / 2000;

    for (;;) {
        int64_t remaining = static_cast<int64_t>(deadline - OsGetAsyncTimeClocks());

        if (remaining <= 0) {
            return;
        }

        if (remaining <= spin) {
            SwitchToThread();
            continue;
        }

        int64_t sleepClocks = remaining - spin;
        HANDLE timer = WaitTimer();

        if (timer) {
            // relative, in 100 ns units
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<int64_t>(static_cast<double>(sleepClocks) * 10000000.0 / static_cast<double>(frequency));

            if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                continue;
            }
        }

        // whole milliseconds, one short so the timer period cannot overshoot the deadline
        DWORD milliseconds = static_cast<DWORD>(sleepClocks * 1000 / frequency);
        Sleep(milliseconds > 1 ? milliseconds - 1 : 0);
    }
}
