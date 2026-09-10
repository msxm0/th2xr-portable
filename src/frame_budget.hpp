#pragma once

#include <chrono>

namespace th2 {

// One frame's allowance for work nobody is waiting on.
//
// Prefetching, audio decoding and image decoding each used to carry a private
// budget, so none of them knew what the others had already spent: three
// subsystems politely staying under two milliseconds apiece still hand the
// frame six.  They share this instead - the frame gets one allowance, and
// whoever asks first spends it.
//
// Work the player is actually waiting on does not come through here.  A read
// the script has blocked on happens whatever the clock says; this governs
// only what is being done early.
class FrameBudget {
public:
    explicit FrameBudget(std::chrono::nanoseconds allowance)
        : allowance_(allowance)
    {
    }

    // At the top of a frame.  Nothing is carried forward: a quiet frame does
    // not entitle the next one to overrun.
    void begin_frame() { spent_ = std::chrono::nanoseconds::zero(); }

    std::chrono::nanoseconds remaining() const
    {
        return allowance_ > spent_
            ? allowance_ - spent_
            : std::chrono::nanoseconds::zero();
    }

    bool exhausted() const
    {
        return remaining() <= std::chrono::nanoseconds::zero();
    }

    // Times a piece of background work and charges it to the frame.
    template <typename Work>
    auto spend(Work&& work) -> decltype(work())
    {
        struct Charge {
            FrameBudget& budget;
            std::chrono::steady_clock::time_point started;
            ~Charge()
            {
                budget.spent_ += std::chrono::steady_clock::now() - started;
            }
        } charge{*this, std::chrono::steady_clock::now()};
        return work();
    }

    std::chrono::nanoseconds spent() const { return spent_; }
    std::chrono::nanoseconds allowance() const { return allowance_; }

private:
    std::chrono::nanoseconds allowance_;
    std::chrono::nanoseconds spent_{};
};

}  // namespace th2
