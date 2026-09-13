// Exercises the exported lifecycle surface of the real logging runtime DLL from a harmless
// non-game x64 host. Every path here must fail closed: the host is not Grim Dawn, so no hook
// may ever be installed, no writer may be created, and no game-state write may be enabled.
//
// This complements lifecycle_policy_tests.cpp, which covers the ownership model in isolation;
// this binary covers the shipped exports and the remote-initialization ABI itself.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime_api.h"

#include <atomic>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using GetStatus = GdTpcRuntimeStatus(__cdecl*)() noexcept;
using GetStatusV2 = GdTpcRuntimeStatusV2(__cdecl*)() noexcept;
using InitializeLogging = GdTpcPhase(__cdecl*)(const wchar_t*) noexcept;
using InitializeLoggingV2 = GdTpcPhase(__cdecl*)(const wchar_t*, const wchar_t*) noexcept;
using InitializeRemote = DWORD(WINAPI*)(void*) noexcept;
using RequestLogicalStop = GdTpcPhase(__cdecl*)(std::uint32_t) noexcept;
using RequestStopRemote = DWORD(WINAPI*)(void*) noexcept;
using RequestGate1FaultRemote = DWORD(WINAPI*)(void*) noexcept;
using ValidateKnownFiles = std::uint32_t(__cdecl*)(const wchar_t*) noexcept;
using GetLastErrorText = std::uint32_t(__cdecl*)(wchar_t*, std::uint32_t) noexcept;

void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
Function resolve(const HMODULE module, const char* name)
{
    const auto address = GetProcAddress(module, name);
    if (address == nullptr) throw std::runtime_error(std::string("missing export: ") + name);
    return reinterpret_cast<Function>(address);
}

void require_inert_and_unhooked(const GdTpcRuntimeStatusV2& status, const char* message)
{
    require(status.hooks_installed == 0 && status.game_state_writes_enabled == 0 &&
        status.writer_health == 0 && status.callback_count == 0 && status.telemetry_published == 0 &&
        status.control_write_count == 0 && status.restore_write_count == 0 &&
        status.collision_enabled == 0 && status.collision_query_count == 0 &&
        status.collision_hit_count == 0 && status.collision_fault_count == 0 &&
        status.collision_write_count == 0, message);
}
}

int wmain(const int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::wcerr << L"Supply the logging runtime DLL path.\n";
        return 2;
    }
    const auto module = LoadLibraryW(argv[1]);
    if (module == nullptr)
    {
        std::wcerr << L"Could not load the logging runtime. Win32 error " << GetLastError() << L".\n";
        return 3;
    }

    auto exit_code = 0;
    try
    {
        const auto status_v1 = resolve<GetStatus>(module, "GdTpcGetRuntimeStatus");
        const auto status_v2 = resolve<GetStatusV2>(module, "GdTpcGetRuntimeStatusV2");
        const auto initialize_logging = resolve<InitializeLogging>(module, "GdTpcInitializeLogging");
        const auto initialize_logging_v2 = resolve<InitializeLoggingV2>(module, "GdTpcInitializeLoggingV2");
        const auto initialize_remote = resolve<InitializeRemote>(module, "GdTpcInitializeRemote");
        const auto request_stop = resolve<RequestLogicalStop>(module, "GdTpcRequestLogicalStop");
        const auto request_fault = resolve<RequestGate1FaultRemote>(module, "GdTpcRequestGate1RecoverableFaultRemote");
        const auto validate_files = resolve<ValidateKnownFiles>(module, "GdTpcValidateKnownFiles");
        const auto last_error = resolve<GetLastErrorText>(module, "GdTpcGetLastErrorText");
        using CheckConfigFile = std::uint32_t(__cdecl*)(const wchar_t*, wchar_t*, std::uint32_t);
        const auto check_config = resolve<CheckConfigFile>(module, "GdTpcCheckConfigFile");
        {
            wchar_t reason[256]{};
            require(check_config(nullptr, reason, 256) == 0 && reason[0] != L'\0', "a null settings path was accepted");
            require(check_config(L"Z:\\gdtpc-missing\\runtime.ini", reason, 256) == 0 && reason[0] != L'\0',
                "a missing settings file was accepted");
            wchar_t temp_dir[MAX_PATH]{};
            wchar_t temp_file[MAX_PATH]{};
            require(GetTempPathW(MAX_PATH, temp_dir) != 0 && GetTempFileNameW(temp_dir, L"gdc", 0, temp_file) != 0,
                "a temporary settings file could not be created");
            const auto handle = CreateFileW(temp_file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            constexpr char garbage[] = "[mystery]\nvalue=1\n";
            DWORD written = 0;
            const auto wrote = handle != INVALID_HANDLE_VALUE && WriteFile(handle, garbage, sizeof(garbage) - 1, &written, nullptr) != FALSE;
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            require(wrote, "the temporary settings file could not be written");
            require(check_config(temp_file, reason, 256) == 0 && reason[0] != L'\0', "an invalid settings file was accepted");
            require(check_config(temp_file, nullptr, 0) == 0, "checking without an error buffer failed unsafely");
            DeleteFileW(temp_file);
            require_inert_and_unhooked(status_v2(), "checking a settings file changed lifecycle state");
        }

        // Status layout and inert starting state.
        const auto first_v1 = status_v1();
        require(first_v1.abi_version == 1 && first_v1.phase == GdTpcPhase::inert && first_v1.hooks_installed == 0 &&
            first_v1.game_state_writes_enabled == 0, "v1 status did not start inert and unhooked");
        const auto first_v2 = status_v2();
        require(first_v2.abi_version == 2 && first_v2.structure_size == static_cast<std::uint32_t>(sizeof(GdTpcRuntimeStatusV2)) &&
            first_v2.phase == GdTpcPhase::inert, "v2 status did not start inert with the expected layout");
        require(first_v2.config_loaded == 0 && first_v2.restore_state == 0 && first_v2.session_generation == 0,
            "v2 status did not start with a clean configuration/restore/session state");
        require_inert_and_unhooked(first_v2, "v2 status did not start clean");

        // Argument rejection that must not touch lifecycle state.
        require(validate_files(nullptr) == 0 && validate_files(L"") == 0, "empty game root was not rejected");
        require(initialize_remote(nullptr) == static_cast<DWORD>(GdTpcPhase::internal_error),
            "a null remote request was not rejected");

        // Logical stop before activation must be a no-op, never a claim of a clean shutdown.
        require(request_stop(0) == GdTpcPhase::inert, "logical stop from inert did not report inert");
        require_inert_and_unhooked(status_v2(), "logical stop from inert changed lifecycle state");
        require(request_fault(nullptr) == static_cast<DWORD>(GdTpcPhase::internal_error),
            "a null Gate 1 fault request was not rejected");
        GdTpcRemoteGate1FaultRequest malformed_fault{};
        malformed_fault.abi_version = GDTPC_REMOTE_REQUEST_ABI + 1;
        malformed_fault.structure_size = static_cast<std::uint32_t>(sizeof(malformed_fault));
        malformed_fault.timeout_ms = 1000;
        require(request_fault(&malformed_fault) == static_cast<DWORD>(GdTpcPhase::internal_error) &&
            malformed_fault.completed == 0, "a malformed Gate 1 fault request was not safely rejected");

        // Malformed remote requests must be refused without running initialization and without
        // setting the completion marker the injector relies on.
        {
            std::vector<GdTpcRemoteInitializeRequest> malformed(3);
            malformed[0].abi_version = GDTPC_REMOTE_REQUEST_ABI + 1;
            malformed[0].structure_size = static_cast<std::uint32_t>(sizeof(GdTpcRemoteInitializeRequest));
            malformed[1].abi_version = GDTPC_REMOTE_REQUEST_ABI;
            malformed[1].structure_size = static_cast<std::uint32_t>(sizeof(GdTpcRemoteInitializeRequest)) - 8;
            malformed[2].abi_version = GDTPC_REMOTE_REQUEST_ABI;
            malformed[2].structure_size = static_cast<std::uint32_t>(sizeof(GdTpcRemoteInitializeRequest));
            malformed[2].log_path[GDTPC_REMOTE_PATH_CAPACITY - 1] = L'x'; // unterminated fixed-size path
            const char* names[]{"a mismatched request ABI", "a mismatched request size", "an unterminated request path"};
            for (std::size_t index = 0; index < malformed.size(); ++index)
            {
                require(initialize_remote(&malformed[index]) == static_cast<DWORD>(GdTpcPhase::internal_error), names[index]);
                require(malformed[index].completed == 0, "a refused remote request set the completion marker");
            }
            require_inert_and_unhooked(status_v2(), "a refused remote request changed lifecycle state");
        }

        // Thirty-two simultaneous callers: the host is not Grim Dawn, so every caller must see the
        // same terminal rejection, and no caller may create a hook, a writer, or a second worker.
        {
            std::atomic<unsigned> unexpected{0};
            std::vector<std::thread> callers;
            callers.reserve(32);
            for (unsigned index = 0; index < 32; ++index)
                callers.emplace_back([&]
                {
                    const auto result = initialize_logging_v2(L"C:\\nonexistent\\gdtpc-host-test.csv", nullptr);
                    if (result != GdTpcPhase::rejected_host) ++unexpected;
                });
            for (auto& caller : callers) caller.join();
            require(unexpected.load() == 0, "a concurrent initialization caller saw a phase other than rejected_host");
            const auto after = status_v2();
            require(after.phase == GdTpcPhase::rejected_host, "concurrent initialization did not leave a terminal rejection");
            require_inert_and_unhooked(after, "concurrent initialization created a hook, writer, or write capability");
        }

        // Rejection is terminal: later callers, including the v1 entry point, cannot re-enter.
        require(initialize_logging(L"C:\\nonexistent\\gdtpc-host-test.csv") == GdTpcPhase::rejected_host,
            "the v1 entry point re-entered a terminal rejection");
        require(initialize_logging_v2(nullptr, nullptr) == GdTpcPhase::rejected_host,
            "a null log path re-entered a terminal rejection");
        require_inert_and_unhooked(status_v2(), "a post-rejection call created runtime resources");

        // A well-formed remote request still fails closed, but must report the full v2 result and
        // set the completion marker so the injector can distinguish refusal from a lost thread.
        {
            GdTpcRemoteInitializeRequest request{};
            request.abi_version = GDTPC_REMOTE_REQUEST_ABI;
            request.structure_size = static_cast<std::uint32_t>(sizeof(request));
            wcscpy_s(request.log_path, L"C:\\nonexistent\\gdtpc-host-test.csv");
            wcscpy_s(request.config_path, L"C:\\nonexistent\\gdtpc-host-test.ini");
            require(initialize_remote(&request) == static_cast<DWORD>(GdTpcPhase::rejected_host),
                "a well-formed remote request did not report the host rejection");
            require(request.completed == 1, "a completed remote request did not set the completion marker");
            require(request.result.abi_version == 2 && request.result.structure_size == static_cast<std::uint32_t>(sizeof(GdTpcRuntimeStatusV2)) &&
                request.result.phase == GdTpcPhase::rejected_host, "the remote result did not carry the v2 status");
            require_inert_and_unhooked(request.result, "the remote result reported a hook, writer, or write capability");
        }

        // Error text is bounded, NUL-terminated on truncation, and never requires the caller to
        // hold the lifecycle lock.
        {
            const auto required = last_error(nullptr, 0);
            require(required > 1, "a rejected runtime reported no error text");
            std::wstring buffer(4, L'#');
            const auto again = last_error(buffer.data(), static_cast<std::uint32_t>(buffer.size()));
            require(again == required, "error-text length changed between queries");
            require(buffer[3] == L'\0', "truncated error text was not NUL-terminated");
        }

        // Logical stop after a rejection must report the rejection, not a clean stop.
        require(request_stop(0) == GdTpcPhase::rejected_host, "logical stop after rejection claimed a different state");
        require_inert_and_unhooked(status_v2(), "logical stop after rejection changed lifecycle state");

        // The remote stop entry validates its versioned request the same way initialization does,
        // and must never report stopped-resident for a runtime that was never active.
        {
            const auto stop_remote = resolve<RequestStopRemote>(module, "GdTpcRequestLogicalStopRemote");
            require(stop_remote(nullptr) == static_cast<DWORD>(GdTpcPhase::internal_error), "a null stop request was not rejected");
            GdTpcRemoteStopRequest bad{};
            bad.abi_version = GDTPC_REMOTE_REQUEST_ABI + 1;
            bad.structure_size = static_cast<std::uint32_t>(sizeof(bad));
            require(stop_remote(&bad) == static_cast<DWORD>(GdTpcPhase::internal_error), "a mismatched stop ABI was not rejected");
            require(bad.completed == 0, "a refused stop request set the completion marker");
            bad.abi_version = GDTPC_REMOTE_REQUEST_ABI;
            bad.structure_size = static_cast<std::uint32_t>(sizeof(bad)) - 4;
            require(stop_remote(&bad) == static_cast<DWORD>(GdTpcPhase::internal_error), "a mismatched stop size was not rejected");
            require(bad.completed == 0, "a refused stop request set the completion marker");

            GdTpcRemoteStopRequest request{};
            request.abi_version = GDTPC_REMOTE_REQUEST_ABI;
            request.structure_size = static_cast<std::uint32_t>(sizeof(request));
            request.timeout_ms = 1000;
            require(stop_remote(&request) == static_cast<DWORD>(GdTpcPhase::rejected_host),
                "a well-formed stop request did not report the existing rejection");
            require(request.completed == 1, "a completed stop request did not set the completion marker");
            require(request.result.phase != GdTpcPhase::stopped_resident,
                "a runtime that was never active reported a clean stop");
            require_inert_and_unhooked(request.result, "a stop request created a hook, writer, or write capability");
        }

        std::cout << "PASS: exported status layout, argument rejection, malformed/complete remote ABI, "
                     "32-caller terminal rejection, bounded error text, remote stop-request ABI, and "
                     "stop-before-activation no-op; hooks=0, writes=0.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        exit_code = 1;
    }

    // The runtime pins itself only on successful activation, which cannot happen in this host.
    FreeLibrary(module);
    return exit_code;
}
