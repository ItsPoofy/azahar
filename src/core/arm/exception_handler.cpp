// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include "core/arm/exception_handler.h"

#include <atomic>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/memory.h"

namespace Core {

static bool g_ignore_exceptions_for_session = false;

void SetIgnoreExceptionsForSession(bool ignore) {
    g_ignore_exceptions_for_session = ignore;
}

bool AreExceptionsIgnoredForSession() {
    return g_ignore_exceptions_for_session;
}

static std::string DumpRegisters(const ARM_Interface& core) {
    std::string out;

    // General purpose registers: R0-R12
    for (int i = 0; i < 13; i += 3) {
        out += "    ";
        for (int j = 0; j < 3 && i + j < 13; j++) {
            const int reg = i + j;
            out += fmt::format("R{:<3}  0x{:08X}     ", reg, core.GetReg(reg));
        }
        out += '\n';
    }

    out += '\n';

    // SP, LR, PC
    out += fmt::format("    SP    0x{:08X}     LR    0x{:08X}     PC    0x{:08X}\n",
                       core.GetReg(13), core.GetReg(14), core.GetReg(15));

    // CPSR, FPEXC, FPSCR
    out += fmt::format("    CPSR  0x{:08X}     FPEXC 0x{:08X}     FPSCR 0x{:08X}\n", core.GetCPSR(),
                       core.GetVFPSystemReg(VFP_FPEXC), core.GetVFPSystemReg(VFP_FPSCR));

    // VFP single-precision registers S0-S31
    out += '\n';
    for (int i = 0; i < 32; i += 3) {
        out += "    ";
        for (int j = 0; j < 3 && i + j < 32; j++) {
            const int reg = i + j;
            out += fmt::format("S{:<3}  0x{:08X}     ", reg, core.GetVFPReg(reg));
        }
        out += '\n';
    }

    return out;
}

static std::string DumpStack(const ARM_Interface& core, Memory::MemorySystem& memory) {
    std::string out;
    constexpr u32 stack_dump_size = 256;
    const u32 sp = core.GetReg(13);

    for (u32 offset = 0; offset < stack_dump_size; offset += 16) {
        const u32 addr = sp + offset;
        out += fmt::format("    0x{:08X}:  ", addr);

        for (u32 byte_offset = 0; byte_offset < 16; byte_offset += 4) {
            auto word = memory.Read32OrNullopt(addr + byte_offset);
            if (word) {
                out += fmt::format("{:02X} {:02X} {:02X} {:02X}  ", (*word >> 0) & 0xFF,
                                   (*word >> 8) & 0xFF, (*word >> 16) & 0xFF, (*word >> 24) & 0xFF);
            } else {
                out += "?? ?? ?? ??  ";
            }
        }
        out += '\n';
    }

    return out;
}

static const char* ExceptionTypeToString(ExceptionType type) {
    switch (type) {
    case ExceptionType::UnmappedRead:
        return "Unmapped Read";
    case ExceptionType::UnmappedWrite:
        return "Unmapped Write";
    case ExceptionType::DataAbort:
        return "Data Abort";
    case ExceptionType::PrefetchAbort:
        return "Prefetch Abort";
    case ExceptionType::UndefinedInstruction:
        return "Undefined Instruction";
    case ExceptionType::Break:
        return "Break";
    default:
        return "Unknown";
    }
}

void LogException(System& system, ExceptionType type) {
    if (g_ignore_exceptions_for_session) {
        return;
    }

    static std::atomic<bool> s_in_handler{false};
    bool expected = false;
    if (!s_in_handler.compare_exchange_strong(expected, true)) {
        return;
    }
    struct HandlerGuard {
        std::atomic<bool>& flag;
        ~HandlerGuard() {
            flag.store(false);
        }
    } guard{s_in_handler};

    auto& core = system.GetRunningCore();
    auto& memory = system.Memory();

    std::string report;

    if (system.KernelRunning()) {
        auto& kernel = system.Kernel();
        auto thread = kernel.GetCurrentThreadManager().GetCurrentThread();
        if (thread) {
            report +=
                fmt::format("Thread: {} (ID: {})\n", thread->GetName(), thread->GetThreadId());
            auto process = thread->owner_process.lock();
            if (process) {
                report += fmt::format("Process: {}\n", process->GetName());
            }
        }
    }

    report += fmt::format("Exception Type: {}\n\n", ExceptionTypeToString(type));

    if (type == ExceptionType::PrefetchAbort && system.KernelRunning()) {
        const auto process = system.Kernel().GetCurrentProcess();
        if (process) {
            const auto& manager = process->vm_manager;
            const auto vma = manager.FindVMA(core.GetPC());
            const bool mapped = vma != manager.vma_map.end() &&
                                vma->second.type == Kernel::VMAType::BackingMemory;
            report += fmt::format("Fetch fault: {} at 0x{:08X}; Luma ForceRWXPages={}\n",
                                  mapped ? "execute permission" : "unmapped address", core.GetPC(),
                                  process->force_rwx_pages);
            if (mapped) {
                report += fmt::format("Mapping: 0x{:08X}-0x{:08X}, SVC permissions=0x{:X}\n",
                                      vma->second.base, vma->second.base + vma->second.size,
                                      static_cast<u32>(vma->second.permissions));
            }
            report += '\n';
        }
    }

    report += "Registers:\n\n";
    report += DumpRegisters(core);
    report += '\n';

    report += "Stack Dump:\n\n";
    report += DumpStack(core, memory);

    LOG_CRITICAL(Core, "\n{}", report);

    system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised, report.c_str());
    // Stop the active JIT slice at the fault instead of executing more guest instructions
    // with a fabricated zero read or a discarded write before RunLoop sees the error.
    core.PrepareReschedule();
}

} // namespace Core
