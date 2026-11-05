// Windows waiter: high-resolution waitable timer (Windows 10 1803+) plus a
// wake event, with a short spin before macro deadlines. The engine thread
// joins the MMCSS "Games" class for scheduling priority.
//
// UNTESTED ON THE TARGET PC: compiles; timing must be checked with gsm_bench.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

#include "../../core/backend.h"

namespace gsm {

class WinWaiter final : public IWaiter {
public:
    WinWaiter();
    ~WinWaiter() override;
    bool waitUntil(Time deadline, Time spinTail) override;
    void wake() override;
    void onThreadStart() override;
    bool highResolution() const { return highRes_; }

private:
    HANDLE timer_ = nullptr;
    HANDLE event_ = nullptr;  // auto-reset
    std::atomic<bool> flag_{false};
    bool highRes_ = false;
    HANDLE mmcss_ = nullptr;
};

}  // namespace gsm
