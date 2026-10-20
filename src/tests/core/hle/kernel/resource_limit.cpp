// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/memory.h"

TEST_CASE("Resource budgets reject without changing accounting", "[core][kernel][accuracy]") {
    Core::Timing timing(1, 100);
    Core::System system;
    Memory::MemorySystem memory{system};
    Kernel::KernelSystem kernel(memory, timing, [] {}, Kernel::MemoryMode::NewProd, 1);
    auto limit = Kernel::ResourceLimit::Create(kernel, Kernel::ResourceLimitCategory::Application);
    for (auto kind : {Kernel::ResourceLimitType::Thread, Kernel::ResourceLimitType::Event,
                      Kernel::ResourceLimitType::Commit, Kernel::ResourceLimitType::SharedMemory}) {
        limit->SetLimitValue(kind, 3);
        CHECK(limit->Reserve(kind, 3));
        CHECK_FALSE(limit->Reserve(kind, 1));
        CHECK(limit->GetCurrentValue(kind) == 3);
        CHECK_FALSE(limit->Reserve(kind, -1));
        CHECK_FALSE(limit->Release(kind, 4));
        CHECK_FALSE(limit->Release(kind, -1));
        CHECK(limit->GetCurrentValue(kind) == 3);
        CHECK(limit->Release(kind, 3));
        CHECK(limit->GetCurrentValue(kind) == 0);
        CHECK(limit->Reserve(kind, 1));
    }
    auto kind = Kernel::ResourceLimitType::Commit;
    limit->SetLimitValue(kind, std::numeric_limits<s32>::max());
    limit->SetCurrentValue(kind, std::numeric_limits<s32>::max());
    CHECK_FALSE(limit->Reserve(kind, 1));
    CHECK(limit->GetCurrentValue(kind) == std::numeric_limits<s32>::max());
}

TEST_CASE("Luma quota-bypassing allocations do not release charged memory", "[core][kernel][accuracy]") {
    Core::Timing timing(1, 100);
    Core::System system;
    Memory::MemorySystem memory{system};
    Kernel::KernelSystem kernel(memory, timing, [] {}, Kernel::MemoryMode::NewProd, 1);
    auto process = kernel.CreateProcess(kernel.CreateCodeSet("", 0));
    process->flags.memory_region.Assign(Kernel::MemoryRegion::APPLICATION);
    process->memory_region = kernel.GetMemoryRegion(Kernel::MemoryRegion::APPLICATION);
    process->resource_limit = kernel.ResourceLimit().GetForCategory(Kernel::ResourceLimitCategory::Application);
    auto limit = process->resource_limit;
    limit->SetLimitValue(Kernel::ResourceLimitType::Commit, 0x2000);
    u32 address = 0;
    CHECK(process->HeapAllocate(&address, 0x08000000, 0x2000, Kernel::VMAPermission::ReadWrite) == ResultSuccess);
    CHECK(process->HeapAllocate(&address, 0x08002000, 0x1000, Kernel::VMAPermission::ReadWrite).IsError());
    CHECK(process->HeapAllocate(&address, 0x08002000, 0x1000, Kernel::VMAPermission::ReadWrite,
                                Kernel::MemoryState::Private, true, {}, true) == ResultSuccess);
    CHECK(limit->GetCurrentValue(Kernel::ResourceLimitType::Commit) == 0x2000);
    CHECK(process->HeapFree(0x08002000, 0x1000, true, true) == ResultSuccess);
    CHECK(limit->GetCurrentValue(Kernel::ResourceLimitType::Commit) == 0x2000);
    CHECK(process->HeapFree(0x08000000, 0x2000) == ResultSuccess);
    CHECK(limit->GetCurrentValue(Kernel::ResourceLimitType::Commit) == 0);
}

TEST_CASE("Failed process startup rolls back its committed memory", "[core][kernel][accuracy]") {
    Core::Timing timing(1, 100);
    Core::System system;
    Memory::MemorySystem memory{system};
    Kernel::KernelSystem kernel(memory, timing, [] {}, Kernel::MemoryMode::NewProd, 1);
    auto code = kernel.CreateCodeSet("", 0);
    code->memory.resize(0x2000);
    code->CodeSegment().addr = 0x100000;
    code->CodeSegment().size = 0x1000;
    code->RODataSegment().addr = 0x101000;
    code->RODataSegment().size = 0x1000;
    code->RODataSegment().offset = 0x1000;
    auto process = kernel.CreateProcess(code);
    process->flags.memory_region.Assign(Kernel::MemoryRegion::APPLICATION);
    process->resource_limit = kernel.ResourceLimit().GetForCategory(Kernel::ResourceLimitCategory::Application);
    process->resource_limit->SetLimitValue(Kernel::ResourceLimitType::Commit, 0x1000);
    CHECK_FALSE(process->Run(0x30, 0x1000));
    CHECK(process->resource_limit->GetCurrentValue(Kernel::ResourceLimitType::Commit) == 0);
    CHECK(process->resource_limit->GetCurrentValue(Kernel::ResourceLimitType::Thread) == 0);
    CHECK(process->memory_used == 0);
    CHECK_FALSE(memory.IsValidVirtualAddress(*process, 0x100000));
}
