#pragma once

#include <atomic>
#include <cstdint>

namespace gdtpc
{
struct CallbackEvidenceTicket
{
    std::uint64_t hook_entry{};
    std::uint32_t thread_id{};
};

struct CallbackEvidenceSnapshot
{
    std::uint64_t hook_entry{};
    std::uint64_t original_return{};
    std::uint32_t callback_thread_id{};
    std::uint32_t owner_thread_id{};
    std::uint64_t owner_thread_mismatches{};
};

// Allocation-free evidence for the live Gate 0 hook contract. The hook takes one ticket before
// its single original call and records one return after that call completes. In a single-owner,
// non-reentrant callback stream, hook_entry and original_return remain equal and every active
// snapshot carries the same thread id. Any other active thread increments the mismatch counter.
class CallbackEvidence final
{
public:
    [[nodiscard]] CallbackEvidenceTicket enter(const std::uint32_t thread_id) noexcept
    {
        return {hook_entries_.fetch_add(1, std::memory_order_relaxed) + 1, thread_id};
    }

    [[nodiscard]] CallbackEvidenceSnapshot original_returned(
        const CallbackEvidenceTicket ticket, const bool observe_active_owner) noexcept
    {
        const auto returned = original_returns_.fetch_add(1, std::memory_order_relaxed) + 1;
        auto owner = owner_thread_id_.load(std::memory_order_relaxed);
        if (observe_active_owner)
        {
            if (owner == 0)
            {
                auto expected = std::uint32_t{0};
                static_cast<void>(owner_thread_id_.compare_exchange_strong(
                    expected, ticket.thread_id, std::memory_order_relaxed));
                owner = owner_thread_id_.load(std::memory_order_relaxed);
            }
            if (owner != ticket.thread_id)
                owner_thread_mismatches_.fetch_add(1, std::memory_order_relaxed);
        }
        return {ticket.hook_entry, returned, ticket.thread_id, owner_thread_id_.load(std::memory_order_relaxed),
            owner_thread_mismatches_.load(std::memory_order_relaxed)};
    }

    [[nodiscard]] std::uint64_t hook_entries() const noexcept { return hook_entries_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t original_returns() const noexcept { return original_returns_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t owner_thread_id() const noexcept { return owner_thread_id_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t owner_thread_mismatches() const noexcept
    {
        return owner_thread_mismatches_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> hook_entries_{0};
    std::atomic<std::uint64_t> original_returns_{0};
    std::atomic<std::uint32_t> owner_thread_id_{0};
    std::atomic<std::uint64_t> owner_thread_mismatches_{0};
};
}
