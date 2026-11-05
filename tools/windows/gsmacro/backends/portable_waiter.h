// std::condition_variable waiter with a spin tail. Used on non-Windows hosts
// (demo, tests). The Windows build uses WinWaiter (high-resolution waitable
// timer) instead.
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "../core/backend.h"

namespace gsm {

class PortableWaiter final : public IWaiter {
public:
    bool waitUntil(Time deadline, Time spinTail) override {
        SteadyClock clock;
        {
            std::unique_lock<std::mutex> lk(m_);
            if (deadline == kNever) {
                cv_.wait(lk, [&] { return woken_.load(); });
            } else {
                Time sleepUntil = deadline - spinTail;
                if (sleepUntil > clock.now()) {
                    auto tp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(sleepUntil));
                    cv_.wait_until(lk, tp, [&] { return woken_.load(); });
                }
            }
            if (woken_.exchange(false)) return true;
        }
        while (clock.now() < deadline) {
            if (woken_.exchange(false)) return true;
        }
        return false;
    }
    void wake() override {
        {
            std::lock_guard<std::mutex> l(m_);
            woken_ = true;
        }
        cv_.notify_one();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> woken_{false};
};

}  // namespace gsm
