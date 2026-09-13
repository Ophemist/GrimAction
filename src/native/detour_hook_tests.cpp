#include "detour_hook.h"
#include <detours.h>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
using TestFunction = int(__cdecl*)(int);

__declspec(noinline) int __cdecl increment(const int value) { return value + 1; }
TestFunction true_increment = increment;

int __cdecl detoured_increment(const int value)
{
    return true_increment(value) + 100;
}

void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int invoke(const int value)
{
    volatile TestFunction entry = increment;
    return entry(value);
}

void wait_for_value(const std::atomic<int>& value, const int expected, const char* message)
{
    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        if (value.load(std::memory_order_acquire) == expected) return;
        Sleep(1);
    }
    throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        require(invoke(1) == 2, "Baseline function result is wrong.");

        std::atomic<int> worker_result{0};
        std::jthread worker([&](const std::stop_token token)
        {
            while (!token.stop_requested())
                worker_result.store(invoke(1), std::memory_order_release);
        });
        wait_for_value(worker_result, 2, "Worker did not observe the baseline function.");

        gdtpc::DetourHook invalid;
        require(invalid.attach(nullptr, reinterpret_cast<void*>(detoured_increment)) == ERROR_INVALID_PARAMETER,
            "Null target was not rejected.");
        require(!invalid.installed(), "Invalid hook reported installed.");

        gdtpc::DetourHook hook;
        require(DetourTransactionBegin() == NO_ERROR, "Could not create transaction-conflict fixture.");
        require(hook.attach(reinterpret_cast<void**>(&true_increment), reinterpret_cast<void*>(detoured_increment)) != NO_ERROR,
            "Attach unexpectedly took ownership of another transaction.");
        require(DetourTransactionAbort() == NO_ERROR, "Attach aborted a transaction it did not own.");
        require(!hook.installed(), "Failed conflict attach reported installed.");
        require(hook.attach(reinterpret_cast<void**>(&true_increment), reinterpret_cast<void*>(detoured_increment)) == NO_ERROR,
            "Hook attach failed.");
        require(hook.installed(), "Hook did not report installed.");
        require(invoke(1) == 102, "Detour or trampoline result is wrong.");
        wait_for_value(worker_result, 102, "Concurrent worker did not observe the detour.");
        require(hook.attach(reinterpret_cast<void**>(&true_increment), reinterpret_cast<void*>(detoured_increment)) == ERROR_ALREADY_EXISTS,
            "Duplicate attach was not rejected.");

        // The worker is retired BEFORE anything detaches.
        //
        // Detours protects a thread whose instruction pointer is inside the rewritten bytes of the
        // TARGET; that is what DetourUpdateThread is for, and the attach above exercises it. It cannot
        // protect a thread already inside the DETOUR function holding the trampoline address, because
        // detaching frees the trampoline underneath it. Detaching while other threads may be inside the
        // detour is unsafe by construction, not a defect in this code.
        //
        // Asserting otherwise made this test fault roughly twice per thousand runs with an execute
        // violation on freed trampoline memory, reproduced at cycle 231 of a dedicated probe. The
        // runtime never detaches at all: the logical stop leaves the hook installed and inert until the
        // process exits, precisely so this can never arise. Keep it that way.
        worker.request_stop();
        worker.join();

        require(DetourTransactionBegin() == NO_ERROR, "Could not create detach-conflict fixture.");
        require(hook.detach() != NO_ERROR, "Detach unexpectedly took ownership of another transaction.");
        require(hook.installed(), "Failed detach discarded installed state.");
        require(invoke(1) == 102, "Failed detach changed the installed hook.");
        require(DetourTransactionAbort() == NO_ERROR, "Detach aborted a transaction it did not own.");
        require(hook.detach() == NO_ERROR, "Hook detach failed.");
        require(!hook.installed(), "Hook remained installed after detach.");
        require(invoke(1) == 2, "Original function was not restored.");
        require(hook.detach() == ERROR_NOT_FOUND, "Duplicate detach was not rejected.");

        std::cout << "PASS: Detours transaction ownership, attach under a concurrently executing thread, trampoline, retained-state detach failure, and detach after quiescing.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
