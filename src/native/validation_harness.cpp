#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime_api.h"

#include <iostream>

using GetStatus = GdTpcRuntimeStatus(__cdecl*)() noexcept;
using Initialize = GdTpcPhase(__cdecl*)() noexcept;
using ValidateKnownFiles = std::uint32_t(__cdecl*)(const wchar_t*) noexcept;

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
    {
        std::wcerr << L"Supply the validation DLL path and Grim Dawn root.\n";
        return 2;
    }

    const auto module = LoadLibraryW(argv[1]);
    if (module == nullptr)
    {
        std::wcerr << L"Could not load validation DLL. Win32 error " << GetLastError() << L".\n";
        return 3;
    }

    const auto get_status = reinterpret_cast<GetStatus>(GetProcAddress(module, "GdTpcGetRuntimeStatus"));
    const auto initialize = reinterpret_cast<Initialize>(GetProcAddress(module, "GdTpcInitializeValidation"));
    const auto validate_files = reinterpret_cast<ValidateKnownFiles>(GetProcAddress(module, "GdTpcValidateKnownFiles"));
    if (get_status == nullptr || initialize == nullptr || validate_files == nullptr)
    {
        std::wcerr << L"Required validation exports are missing.\n";
        FreeLibrary(module);
        return 4;
    }

    const auto before = get_status();
    const auto files_are_known = validate_files(argv[2]);
    const auto result = initialize();
    const auto after = get_status();
    const auto passed = files_are_known == 1 && before.abi_version == 1 && before.phase == GdTpcPhase::inert &&
        result == GdTpcPhase::rejected_host && after.phase == GdTpcPhase::rejected_host &&
        after.hooks_installed == 0 && after.game_state_writes_enabled == 0;

    FreeLibrary(module);
    if (!passed)
    {
        std::wcerr << L"Validation runtime did not fail closed in the harness.\n";
        return 5;
    }

    std::wcout << L"PASS: known files matched; non-game host rejected; hooks=0; game-state-writes=0.\n";
    return 0;
}
