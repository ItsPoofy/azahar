// Local debugger integration of Luma3DS LayeredFS (GPL-3.0-or-later).
// Symbol discovery and guest payload:
// https://github.com/LumaTeam/Luma3DS/tree/master/sysmodules/loader/source

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/file_sys/luma_layered_fs.h"
#include "core/file_sys/ncch_container.h"

namespace FileSys {
namespace {
constexpr u32 Missing = std::numeric_limits<u32>::max();
constexpr std::array<u8, 0x114> Payload{
#include "core/file_sys/luma/romfsredir.inc"
};

u32 Word(const std::vector<u8>& code, u32 offset) {
    u32 value;
    std::memcpy(&value, code.data() + offset, sizeof(value));
    return value;
}

void Put(std::vector<u8>& code, u32 offset, u32 value) {
    std::memcpy(code.data() + offset, &value, sizeof(value));
}

u32 Branch(u32 from, u32 to, bool link = false) {
    return (link ? 0xeb000000 : 0xea000000) | (((to - from - 8) >> 2) & 0xffffff);
}

u32 FunctionStart(const std::vector<u8>& code, u32 offset) {
    while (offset >= 4) {
        offset -= 4;
        if ((Word(code, offset) >> 16) == 0xe92d)
            return offset;
    }
    return Missing;
}

u32 Rounded(u32 size) {
    return (size + 4095) & ~4095U;
}

std::string Directory(u64 program_id) {
    return fmt::format("{}luma/titles/{:016X}/romfs/",
                       FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir), GetModId(program_id));
}
} // namespace

bool HasLumaLayeredFs(u64 program_id) {
    return FileUtil::IsDirectory(Directory(program_id));
}

bool ApplyLumaLayeredFs(std::vector<u8>& code, const ExHeader_CodeSetInfo& info, u64 program_id) {
    if (!HasLumaLayeredFs(program_id))
        return true;

    const u32 size = info.text.code_size;
    const u32 rounded_text = Rounded(size);
    const u32 rounded_ro = Rounded(info.ro.code_size);
    if (size < 0x40 ||
        static_cast<u64>(rounded_text) + rounded_ro + Rounded(info.data.code_size) > code.size()) {
        LOG_ERROR(Loader, "Luma LayeredFS: invalid executable layout");
        return false;
    }

    u32 mount = Missing, unmount = Missing, reg = Missing, open = Missing, direct = Missing;
    for (u32 offset = 0; offset <= size - 0x40; offset += 4) {
        const auto w = [&](u32 index) { return Word(code, offset + index * 4); };
        u32* symbol = nullptr;
        switch (w(0)) {
        case 0xe5970010:
            if (w(1) == 0xe1cd20d8 && (w(2) & 0xffffff) == 0x008d0000)
                symbol = &mount;
            break;
        case 0xe24dd028:
            if (w(1) == 0xe1a04000 && w(2) == 0xe59f60a8 && w(3) == 0xe3a0c001)
                symbol = &mount;
            break;
        case 0xe2844001:
            if (w(1) == 0xe3540020 && w(2) == 0x3afffff0)
                symbol = &unmount;
            break;
        case 0xe353003a:
            if ((w(1) & 0xffffff0f) == 0x0a000009 && (w(2) & 0xffff0ff0) == 0xe1a00400)
                symbol = &unmount;
            break;
        case 0xe3500008:
            if ((w(1) & 0xfff00ff0) == 0xe1800400 && (w(2) & 0xfff00ff0) == 0xe1800fc0)
                symbol = &reg;
            break;
        case 0xe351003a:
            if (w(1) == 0x1afffffc && w(13) == 0xe590c000 && w(15) == 0xe12fff3c)
                symbol = &open;
            break;
        case 0x08030204:
            symbol = &direct;
            break;
        }
        if (symbol && *symbol == Missing)
            *symbol = FunctionStart(code, offset);
    }
    if (mount == Missing || reg == Missing || open == Missing || direct == Missing) {
        LOG_ERROR(Loader, "Luma LayeredFS: SDK file symbols not found");
        return false;
    }

    // Refuse to hide a payload collision behind the emulator's host RomFS merger.
    if (rounded_text - size < Payload.size() ||
        !std::all_of(
            code.begin() + size, code.begin() + size + Payload.size(),
            [](u8 value) { return value == 0; })) {
        LOG_ERROR(Loader, "Luma LayeredFS: insufficient unoccupied text padding");
        return false;
    }
    const std::string guest_path =
        fmt::format("lf:/luma/titles/{:016X}/romfs", GetModId(program_id));
    const u32 path_size = static_cast<u32>(guest_path.size() + 1);
    u32 path_offset, path_address;
    if (rounded_ro - info.ro.code_size >= path_size) {
        path_offset = rounded_text + info.ro.code_size;
        path_address = info.ro.address + info.ro.code_size;
    } else if (Rounded(info.data.code_size) - info.data.code_size >= path_size) {
        path_offset = rounded_text + rounded_ro + info.data.code_size;
        path_address = info.data.address + info.data.code_size;
    } else {
        LOG_ERROR(Loader, "Luma LayeredFS: insufficient path padding");
        return false;
    }

    std::vector<u8> payload(Payload.begin(), Payload.end());
    Put(payload, 4, Word(code, direct));
    Put(payload, 8, Branch(size + 8, direct + 4));
    Put(payload, 0x10, Word(code, open));
    Put(payload, 0x14, Branch(size + 0x14, open + 4));
    Put(payload, 0x28, unmount == Missing ? 0xe320f000 : Branch(size + 0x28, unmount, true));
    Put(payload, 0x38, Branch(size + 0x38, mount, true));
    Put(payload, 0x4c, Branch(size + 0x4c, reg, true));
    Put(payload, 0x104, 9); // ARCHIVE_SDMC, accessed by the guest SDK.
    constexpr std::array<std::string_view, 6> update_mounts{
        "ro2:", "rom2:", "rex:", "patch:", "ext:", "rom:"};
    auto update_mount = update_mounts.back();
    for (const auto candidate : update_mounts) {
        const std::string pattern = std::string(1, '\0') + std::string(candidate);
        if (std::search(code.begin(), code.end(), pattern.begin(), pattern.end()) != code.end()) {
            update_mount = candidate;
            break;
        }
    }
    std::memcpy(payload.data() + 0x10c, update_mount.data(), 4);
    Put(payload, 0x110, path_address);
    std::copy(payload.begin(), payload.end(), code.begin() + size);
    std::memcpy(code.data() + path_offset, guest_path.c_str(), path_size);
    Put(code, direct, Branch(direct, size));
    Put(code, open, Branch(open, size + 0xc));
    LOG_INFO(Loader, "Luma LayeredFS guest SDK path active: {} (payload at {:08X})",
             Directory(program_id), info.text.address + size);
    return true;
}
} // namespace FileSys
