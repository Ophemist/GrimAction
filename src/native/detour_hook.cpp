#include "detour_hook.h"

#include <detours.h>
#include <tlhelp32.h>

#include <vector>

namespace
{
void close_thread_handles(std::vector<HANDLE>& handles) noexcept
{
    for (const auto handle : handles) CloseHandle(handle);
    handles.clear();
}

LONG collect_process_threads(std::vector<HANDLE>& opened_threads) noexcept
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return static_cast<LONG>(GetLastError());

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process_id = GetCurrentProcessId();
    const auto current_thread_id = GetCurrentThreadId();
    auto status = NO_ERROR;
    try
    {
        if (Thread32First(snapshot, &entry) != FALSE)
        {
            SetLastError(ERROR_SUCCESS);
            do
            {
                if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == current_thread_id) continue;
                const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                    THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (thread == nullptr)
                {
                    const auto error = GetLastError();
                    if (error == ERROR_INVALID_PARAMETER) continue;
                    status = static_cast<LONG>(error);
                    break;
                }
                try { opened_threads.push_back(thread); }
                catch (...) { CloseHandle(thread); status = ERROR_NOT_ENOUGH_MEMORY; break; }
                SetLastError(ERROR_SUCCESS);
            } while (Thread32Next(snapshot, &entry) != FALSE);
            if (status == NO_ERROR && GetLastError() != ERROR_NO_MORE_FILES) status = static_cast<LONG>(GetLastError());
        }
        else status = static_cast<LONG>(GetLastError());
    }
    catch (...) { status = ERROR_NOT_ENOUGH_MEMORY; }
    CloseHandle(snapshot);
    return status;
}

LONG run_transaction(std::vector<HANDLE>& opened_threads, void** target_pointer, void* replacement, const bool attach) noexcept
{
    auto status = collect_process_threads(opened_threads);
    if (status != NO_ERROR) return status;
    bool owns_transaction = false;
    status = DetourTransactionBegin();
    if (status == NO_ERROR) owns_transaction = true;
    if (!owns_transaction) return status; // never abort somebody else's transaction

    status = DetourUpdateThread(GetCurrentThread());
    for (const auto thread : opened_threads)
    {
        if (status != NO_ERROR) break;
        status = DetourUpdateThread(thread);
    }
    if (status == NO_ERROR)
        status = attach ? DetourAttach(reinterpret_cast<PVOID*>(target_pointer), replacement)
                        : DetourDetach(reinterpret_cast<PVOID*>(target_pointer), replacement);
    if (status == NO_ERROR) status = DetourTransactionCommit();
    else static_cast<void>(DetourTransactionAbort());
    return status;
}
}

LONG gdtpc::DetourHook::attach(void** target_pointer, void* replacement) noexcept
{
    if (installed_) return last_error_ = ERROR_ALREADY_EXISTS;
    if (target_pointer == nullptr || *target_pointer == nullptr || replacement == nullptr)
        return last_error_ = ERROR_INVALID_PARAMETER;

    std::vector<HANDLE> opened_threads;
    auto status = run_transaction(opened_threads, target_pointer, replacement, true);
    close_thread_handles(opened_threads);

    if (status == NO_ERROR)
    {
        target_pointer_ = target_pointer;
        replacement_ = replacement;
        installed_ = true;
    }
    return last_error_ = status;
}

LONG gdtpc::DetourHook::detach() noexcept
{
    if (!installed_) return last_error_ = ERROR_NOT_FOUND;

    std::vector<HANDLE> opened_threads;
    auto status = run_transaction(opened_threads, target_pointer_, replacement_, false);
    close_thread_handles(opened_threads);

    if (status == NO_ERROR)
    {
        target_pointer_ = nullptr;
        replacement_ = nullptr;
        installed_ = false;
    }
    return last_error_ = status;
}
