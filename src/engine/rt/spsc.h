// A fixed-size single-producer single-consumer ring of trivially copyable items: one thread pushes,
// one other thread pops, neither ever waits or allocates (docs/architecture.md, *Threads*).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace rack
{

template <class T, size_t N> class SpscRing
{
    static_assert(std::is_trivially_copyable_v<T>, "the ring copies items as bytes would");
    static_assert(N >= 2 && (N & (N - 1)) == 0, "a power of two");

public:
    // Producer only. False (and nothing written) when full.
    bool push(const T &v)
    {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) == N)
            return false;
        items_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    // Producer only: is there no room? (Exact for the producer: only the consumer makes room.)
    bool full() const
    {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_acquire) == N;
    }
    // Consumer only. False when empty.
    bool pop(T &out)
    {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire))
            return false;
        out = items_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, N>    items_ { };
    std::atomic<size_t> head_ { 0 }, tail_ { 0 };
};

} // namespace rack
