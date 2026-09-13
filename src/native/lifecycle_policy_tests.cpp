#include "lifecycle_policy.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace { void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); } }

int main()
{
    try
    {
        gdtpc::InitializationGate gate;
        std::atomic<unsigned> winners{};
        std::vector<std::thread> callers;
        for (unsigned i=0;i<32;++i) callers.emplace_back([&]{ if (gate.try_begin()) ++winners; });
        for (auto& caller: callers) caller.join();
        require(winners==1 && gate.phase()==1, "concurrent initialization had multiple owners");
        gate.publish_active(); require(!gate.try_begin() && gate.phase()==2, "active initialization was not idempotent");

        gdtpc::WriterOwnershipPolicy writer;
        writer.worker_started(); writer.stop_requested(); writer.join_result(false);
        require(writer.stop_requested_value() && writer.shutdown_pending(), "blocked write/flush was called clean");
        require(writer.worker_owns_file() && writer.thread_handle_open(), "timeout reclaimed worker-owned resources");
        writer.join_result(true);
        require(!writer.shutdown_pending() && !writer.worker_owns_file() && !writer.thread_handle_open(), "late completion did not release ownership");

        require(gdtpc::RemoteStoragePolicy{false,false}.may_free(), "not-started call retained storage");
        require(gdtpc::RemoteStoragePolicy{true,true}.may_free(), "completed call retained storage");
        const gdtpc::RemoteStoragePolicy unknown{true,false};
        require(!unknown.may_free() && unknown.retry_prohibited(), "unknown completion freed storage or allowed retry");

        gdtpc::SessionTracker sessions;
        require(sessions.observe({1,2,3},true)==1 && sessions.observe({1,2,3},true)==1, "stable session changed generation");
        require(sessions.observe({1,2,4},true)==2, "player replacement did not change generation");
        require(sessions.observe({1,2,4},false)==2, "invalid sample created a generation");
        require(sessions.observe({1,9,4},true)==3, "engine replacement did not change generation");

        std::cout << "PASS: concurrent init, remote-memory ownership, blocked writer/flush shutdown, late completion, and session generation.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
