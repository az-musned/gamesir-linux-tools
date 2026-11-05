#include "win_waiter.h"

#include <avrt.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace gsm {

WinWaiter::WinWaiter() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    highRes_ = timer_ != nullptr;
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // pre-1803 fallback
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

WinWaiter::~WinWaiter() {
    if (timer_) CloseHandle(timer_);
    if (event_) CloseHandle(event_);
}

void WinWaiter::onThreadStart() {
    DWORD task = 0;
    mmcss_ = AvSetMmThreadCharacteristicsW(L"Games", &task);
    if (!mmcss_) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

void WinWaiter::wake() {
    flag_.store(true, std::memory_order_release);
    SetEvent(event_);
}

bool WinWaiter::waitUntil(Time deadline, Time spinTail) {
    SteadyClock clock;
    if (flag_.exchange(false)) return true;
    if (deadline == kNever) {
        WaitForSingleObject(event_, INFINITE);
        flag_.store(false);
        return true;
    }
    Time sleepUntil = deadline - spinTail;
    Time rem = sleepUntil - clock.now();
    if (rem > 0) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(rem / 100);  // relative, 100 ns units
        if (due.QuadPart < 0 && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
            HANDLE h[2] = {event_, timer_};
            DWORD r = WaitForMultipleObjects(2, h, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) {
                CancelWaitableTimer(timer_);
                flag_.store(false);
                return true;
            }
        } else {
            // Could not arm the timer: block on the event with a ms timeout.
            DWORD msWait = (DWORD)(rem / 1'000'000);
            if (WaitForSingleObject(event_, msWait) == WAIT_OBJECT_0) { flag_.store(false); return true; }
        }
    }
    while (clock.now() < deadline) {
        if (flag_.exchange(false)) return true;
        YieldProcessor();
    }
    return false;
}

}  // namespace gsm
