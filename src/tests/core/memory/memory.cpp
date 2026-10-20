// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <catch2/catch_test_macros.hpp>
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

TEST_CASE("memory.IsValidVirtualAddress", "[core][memory]") {
    Core::Timing timing(1, 100);
    Core::System system;
    Memory::MemorySystem memory{system};
    Kernel::KernelSystem kernel(memory, timing, [] {}, Kernel::MemoryMode::NewProd, 1);
    SECTION("these regions should not be mapped on an empty process") {
        auto process = kernel.CreateProcess(kernel.CreateCodeSet("", 0));
        CHECK(memory.IsValidVirtualAddress(*process, Memory::PROCESS_IMAGE_VADDR) == false);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::HEAP_VADDR) == false);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::LINEAR_HEAP_VADDR) == false);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::VRAM_VADDR) == false);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::CONFIG_MEMORY_VADDR) == false);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::SHARED_PAGE_VADDR) == false);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::TLS_AREA_VADDR) == false);
    }

    SECTION("CONFIG_MEMORY_VADDR and SHARED_PAGE_VADDR should be valid after mapping them") {
        auto process = kernel.CreateProcess(kernel.CreateCodeSet("", 0));
        kernel.MapSharedPages(process->vm_manager);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::CONFIG_MEMORY_VADDR) == true);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::SHARED_PAGE_VADDR) == true);
    }

    SECTION("special regions should be valid after mapping them") {
        auto process = kernel.CreateProcess(kernel.CreateCodeSet("", 0));
        SECTION("VRAM") {
            kernel.HandleSpecialMapping(process->vm_manager,
                                        {Memory::VRAM_VADDR, Memory::VRAM_SIZE, false, false});
            CHECK(memory.IsValidVirtualAddress(*process, Memory::VRAM_VADDR) == true);
        }

        SECTION("IO (Not yet implemented)") {
            kernel.HandleSpecialMapping(
                process->vm_manager, {Memory::IO_AREA_VADDR, Memory::IO_AREA_SIZE, false, false});
            CHECK_FALSE(memory.IsValidVirtualAddress(*process, Memory::IO_AREA_VADDR) == true);
        }
    }

    SECTION("Unmapping a VAddr should make it invalid") {
        auto process = kernel.CreateProcess(kernel.CreateCodeSet("", 0));
        kernel.MapSharedPages(process->vm_manager);
        process->vm_manager.UnmapRange(Memory::CONFIG_MEMORY_VADDR, Memory::CONFIG_MEMORY_SIZE);
        CHECK(memory.IsValidVirtualAddress(*process, Memory::CONFIG_MEMORY_VADDR) == false);
    }
}

TEST_CASE("Guest buffer ranges and Luma permissions", "[core][memory][accuracy]") {
    Core::Timing timing(1, 100);
    Core::System system;
    Memory::MemorySystem memory{system};
    Kernel::KernelSystem kernel(memory, timing, [] {}, Kernel::MemoryMode::NewProd, 1);
    auto process = kernel.CreateProcess(kernel.CreateCodeSet("", 0));
    auto backing = std::make_shared<BufferMem>(0x2000);
    REQUIRE(process->vm_manager.MapBackingMemory(0x10000000, MemoryRef(backing), 0x2000,
                                                Kernel::MemoryState::Private).Succeeded());
    CHECK(memory.IsValidVirtualRange(*process, 0x10000fff, 2, 3));
    CHECK_FALSE(memory.IsValidVirtualRange(*process, 0x10001fff, 2, 1));
    CHECK_FALSE(memory.IsValidVirtualRange(*process, 0xfffffff0, 0x40, 1));
    CHECK(memory.IsValidVirtualRange(*process, 0, 0));
    REQUIRE(process->vm_manager.ReprotectRange(0x10001000, 0x1000, Kernel::VMAPermission::Read)
                == ResultSuccess);
    CHECK(memory.IsValidVirtualRange(*process, 0x10000fff, 2, 1));
    CHECK_FALSE(memory.IsValidVirtualRange(*process, 0x10000fff, 2, 2));
    process->force_rwx_pages = true;
    CHECK(memory.IsValidVirtualRange(*process, 0x10000fff, 2, 2));
    CHECK_FALSE(memory.IsValidVirtualRange(*process, 0x10001fff, 2, 2));
}
