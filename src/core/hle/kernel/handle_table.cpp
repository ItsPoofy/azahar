// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <utility>
#include <boost/serialization/array.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"

SERIALIZE_EXPORT_IMPL(Kernel::HandleTable)

namespace Kernel {
namespace {
constexpr u16 GetSlot(Handle handle) {
    return handle >> 15;
}

constexpr u16 GetGeneration(Handle handle) {
    return handle & 0x7FFF;
}
} // Anonymous namespace

HandleTable::HandleTable(KernelSystem& kernel) : kernel(kernel) {
    next_generation = 1;
    Clear();
}

HandleTable::~HandleTable() = default;

Result HandleTable::Create(Handle* out_handle, std::shared_ptr<Object> obj) {
    DEBUG_ASSERT(obj != nullptr);

    u16 slot = next_free_slot;
    R_UNLESS(slot < max_count, ResultOutOfHandles);
    next_free_slot = generations[slot];

    u16 generation = next_generation++;

    // Overflow count so it fits in the 15 bits dedicated to the generation in the handle.
    // CTR-OS doesn't use generation 0, so skip straight to 1.
    if (next_generation >= (1 << 15)) {
        next_generation = 1;
    }

    generations[slot] = generation;
    objects[slot] = std::move(obj);

    *out_handle = generation | (slot << 15);
    return ResultSuccess;
}

Result HandleTable::Duplicate(Handle* out_handle, Handle handle) {
    std::shared_ptr<Object> object = GetGeneric(handle);
    R_UNLESS(object, ResultInvalidHandle);
    return Create(out_handle, std::move(object));
}

Result HandleTable::Close(Handle handle) {
    R_UNLESS(IsValid(handle), ResultInvalidHandle);

    const u16 slot = GetSlot(handle);
    objects[slot] = nullptr;

    generations[slot] = next_free_slot;
    next_free_slot = slot;
    return ResultSuccess;
}

bool HandleTable::IsValid(Handle handle) const {
    const u16 slot = GetSlot(handle);
    const u16 generation = GetGeneration(handle);
    return slot < max_count && objects[slot] != nullptr && generations[slot] == generation;
}

std::shared_ptr<Object> HandleTable::GetGeneric(Handle handle) const {
    if (handle == CurrentThread) {
        return SharedFrom(kernel.GetCurrentThreadManager().GetCurrentThread());
    } else if (handle == CurrentProcess) {
        return kernel.GetCurrentProcess();
    }

    if (!IsValid(handle)) {
        return nullptr;
    }
    return objects[GetSlot(handle)];
}

std::vector<Handle> HandleTable::GetHandles(u32 type_token) const {
    const auto luma_token = [](HandleType type) -> u32 {
        switch (type) {
        case HandleType::Event:
            return 0x1F;
        case HandleType::Semaphore:
            return 0x2F;
        case HandleType::Timer:
            return 0x35;
        case HandleType::Mutex:
            return 0x39;
        case HandleType::ServerPort:
            return 0x55;
        case HandleType::ClientPort:
            return 0x65;
        case HandleType::CodeSet:
            return 0x68;
        case HandleType::Thread:
            return 0x8D;
        case HandleType::ServerSession:
            return 0x95;
        case HandleType::AddressArbiter:
            return 0x98;
        case HandleType::ClientSession:
            return 0xA5;
        case HandleType::SharedMemory:
            return 0xB0;
        case HandleType::Process:
            return 0xC5;
        case HandleType::ResourceLimit:
            return 0xC8;
        default:
            return 0;
        }
    };

    std::vector<Handle> handles;
    for (u32 slot = 0; slot < max_count; ++slot) {
        if (!objects[slot] || (type_token != 0 && luma_token(objects[slot]->GetHandleType()) != type_token)) {
            continue;
        }
        handles.push_back(generations[slot] | (slot << 15));
    }
    return handles;
}

void HandleTable::SetMaxCount(std::size_t count) {
    ASSERT(count <= MAX_COUNT);
    // A zero ExHeader size selects Kernel11's embedded table; it does not disable handles.
    max_count = count == 0 ? INTERNAL_COUNT : count;
}

void HandleTable::Clear() {
    for (u16 i = 0; i < MAX_COUNT; ++i) {
        generations[i] = i + 1;
        objects[i] = nullptr;
    }
    next_free_slot = 0;
}

template <class Archive>
void HandleTable::serialize(Archive& ar, const unsigned int) {
    ar & objects;
    ar & generations;
    ar & next_generation;
    ar & next_free_slot;
}
SERIALIZE_IMPL(HandleTable)

} // namespace Kernel
