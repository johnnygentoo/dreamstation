// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dreamstation/common/types.hpp>
#include <array>
#include <atomic>

namespace ds::common {

// Lock-free SPSC ring buffer. N must be a power of two; T must be trivially copyable.
template<typename T, ds::usize N>
class Fifo {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
public:
    bool push(T val) {
        const auto head = m_head.load(std::memory_order_relaxed);
        const auto next = (head + 1u) & (N - 1u);
        if (next == m_tail.load(std::memory_order_acquire)) return false;
        m_buf[head] = val;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return false;
        out = m_buf[tail];
        m_tail.store((tail + 1u) & (N - 1u), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return m_head.load(std::memory_order_acquire) ==
               m_tail.load(std::memory_order_acquire);
    }

private:
    std::array<T, N>          m_buf{};
    std::atomic<ds::usize>    m_head{0};
    std::atomic<ds::usize>    m_tail{0};
};

} // namespace ds::common
