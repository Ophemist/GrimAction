// Offline fault coverage for the bounded telemetry ring and the writer drain loop that the
// logging runtime actually links. A fake byte sink stands in for the Win32 file handle so
// blocked writes, failed writes and failed flushes can be injected without a game process.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "callback_evidence.h"
#include "logging_drain.h"
#include "telemetry_queue.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct Row
{
    std::uint64_t sequence{};
    std::uint64_t payload{};
};

using Queue = gdtpc::TelemetryQueue<Row, 8>;

void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int format_row(const Row& row, char* line, const std::size_t capacity) noexcept
{
    return std::snprintf(line, capacity, "%llu,%llu\n",
        static_cast<unsigned long long>(row.sequence), static_cast<unsigned long long>(row.payload));
}

// Records everything written and can be told to fail, or to block, on a chosen write.
class FakeSink final
{
public:
    [[nodiscard]] bool write(const char* text, const std::size_t length) noexcept
    {
        const auto index = writes_.fetch_add(1, std::memory_order_acq_rel);
        if (block_at_ >= 0 && index == static_cast<std::uint64_t>(block_at_))
        {
            entered_block_.store(true, std::memory_order_release);
            WaitForSingleObject(release_, INFINITE);
        }
        if (fail_at_ >= 0 && index == static_cast<std::uint64_t>(fail_at_)) return false;
        try { text_.append(text, length); }
        catch (...) { return false; }
        return true;
    }

    [[nodiscard]] bool flush() noexcept { flushed_.store(true, std::memory_order_release); return !fail_flush_; }
    void close() noexcept { closed_.fetch_add(1, std::memory_order_acq_rel); }

    void fail_write_at(const long long index) noexcept { fail_at_ = index; }
    void block_write_at(const long long index) noexcept { block_at_ = index; }
    void fail_flush() noexcept { fail_flush_ = true; }
    void release_block() const noexcept { SetEvent(release_); }
    [[nodiscard]] bool entered_block() const noexcept { return entered_block_.load(std::memory_order_acquire); }
    [[nodiscard]] bool flushed() const noexcept { return flushed_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint32_t close_count() const noexcept { return closed_.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

    FakeSink() : release_{CreateEventW(nullptr, TRUE, FALSE, nullptr)} {}
    ~FakeSink() { if (release_ != nullptr) CloseHandle(release_); }
    FakeSink(const FakeSink&) = delete;
    FakeSink& operator=(const FakeSink&) = delete;

private:
    HANDLE release_{};
    std::string text_;
    std::atomic<std::uint64_t> writes_{0};
    std::atomic<bool> entered_block_{false};
    std::atomic<bool> flushed_{false};
    std::atomic<std::uint32_t> closed_{0};
    long long fail_at_{-1};
    long long block_at_{-1};
    bool fail_flush_{false};
};

std::size_t count_lines(const std::string& text)
{
    std::size_t lines = 0;
    for (const auto character : text) if (character == '\n') ++lines;
    return lines;
}

void publish(Queue& queue, const std::uint64_t payload, const bool expect_accepted, const char* message)
{
    Row row{};
    row.payload = payload;
    require(queue.try_acquire_producer(), "producer rights were already held");
    const auto accepted = queue.publish(row);
    queue.release_producer();
    require(accepted == expect_accepted, message);
}
}

int main()
{
    try
    {
        // The shipped hook evidence tracker must prove the normal one-thread, one-return order and
        // make both a second active owner and out-of-order returns visible in immutable samples.
        {
            gdtpc::CallbackEvidence evidence;
            const auto first_ticket = evidence.enter(42);
            const auto first = evidence.original_returned(first_ticket, true);
            require(first.hook_entry == 1 && first.original_return == 1 && first.callback_thread_id == 42 &&
                first.owner_thread_id == 42 && first.owner_thread_mismatches == 0,
                "first callback did not establish coherent owner/original evidence");
            const auto second_ticket = evidence.enter(42);
            const auto second = evidence.original_returned(second_ticket, true);
            require(second.hook_entry == 2 && second.original_return == 2 && second.owner_thread_mismatches == 0,
                "single-owner callback order drifted");
            const auto other_ticket = evidence.enter(99);
            const auto other = evidence.original_returned(other_ticket, true);
            require(other.owner_thread_id == 42 && other.callback_thread_id == 99 && other.owner_thread_mismatches == 1,
                "a second active callback owner was not reported");
        }
        {
            gdtpc::CallbackEvidence evidence;
            const auto outer = evidence.enter(7);
            const auto inner = evidence.enter(7);
            const auto inner_return = evidence.original_returned(inner, true);
            const auto outer_return = evidence.original_returned(outer, true);
            require(inner_return.hook_entry != inner_return.original_return &&
                outer_return.hook_entry != outer_return.original_return,
                "out-of-order original returns were not distinguishable in the evidence");
            require(evidence.hook_entries() == 2 && evidence.original_returns() == 2 &&
                evidence.owner_thread_id() == 7 && evidence.owner_thread_mismatches() == 0,
                "callback evidence totals were inconsistent");
        }

        // Capacity, drop accounting and sequence assignment.
        {
            Queue queue;
            for (std::uint64_t index = 0; index < Queue::capacity; ++index)
                publish(queue, index, true, "ring rejected a row while space remained");
            publish(queue, 999, false, "full ring accepted a row instead of reporting a drop");
            Row seen{};
            require(queue.peek(seen) && seen.sequence == 1 && seen.payload == 0, "oldest row was not published coherently");
            queue.pop();
            publish(queue, 1000, true, "retiring a row did not free a slot");
            require(queue.retired() == 1 && queue.published() == Queue::capacity + 1, "ring indices drifted");
        }

        // A nested or concurrent producer must be refused rather than interleaved.
        {
            Queue queue;
            require(queue.try_acquire_producer(), "first producer was refused");
            require(!queue.try_acquire_producer(), "reentrant producer was admitted");
            queue.release_producer();
            require(queue.try_acquire_producer(), "producer rights were not released");
            queue.release_producer();
        }

        // Normal drain: every published row reaches the sink exactly once, in order.
        {
            Queue queue;
            FakeSink sink;
            for (std::uint64_t index = 0; index < 5; ++index) publish(queue, index, true, "setup publish failed");
            const auto healthy = gdtpc::run_drain_loop<Queue, Row>(queue, sink, [] { return true; }, format_row);
            require(healthy, "clean drain reported an unhealthy writer");
            require(count_lines(sink.text()) == 5, "clean drain did not write every row");
            require(sink.text().rfind("1,0\n", 0) == 0, "clean drain reordered rows");
            require(queue.empty(), "clean drain left rows behind");
        }

        // Failed write: the loop stops immediately and leaves the failing row unretired.
        {
            Queue queue;
            FakeSink sink;
            for (std::uint64_t index = 0; index < 5; ++index) publish(queue, index, true, "setup publish failed");
            sink.fail_write_at(2);
            const auto healthy = gdtpc::run_drain_loop<Queue, Row>(queue, sink, [] { return true; }, format_row);
            require(!healthy, "failed write was reported as healthy");
            require(count_lines(sink.text()) == 2, "failed write did not stop the drain");
            require(queue.retired() == 2 && !queue.empty(), "failed row was retired despite not being written");
        }

        // Failed flush is reported separately from a healthy drain.
        {
            Queue queue;
            FakeSink sink;
            publish(queue, 7, true, "setup publish failed");
            sink.fail_flush();
            const auto healthy = gdtpc::run_drain_loop<Queue, Row>(queue, sink, [] { return true; }, format_row);
            require(healthy, "drain that wrote every row was reported unhealthy");
            require(!sink.flush(), "fake sink did not report the injected flush failure");
        }

        // Rows published after the stop signal but before the queue empties are still drained.
        {
            Queue queue;
            FakeSink sink;
            std::atomic<int> stop_polls{0};
            for (std::uint64_t index = 0; index < 3; ++index) publish(queue, index, true, "setup publish failed");
            const auto healthy = gdtpc::run_drain_loop<Queue, Row>(queue, sink,
                [&] { if (stop_polls.fetch_add(1) == 0) { publish(queue, 99, true, "late publish failed"); return false; } return true; },
                format_row);
            require(healthy, "stop drain reported an unhealthy writer");
            require(count_lines(sink.text()) == 4, "a row published before stop completed was lost");
        }

        // Blocked write: a bounded join must time out, leave the worker owning its sink, and the
        // joining thread must not close or reclaim anything. The worker closes exactly once.
        {
            Queue queue;
            FakeSink sink;
            HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            require(stop != nullptr, "could not create the stop event");
            for (std::uint64_t index = 0; index < 3; ++index) publish(queue, index, true, "setup publish failed");
            sink.block_write_at(1);
            std::atomic<bool> worker_finished{false};
            std::thread worker([&]
            {
                const auto healthy = gdtpc::run_drain_loop<Queue, Row>(queue, sink,
                    [&] { return WaitForSingleObject(stop, 5) == WAIT_OBJECT_0; }, format_row);
                static_cast<void>(sink.flush());
                sink.close();
                worker_finished.store(healthy, std::memory_order_release);
            });

            for (int attempt = 0; attempt < 2000 && !sink.entered_block(); ++attempt) Sleep(1);
            require(sink.entered_block(), "the fake sink never reached the injected blocking write");
            SetEvent(stop);
            const auto joined = worker.native_handle() != nullptr &&
                WaitForSingleObject(worker.native_handle(), 200) == WAIT_OBJECT_0;
            require(!joined, "a blocked writer reported a confirmed join");
            require(sink.close_count() == 0, "the sink was closed while the worker was still blocked in a write");
            require(!sink.flushed(), "the sink was flushed while the worker was still blocked in a write");

            sink.release_block();
            worker.join();
            require(worker_finished.load(std::memory_order_acquire), "the released writer did not complete cleanly");
            require(sink.close_count() == 1, "the worker did not close its sink exactly once");
            require(count_lines(sink.text()) == 3, "the released writer lost rows");
            CloseHandle(stop);
        }

        std::cout << "PASS: callback owner/original-order evidence, telemetry ring capacity/drop/coherence, "
                     "reentrant producer refusal, drain ordering, "
                     "failed write, failed flush, late publish, and blocked-write ownership retention.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
