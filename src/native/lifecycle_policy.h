#pragma once
#include <atomic>
#include <cstdint>

namespace gdtpc
{
enum class RemoteCompletion { not_started, completed, unknown };
struct RemoteStoragePolicy
{
    bool thread_started{};
    bool completion_confirmed{};
    [[nodiscard]] bool may_free() const noexcept { return !thread_started || completion_confirmed; }
    [[nodiscard]] bool retry_prohibited() const noexcept { return thread_started && !completion_confirmed; }
};

class InitializationGate
{
public:
    [[nodiscard]] bool try_begin() noexcept
    {
        auto expected = 0U;
        return phase_.compare_exchange_strong(expected, 1U, std::memory_order_acq_rel);
    }
    void publish_active() noexcept { phase_.store(2U, std::memory_order_release); }
    [[nodiscard]] std::uint32_t phase() const noexcept { return phase_.load(std::memory_order_acquire); }
private:
    std::atomic<std::uint32_t> phase_{0};
};

class WriterOwnershipPolicy
{
public:
    void worker_started() noexcept { worker_owns_file_ = true; thread_handle_open_ = true; }
    void stop_requested() noexcept { stop_requested_ = true; }
    void join_result(bool completed) noexcept
    {
        if (!completed) { shutdown_pending_ = true; return; }
        thread_handle_open_ = false; worker_owns_file_ = false; shutdown_pending_ = false;
    }
    [[nodiscard]] bool worker_owns_file() const noexcept { return worker_owns_file_; }
    [[nodiscard]] bool thread_handle_open() const noexcept { return thread_handle_open_; }
    [[nodiscard]] bool shutdown_pending() const noexcept { return shutdown_pending_; }
    [[nodiscard]] bool stop_requested_value() const noexcept { return stop_requested_; }
private:
    bool worker_owns_file_{};
    bool thread_handle_open_{};
    bool stop_requested_{};
    bool shutdown_pending_{};
};

struct SessionIdentity
{
    std::uintptr_t camera{}, engine{}, player{};
    friend bool operator==(const SessionIdentity&, const SessionIdentity&) = default;
};

class SessionTracker
{
public:
    std::uint64_t observe(SessionIdentity identity, bool valid) noexcept
    {
        if (!valid) return generation_;
        if (!bound_ || !(identity == identity_)) { identity_ = identity; bound_ = true; ++generation_; }
        return generation_;
    }
private:
    SessionIdentity identity_{};
    std::uint64_t generation_{};
    bool bound_{};
};
}
