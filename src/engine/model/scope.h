// What an oscilloscope module shows: one window of its two inputs, as the lowest and the highest
// value in each of kScopeColumns columns (so a fast wave still shows its full swing in a long
// window). The module (audio thread) hands each finished window to the window (UI thread) through a
// triple buffer: one writer, one reader, neither ever waits, and the reader always gets the newest
// whole frame.
#pragma once

#include <atomic>
#include <cstdint>

namespace rack
{

inline constexpr int kScopeColumns = 240;

struct ScopeFrame
{
    uint32_t seq = 0;           // frames so far; 0: none yet
    float    window_ms = 0;     // how much time the columns cover
    bool     has[2] { };        // which inputs are connected
    bool     triggered = false; // false: drawn by the auto trigger (no edge came in time)
    float lo[2][kScopeColumns] { }, hi[2][kScopeColumns] { }; // scaled: +-1 is the display's edge
};

class ScopeTriple
{
public:
    // Puts the buffer in its starting state. (Module states are zeroed memory, not constructed.)
    void init()
    {
        state_.store(1, std::memory_order_relaxed);
        w_ = 0;
        r_ = 2;
    }
    // ---- the writer (the audio thread): fill back(), then publish() it
    ScopeFrame &back() { return slot_[w_]; }
    void publish() { w_ = state_.exchange((uint8_t)(w_ | kNew), std::memory_order_acq_rel) & 3; }
    // ---- the reader (the UI thread): the newest frame, if one came since the last take
    bool take(ScopeFrame &out)
    {
        if (!(state_.load(std::memory_order_acquire) & kNew))
            return false;
        r_ = state_.exchange(r_, std::memory_order_acq_rel) & 3;
        out = slot_[r_];
        return true;
    }

private:
    static constexpr uint8_t kNew = 4;
    ScopeFrame               slot_[3];
    std::atomic<uint8_t>     state_ { 1 };   // the slot between the two, and whether it is new
    uint8_t                  w_ = 0, r_ = 2; // the writer's and the reader's own slots
};

} // namespace rack
