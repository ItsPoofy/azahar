// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
#include <catch2/catch_test_macros.hpp>
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/server_session.h"
#include "core/hle/kernel/event.h"
#include "core/hle/service/ptm/ptm_u.h"
#include "core/hle/service/service.h"
#include "core/memory.h"

namespace {
class TestService : public Service::ServiceFramework<TestService> {
public:
    TestService() : ServiceFramework("test:accuracy") {
        static const FunctionInfo handlers[]{{1, &TestService::Success, "Success"},
                                             {2, nullptr, "Missing"}};
        RegisterHandlers(handlers);
    }
    void Success(Kernel::HLERequestContext& context) {
        context.CommandBuffer()[0] = IPC::MakeHeader(1, 1, 0);
        context.CommandBuffer()[1] = ResultSuccess.raw;
    }
};
}

TEST_CASE("PTM alarm registrations retain events per client", "[core][service][accuracy]") {
    Core::Timing timing(1, 100);
    Core::System system;
    Memory::MemorySystem memory{system};
    Kernel::KernelSystem kernel(memory, timing, [] {}, Kernel::MemoryMode::NewProd, 1);
    auto module = std::make_shared<Service::PTM::Module>(system);
    auto service = std::make_shared<Service::PTM::PTM_U>(module);
    auto [first, first_client] = kernel.CreateSessionPair();
    auto [second, second_client] = kernel.CreateSessionPair();
    service->ClientConnected(first);
    service->ClientConnected(second);
    auto register_event = [&](std::shared_ptr<Kernel::ServerSession> session,
                              std::shared_ptr<Kernel::Event> event) {
        Kernel::HLERequestContext context(kernel, session, nullptr);
        auto* buffer = context.CommandBuffer();
        buffer[0] = IPC::MakeHeader(1, 0, 2);
        buffer[1] = IPC::CopyHandleDesc(1);
        buffer[2] = context.AddOutgoingHandle(event);
        service->HandleSyncRequest(context);
        CHECK(buffer[1] == ResultSuccess.raw);
    };
    auto event = kernel.CreateEvent(Kernel::ResetType::OneShot);
    std::weak_ptr<Kernel::Event> weak = event;
    register_event(first, event);
    event.reset();
    CHECK_FALSE(weak.expired());
    register_event(second, kernel.CreateEvent(Kernel::ResetType::OneShot));
    CHECK_FALSE(weak.expired());
    register_event(first, nullptr);
    CHECK(weak.expired());
    service->ClientDisconnected(first);
    service->ClientDisconnected(second);
}
TEST_CASE("Unimplemented service calls cannot masquerade as success", "[core][service][accuracy]") {
    Core::Timing timing(1, 100);
    Core::System system;
    Memory::MemorySystem memory{system};
    Kernel::KernelSystem kernel(memory, timing, [] {}, Kernel::MemoryMode::NewProd, 1);
    auto [server, client] = kernel.CreateSessionPair();
    Kernel::HLERequestContext context(kernel, std::move(server), nullptr);
    TestService service;
    for (u32 command : {1, 2, 3}) {
        context.CommandBuffer()[0] = IPC::MakeHeader(command, 0, 0);
        service.HandleSyncRequest(context);
        CHECK(context.CommandHeader().normal_params_size == 1);
        CHECK(context.CommandHeader().translate_params_size == 0);
        if (command == 1) CHECK(context.CommandBuffer()[1] == 0);
        if (command == 2) CHECK(context.CommandBuffer()[1] == UnimplementedFunction(ErrorModule::OS).raw);
        if (command == 3) CHECK(context.CommandBuffer()[1] == 0xd900182f);
    }
}
