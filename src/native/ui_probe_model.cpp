#include "ui_probe_model.h"

#include <cstring>

namespace
{
constexpr std::uintptr_t lowest_pointer = 0x10000;
constexpr std::uintptr_t highest_user_pointer = 0x00007fffffffffffULL;

void put32(std::uint8_t* out, const std::size_t offset, const std::uint32_t value) noexcept
{
    std::memcpy(out + offset, &value, sizeof(value));
}

void put64(std::uint8_t* out, const std::size_t offset, const std::uint64_t value) noexcept
{
    std::memcpy(out + offset, &value, sizeof(value));
}

// Appends one block at `cursor`. Returns false, leaving the cursor unchanged, when the region is not
// readable at all or the copy faults; a partially readable region is captured up to its end.
bool append_block(gdtpc::UiProbeMemory& memory, const std::uintptr_t address, const std::size_t wanted,
    const std::uint64_t parent_offset, const gdtpc::UiProbeBlockKind kind, const bool require_private,
    std::uint8_t* buffer, std::size_t& cursor) noexcept
{
    std::size_t readable = 0;
    bool private_memory = false;
    if (!memory.query(address, wanted, readable, private_memory) || readable == 0) return false;
    if (require_private && !private_memory) return false;
    if (readable > wanted) readable = wanted;
    const auto data = cursor + gdtpc::ui_probe_block_header_bytes;
    if (!memory.copy(address, buffer + data, readable)) return false;
    put64(buffer, cursor, address);
    put64(buffer, cursor + 8, parent_offset);
    put32(buffer, cursor + 16, static_cast<std::uint32_t>(readable));
    put32(buffer, cursor + 20, static_cast<std::uint32_t>(kind));
    cursor = data + readable;
    return true;
}
}

std::size_t gdtpc::ui_probe_record_capacity(const UiProbeLimits& limits) noexcept
{
    return ui_probe_header_bytes + (3 + limits.max_children + limits.max_input_children) * ui_probe_block_header_bytes +
        limits.engine_bytes + limits.ui_bytes + limits.max_children * limits.child_bytes + limits.input_device_bytes +
        limits.max_input_children * limits.input_child_bytes;
}

std::size_t gdtpc::capture_ui_probe(UiProbeMemory& memory, const std::uintptr_t engine, const std::uintptr_t ui,
    const std::uintptr_t input_device, const UiProbeMark& mark, const UiProbeLimits& limits,
    std::uint8_t* const buffer, const std::size_t capacity) noexcept
{
    if (buffer == nullptr || capacity < ui_probe_record_capacity(limits) || engine < lowest_pointer ||
        ui < lowest_pointer || input_device < lowest_pointer || engine > highest_user_pointer ||
        ui > highest_user_pointer || input_device > highest_user_pointer) return 0;

    auto cursor = ui_probe_header_bytes;
    if (!append_block(memory, engine, limits.engine_bytes, 0, UiProbeBlockKind::engine, false, buffer, cursor)) return 0;
    const auto ui_data = cursor + ui_probe_block_header_bytes;
    if (!append_block(memory, ui, limits.ui_bytes, 0, UiProbeBlockKind::ui, false, buffer, cursor)) return 0;
    const auto ui_captured = cursor - ui_data;
    std::uint32_t blocks = 2;

    const auto scan = ui_captured < limits.pointer_scan_bytes ? ui_captured : limits.pointer_scan_bytes;
    std::size_t children = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= scan && children < limits.max_children;
         offset += sizeof(std::uint64_t))
    {
        std::uint64_t value = 0;
        std::memcpy(&value, buffer + ui_data + offset, sizeof(value));
        const auto pointer = static_cast<std::uintptr_t>(value);
        if (pointer < lowest_pointer || pointer > highest_user_pointer || (pointer & 0x7) != 0 ||
            pointer == engine || pointer == ui || pointer == input_device) continue;
        // The same child is often referenced from several members; capture it once, at its first
        // referencing offset, so analysis keys it stably.
        auto duplicate = false;
        for (std::size_t earlier = 0; earlier < offset && !duplicate; earlier += sizeof(std::uint64_t))
        {
            std::uint64_t previous = 0;
            std::memcpy(&previous, buffer + ui_data + earlier, sizeof(previous));
            duplicate = previous == value;
        }
        if (duplicate) continue;
        if (!append_block(memory, pointer, limits.child_bytes, offset, UiProbeBlockKind::ui_child, true, buffer, cursor))
            continue;
        ++children;
        ++blocks;
    }

    const auto input_data = cursor + ui_probe_block_header_bytes;
    if (!append_block(memory, input_device, limits.input_device_bytes, 0, UiProbeBlockKind::input_device, true,
        buffer, cursor)) return 0;
    const auto input_captured = cursor - input_data;
    ++blocks;
    const auto input_scan = input_captured < limits.input_pointer_scan_bytes
        ? input_captured : limits.input_pointer_scan_bytes;
    std::size_t input_children = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= input_scan &&
         input_children < limits.max_input_children; offset += sizeof(std::uint64_t))
    {
        std::uint64_t value = 0;
        std::memcpy(&value, buffer + input_data + offset, sizeof(value));
        const auto pointer = static_cast<std::uintptr_t>(value);
        if (pointer < lowest_pointer || pointer > highest_user_pointer || (pointer & 0x7) != 0 ||
            pointer == engine || pointer == ui || pointer == input_device) continue;
        auto duplicate = false;
        for (std::size_t earlier = 0; earlier < offset && !duplicate; earlier += sizeof(std::uint64_t))
        {
            std::uint64_t previous = 0;
            std::memcpy(&previous, buffer + input_data + earlier, sizeof(previous));
            duplicate = previous == value;
        }
        if (duplicate) continue;
        if (!append_block(memory, pointer, limits.input_child_bytes, offset, UiProbeBlockKind::input_child, true,
            buffer, cursor)) continue;
        ++input_children;
        ++blocks;
    }

    std::memcpy(buffer, "GDUP", 4);
    put32(buffer, 4, ui_probe_version);
    put64(buffer, 8, cursor);
    put64(buffer, 16, mark.sequence);
    put64(buffer, 24, mark.tick);
    put64(buffer, 32, engine);
    put64(buffer, 40, ui);
    put32(buffer, 48, static_cast<std::uint32_t>(mark.label));
    put32(buffer, 52, mark.camera_mode);
    put32(buffer, 56, mark.foreground);
    put32(buffer, 60, blocks);
    return cursor;
}
