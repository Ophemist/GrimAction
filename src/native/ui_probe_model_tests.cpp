#include "ui_probe_model.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace
{
void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct FakeRegion
{
    std::vector<std::uint8_t> bytes;
    bool private_memory{true};
};

// Regions are keyed by base address and never overlap. A read may run to the end of its region and
// no further, matching how the live adapter clamps to VirtualQuery's region.
class FakeMemory final : public gdtpc::UiProbeMemory
{
public:
    std::map<std::uintptr_t, FakeRegion> regions;
    std::uintptr_t fault_on_copy{};
    std::size_t queries{};

    bool query(const std::uintptr_t address, const std::size_t wanted, std::size_t& readable,
        bool& private_memory) noexcept override
    {
        ++queries;
        const auto* region = find(address);
        if (region == nullptr) return false;
        const auto available = region->second.bytes.size() - (address - region->first);
        readable = available < wanted ? available : wanted;
        private_memory = region->second.private_memory;
        return true;
    }

    bool copy(const std::uintptr_t address, std::uint8_t* destination, const std::size_t size) noexcept override
    {
        if (address == fault_on_copy) return false;
        const auto* region = find(address);
        if (region == nullptr || address - region->first + size > region->second.bytes.size()) return false;
        std::memcpy(destination, region->second.bytes.data() + (address - region->first), size);
        return true;
    }

    void put_pointer(const std::uintptr_t base, const std::size_t offset, const std::uint64_t value)
    {
        std::memcpy(regions.at(base).bytes.data() + offset, &value, sizeof(value));
    }

private:
    const std::pair<const std::uintptr_t, FakeRegion>* find(const std::uintptr_t address) const noexcept
    {
        auto it = regions.upper_bound(address);
        if (it == regions.begin()) return nullptr;
        --it;
        if (address >= it->first + it->second.bytes.size()) return nullptr;
        return &*it;
    }
};

std::uint32_t get32(const std::vector<std::uint8_t>& b, const std::size_t o)
{
    std::uint32_t v = 0; std::memcpy(&v, b.data() + o, sizeof(v)); return v;
}

std::uint64_t get64(const std::vector<std::uint8_t>& b, const std::size_t o)
{
    std::uint64_t v = 0; std::memcpy(&v, b.data() + o, sizeof(v)); return v;
}

struct ParsedBlock
{
    std::uint64_t address{}, parent{};
    std::uint32_t size{}, kind{};
    std::size_t data{};
};

std::vector<ParsedBlock> parse(const std::vector<std::uint8_t>& b, const std::size_t length)
{
    require(std::memcmp(b.data(), "GDUP", 4) == 0, "record magic missing");
    require(get32(b, 4) == gdtpc::ui_probe_version, "record version wrong");
    require(get64(b, 8) == length, "record length does not match the returned size");
    std::vector<ParsedBlock> blocks;
    std::size_t cursor = gdtpc::ui_probe_header_bytes;
    for (std::uint32_t i = 0; i < get32(b, 60); ++i)
    {
        ParsedBlock block{get64(b, cursor), get64(b, cursor + 8), get32(b, cursor + 16), get32(b, cursor + 20),
            cursor + gdtpc::ui_probe_block_header_bytes};
        cursor = block.data + block.size;
        require(cursor <= length, "a block runs past the record");
        blocks.push_back(block);
    }
    require(cursor == length, "the block count does not account for the whole record");
    return blocks;
}
}

int main()
{
    try
    {
        constexpr std::uintptr_t engine = 0x10000000;
        constexpr std::uintptr_t ui = 0x20000000;
        constexpr std::uintptr_t child = 0x30000000;
        constexpr std::uintptr_t image = 0x40000000;
        constexpr std::uintptr_t tiny_child = 0x50000000;
        constexpr std::uintptr_t input = 0x60000000;
        constexpr std::uintptr_t input_child = 0x61000000;
        gdtpc::UiProbeLimits limits{};
        limits.engine_bytes = 0x100;
        limits.ui_bytes = 0x200;
        limits.pointer_scan_bytes = 0x80;
        limits.child_bytes = 0x40;
        limits.max_children = 8;
        limits.input_device_bytes = 0x180;
        limits.input_pointer_scan_bytes = 0x80;
        limits.input_child_bytes = 0x40;
        limits.max_input_children = 8;

        FakeMemory memory;
        memory.regions[engine].bytes.assign(0x100, 0xE1);
        memory.regions[ui].bytes.assign(0x180, 0); // shorter than ui_bytes: must clamp, not fail
        memory.regions[child].bytes.assign(0x100, 0xC1);
        memory.regions[image] = FakeRegion{std::vector<std::uint8_t>(0x100, 0x11), false};
        memory.regions[tiny_child].bytes.assign(0x10, 0x7A);
        memory.regions[input].bytes.assign(0x140, 0xD1); // shorter than requested, like a region boundary
        memory.regions[input_child].bytes.assign(0x100, 0xA1);
        memory.regions[ui].bytes[0x100] = 0x55; // beyond the pointer scan, still captured as UI bytes
        memory.put_pointer(ui, 0x08, child);
        memory.put_pointer(ui, 0x10, image);           // mapped image (vtable-like): skipped
        memory.put_pointer(ui, 0x18, child);           // duplicate: captured once, at 0x08
        memory.put_pointer(ui, 0x20, 0x68000000);      // unmapped: skipped
        memory.put_pointer(ui, 0x28, child + 3);       // misaligned: skipped
        memory.put_pointer(ui, 0x30, engine);          // a root: never captured again as a child
        memory.put_pointer(ui, 0x38, tiny_child);      // region shorter than child_bytes: clamped
        memory.put_pointer(ui, 0x88, child + 0x10);    // beyond pointer_scan_bytes: not followed
        memory.put_pointer(input, 0x08, input_child);
        memory.put_pointer(input, 0x10, image);        // mapped image: skipped
        memory.put_pointer(input, 0x18, input_child);  // duplicate: captured once
        memory.put_pointer(input, 0x20, ui);           // another root: skipped
        memory.put_pointer(input, 0x88, input_child + 0x10); // beyond input scan: not followed

        const gdtpc::UiProbeMark mark{gdtpc::UiProbeLabel::open, 1234, 5678, 1, 1};
        std::vector<std::uint8_t> buffer(gdtpc::ui_probe_record_capacity(limits));
        const auto length = gdtpc::capture_ui_probe(memory, engine, ui, input, mark, limits, buffer.data(), buffer.size());
        require(length > 0, "readable engine, UI and input roots produced no record");
        const auto blocks = parse(buffer, length);

        require(get64(buffer, 16) == 1234 && get64(buffer, 24) == 5678, "sequence or tick not recorded");
        require(get64(buffer, 32) == engine && get64(buffer, 40) == ui, "root pointers not recorded");
        require(get32(buffer, 48) == 1 && get32(buffer, 52) == 1 && get32(buffer, 56) == 1, "mark fields not recorded");
        require(blocks.size() == 6, "expected engine, ui, input device, and exactly three followed children");
        require(blocks[0].kind == 0 && blocks[0].address == engine && blocks[0].size == 0x100, "engine block wrong");
        require(buffer[blocks[0].data] == 0xE1, "engine bytes not copied");
        require(blocks[1].kind == 1 && blocks[1].address == ui && blocks[1].size == 0x180, "UI block not clamped to its region");
        require(buffer[blocks[1].data + 0x100] == 0x55, "UI bytes beyond the scan were not copied");
        require(blocks[2].kind == 2 && blocks[2].address == child && blocks[2].parent == 0x08 && blocks[2].size == 0x40,
            "the first child was not keyed at its first referencing offset");
        require(buffer[blocks[2].data] == 0xC1, "child bytes not copied");
        require(blocks[3].address == tiny_child && blocks[3].parent == 0x38 && blocks[3].size == 0x10,
            "a short child region was not clamped");
        require(blocks[4].kind == 3 && blocks[4].address == input && blocks[4].size == 0x140,
            "input-device block was not captured and clamped");
        require(buffer[blocks[4].data] == 0xD1, "input-device bytes not copied");
        require(blocks[5].kind == 4 && blocks[5].address == input_child && blocks[5].parent == 0x08 &&
            blocks[5].size == 0x40 && buffer[blocks[5].data] == 0xA1, "input child was not captured or keyed correctly");

        // The child limit bounds the work regardless of how many pointers the UI object holds.
        limits.max_children = 1;
        buffer.assign(gdtpc::ui_probe_record_capacity(limits), 0);
        auto capped = gdtpc::capture_ui_probe(memory, engine, ui, input, mark, limits, buffer.data(), buffer.size());
        require(parse(buffer, capped).size() == 5, "max_children did not cap followed UI pointers");
        limits.max_children = 8;
        limits.max_input_children = 0;
        buffer.assign(gdtpc::ui_probe_record_capacity(limits), 0);
        capped = gdtpc::capture_ui_probe(memory, engine, ui, input, mark, limits, buffer.data(), buffer.size());
        require(parse(buffer, capped).size() == 5, "max_input_children did not independently cap followed input pointers");
        limits.max_input_children = 8;

        // A faulting child copy is skipped; the record stays well formed.
        memory.fault_on_copy = child;
        buffer.assign(gdtpc::ui_probe_record_capacity(limits), 0);
        auto faulted = gdtpc::capture_ui_probe(memory, engine, ui, input, mark, limits, buffer.data(), buffer.size());
        require(faulted > 0 && parse(buffer, faulted).size() == 5, "a faulting UI child broke the record");
        memory.fault_on_copy = 0;

        memory.fault_on_copy = input_child;
        buffer.assign(gdtpc::ui_probe_record_capacity(limits), 0);
        faulted = gdtpc::capture_ui_probe(memory, engine, ui, input, mark, limits, buffer.data(), buffer.size());
        require(faulted > 0 && parse(buffer, faulted).size() == 5, "a faulting input child broke the record");
        memory.fault_on_copy = 0;

        // Refusals: unreadable roots, invalid root pointers, and an undersized buffer write nothing.
        buffer.assign(gdtpc::ui_probe_record_capacity(limits), 0);
        require(gdtpc::capture_ui_probe(memory, 0x70000000, ui, input, mark, limits, buffer.data(), buffer.size()) == 0,
            "an unreadable engine was captured");
        require(gdtpc::capture_ui_probe(memory, engine, 0x70000000, input, mark, limits, buffer.data(), buffer.size()) == 0,
            "an unreadable UI object was captured");
        require(gdtpc::capture_ui_probe(memory, engine, 0, input, mark, limits, buffer.data(), buffer.size()) == 0,
            "a null UI pointer was captured");
        require(gdtpc::capture_ui_probe(memory, engine, ui, 0x70000000, mark, limits, buffer.data(), buffer.size()) == 0,
            "an unreadable input device was captured");
        require(gdtpc::capture_ui_probe(memory, engine, ui, 0, mark, limits, buffer.data(), buffer.size()) == 0,
            "a null input device was captured");
        require(gdtpc::capture_ui_probe(memory, 0xFFFF800000000000ULL, ui, input, mark, limits, buffer.data(), buffer.size()) == 0,
            "a kernel-range engine pointer was captured");
        require(gdtpc::capture_ui_probe(memory, engine, ui, input, mark, limits, buffer.data(), buffer.size() - 1) == 0,
            "an undersized buffer was written");
        require(gdtpc::capture_ui_probe(memory, engine, ui, input, mark, limits, nullptr, buffer.size()) == 0,
            "a null buffer was written");

        // The shipping limits fit the preallocated runtime buffer.
        require(gdtpc::ui_probe_record_capacity(gdtpc::UiProbeLimits{}) <= (4U << 20),
            "default probe limits exceed the 4 MiB runtime buffer");

        std::cout << "PASS: UI probe record layout, GameEngine/UI/InputDevice root capture with region clamping, "
                     "bounded heap-only children with alignment, duplicate, root, image, unmapped and scan-range "
                     "rejection, independent child caps, faulting-copy skip, and invalid-root/buffer refusal.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
