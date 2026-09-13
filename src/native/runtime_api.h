#pragma once

#include <cstddef>
#include <cstdint>

#if defined(GDTPC_RUNTIME_EXPORTS)
#define GDTPC_API extern "C" __declspec(dllexport)
#else
#define GDTPC_API extern "C" __declspec(dllimport)
#endif

enum class GdTpcPhase : std::uint32_t
{
    inert = 0,
    validating = 1,
    ready_validation_only = 2,
    logging_active = 3,
    rejected_host = 100,
    rejected_module = 101,
    rejected_access_point = 102,
    hook_failed = 103,
    stop_requested = 4,
    stopped_resident = 5,
    shutdown_pending = 6,
    rejected_config = 104,
    internal_error = 199
};

struct GdTpcRuntimeStatus
{
    std::uint32_t abi_version;
    GdTpcPhase phase;
    std::uint32_t hooks_installed;
    std::uint32_t game_state_writes_enabled;
};

// ABI v2 is append-only. The v1 status export remains available to old tools.
struct GdTpcRuntimeStatusV2
{
    std::uint32_t abi_version;
    std::uint32_t structure_size;
    GdTpcPhase phase;
    std::uint32_t hooks_installed;
    std::uint32_t game_state_writes_enabled;
    std::uint32_t writer_health;
    std::uint32_t restore_state;
    std::uint32_t config_loaded; // 1 once a schema-v1 configuration validated and was retained
    std::uint64_t callback_count;
    std::uint64_t telemetry_published;
    std::uint64_t telemetry_dropped;
    std::uint64_t session_generation;
    std::uint64_t control_write_count;
    std::uint64_t restore_write_count;
    std::uint32_t collision_enabled;
    std::uint32_t collision_state;
    float collision_arm;
    std::uint32_t collision_reserved;
    std::uint64_t collision_query_count;
    std::uint64_t collision_hit_count;
    std::uint64_t collision_fault_count;
    std::uint64_t collision_write_count;
};

constexpr std::uint32_t GDTPC_REMOTE_REQUEST_ABI = 2;
constexpr std::size_t GDTPC_REMOTE_PATH_CAPACITY = 32768;

struct GdTpcRemoteInitializeRequest
{
    std::uint32_t abi_version;
    std::uint32_t structure_size;
    wchar_t log_path[GDTPC_REMOTE_PATH_CAPACITY];
    wchar_t config_path[GDTPC_REMOTE_PATH_CAPACITY];
    GdTpcRuntimeStatusV2 result;
    std::uint32_t completed;
    std::uint32_t reserved;
};

// Logical stop is requested the same way initialization is: a dedicated DWORD WINAPI(void*) entry
// with a versioned request block, never by casting an ordinary export to a thread entry point.
struct GdTpcRemoteStopRequest
{
    std::uint32_t abi_version;
    std::uint32_t structure_size;
    std::uint32_t timeout_ms;
    std::uint32_t reserved;
    GdTpcRuntimeStatusV2 result;
    std::uint32_t completed;
    std::uint32_t reserved2;
};

// Gate 1-only controlled fault request. The callback keeps the real camera valid, refuses one
// adapter write, and proves that the ordinary transaction rollback restores the full preimage.
struct GdTpcRemoteGate1FaultRequest
{
    std::uint32_t abi_version;
    std::uint32_t structure_size;
    std::uint32_t timeout_ms;
    std::uint32_t reserved;
    GdTpcRuntimeStatusV2 result;
    std::uint64_t control_writes_before;
    std::uint64_t restore_writes_before;
    std::uint32_t completed;
    std::uint32_t reserved2;
};

static_assert(sizeof(GdTpcRuntimeStatusV2) == 128);
static_assert(sizeof(GdTpcRemoteInitializeRequest) == 131216);
static_assert(sizeof(GdTpcRemoteStopRequest) == 152);
static_assert(sizeof(GdTpcRemoteGate1FaultRequest) == 168);

GDTPC_API GdTpcRuntimeStatus __cdecl GdTpcGetRuntimeStatus() noexcept;
GDTPC_API GdTpcRuntimeStatusV2 __cdecl GdTpcGetRuntimeStatusV2() noexcept;
GDTPC_API std::uint32_t __cdecl GdTpcValidateKnownFiles(const wchar_t* game_root) noexcept;
GDTPC_API GdTpcPhase __cdecl GdTpcInitializeValidation() noexcept;
GDTPC_API GdTpcPhase __cdecl GdTpcInitializeLogging(const wchar_t* log_path) noexcept;
GDTPC_API GdTpcPhase __cdecl GdTpcInitializeLoggingV2(const wchar_t* log_path, const wchar_t* config_path) noexcept;
GDTPC_API DWORD WINAPI GdTpcInitializeRemote(void* request) noexcept;
GDTPC_API GdTpcPhase __cdecl GdTpcRequestLogicalStop(std::uint32_t timeout_ms) noexcept;
GDTPC_API DWORD WINAPI GdTpcRequestLogicalStopRemote(void* request) noexcept;
GDTPC_API DWORD WINAPI GdTpcRequestGate1RecoverableFaultRemote(void* request) noexcept;
GDTPC_API std::uint32_t __cdecl GdTpcGetLastErrorText(wchar_t* destination, std::uint32_t capacity) noexcept;
// Offline settings check with the runtime's own strict parser, for launchers: never touches lifecycle, hooks or game state.
// Returns 1 when the file is a valid configuration, 0 otherwise; the reason (or an empty string) is written to `error_text`.
GDTPC_API std::uint32_t __cdecl GdTpcCheckConfigFile(const wchar_t* config_path, wchar_t* error_text, std::uint32_t capacity) noexcept;
