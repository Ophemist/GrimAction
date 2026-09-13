#pragma once

// Telemetry writer drain loop, separated from the Win32 file handle so an offline test can
// substitute a fake byte sink and inject blocked writes, failed writes and failed flushes.
// The worker thread exclusively owns its sink; the joining thread never reclaims it.

#include <cstddef>

namespace gdtpc
{
constexpr std::size_t telemetry_line_capacity = 1200;

// Drains committed rows until the stop signal is observed and the queue is empty, or until the
// sink reports a failure. Returns false as soon as a write fails; the failing row is left in the
// queue so the caller can report an incomplete, not a complete, drain.
//
// wait_for_stop() returns true once shutdown has been requested.
// format(sample, buffer, capacity) returns the character count, negative or >= capacity on failure.
template <typename Queue, typename Sample, typename Sink, typename Wait, typename Format>
[[nodiscard]] bool run_drain_loop(Queue& queue, Sink& sink, Wait wait_for_stop, Format format) noexcept
{
    for (;;)
    {
        const auto stopped = wait_for_stop();
        Sample sample{};
        while (queue.peek(sample))
        {
            char line[telemetry_line_capacity]{};
            const auto length = format(sample, line, sizeof(line));
            if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(line)) return false;
            if (!sink.write(line, static_cast<std::size_t>(length))) return false;
            queue.pop();
        }
        if (stopped && queue.empty()) return true;
    }
}
}
