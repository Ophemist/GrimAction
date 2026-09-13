#pragma once

// Bounded single-producer/single-consumer telemetry ring shared by the runtime DLL and the
// offline fault tests. Extracted from runtime.cpp so the coherence, drop and ownership rules
// are exercised by a test binary instead of only by the live camera callback.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gdtpc
{
// Sample must be trivially copyable and must expose a std::uint64_t sequence member.
template <typename Sample, std::size_t Capacity>
class TelemetryQueue final
{
public:
    static constexpr std::size_t capacity = Capacity;

    // The camera callback claims exclusive producer rights; a nested or concurrent callback
    // must drop its sample rather than interleave with an in-flight publication.
    [[nodiscard]] bool try_acquire_producer() noexcept
    {
        return !producer_.test_and_set(std::memory_order_acquire);
    }

    void release_producer() noexcept { producer_.clear(std::memory_order_release); }

    // Returns false when the ring is full. The caller counts the drop; the queue never blocks
    // and never overwrites a row the consumer has not yet retired.
    [[nodiscard]] bool publish(Sample& sample) noexcept
    {
        const auto write = write_index_.load(std::memory_order_relaxed);
        const auto read = read_index_.load(std::memory_order_acquire);
        if (write - read >= Capacity) return false;
        auto& slot = slots_[static_cast<std::size_t>(write % Capacity)];
        sample.sequence = write + 1;
        slot.value = sample;
        slot.committed.store(write + 1, std::memory_order_release);
        write_index_.store(write + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Only a fully committed row is ever handed out, so a partially written
    // slot is invisible rather than published as a torn row.
    [[nodiscard]] bool peek(Sample& out) noexcept
    {
        const auto read = read_index_.load(std::memory_order_relaxed);
        if (read >= write_index_.load(std::memory_order_acquire)) return false;
        auto& slot = slots_[static_cast<std::size_t>(read % Capacity)];
        if (slot.committed.load(std::memory_order_acquire) != read + 1) return false;
        out = slot.value;
        return true;
    }

    void pop() noexcept
    {
        const auto read = read_index_.load(std::memory_order_relaxed);
        slots_[static_cast<std::size_t>(read % Capacity)].committed.store(0, std::memory_order_release);
        read_index_.store(read + 1, std::memory_order_release);
    }

    [[nodiscard]] bool empty() noexcept
    {
        return read_index_.load(std::memory_order_relaxed) >= write_index_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t published() const noexcept { return write_index_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t retired() const noexcept { return read_index_.load(std::memory_order_acquire); }

private:
    struct Slot
    {
        std::atomic<std::uint64_t> committed{0};
        Sample value{};
    };

    std::array<Slot, Capacity> slots_{};
    std::atomic<std::uint64_t> write_index_{0};
    std::atomic<std::uint64_t> read_index_{0};
    std::atomic_flag producer_ = ATOMIC_FLAG_INIT;
};
}
