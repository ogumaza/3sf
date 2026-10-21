// SPDX-License-Identifier: MIT

// The services that the process reaches through IPC, emulated at a high level (3dbrew, "Services"): the ports srv: and
// err:f, fs:USER with the RomFS files that it opens, dsp::DSP, cfg:u and APT:U. srv: gives a stub for any other
// service. A stub answers every command with success.
//
// The services keep their state in State. State refers to kernel objects by index. A snapshot copies State.

#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "dsp/dsp.h"
#include "dsp/teakra_dsp.h"
#include "horizon/ipc.h"
#include "horizon/kernel.h"
#include "horizon/romfs.h"

namespace threesf::horizon
{

class Services : public ServiceHost
{
public:
    // A stub for a service that isn't emulated: the name asked for, NULs included, and the headers that the stub has
    // logged.
    struct Stub
    {
        std::string name;
        std::set<uint32_t> logged;
    };

    // What the services keep between requests. The sets hold the headers (and for cfg:u the block ids too) that a
    // service logs once.
    struct State
    {
        uint32_t srv_semaphore = kNone; // srv:'s notification semaphore, made at the first EnableNotification
        std::vector<Stub> stubs;
        std::set<uint32_t> fs_logged;
        std::set<uint32_t> cfg_logged;
        uint32_t apt_lock = kNone;
        uint32_t apt_notification = kNone;
        uint32_t apt_parameter = kNone;
        std::set<uint32_t> apt_logged;
        uint32_t dsp_semaphore = kNone;                               // the DSP's semaphore event, with its hook
        std::array<uint32_t, 2> dsp_reply_events{kNone, kNone};       // for reply registers 0 and 1
        std::array<uint32_t, 3> dsp_pipe_events{kNone, kNone, kNone}; // for pipes 0, 1 and 2
        uint16_t dsp_mask = 0;                                        // the semaphore's preset mask
        std::set<uint32_t> dsp_logged;
    };

    // Installs the services: dsp::DSP with its semaphore event and its interrupt handler, the ports, and the services
    // that srv: finds by name. fs:USER serves the files as the RomFS. Makes no handles.
    Services(Kernel& kernel, dsp::TeakraDsp& dsp, std::vector<RomFs::File> files);
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;
    ~Services() override;

    uint32_t PortService(const std::string& name) const override;
    void HandleRequest(uint32_t service, uint32_t command_buffer) override;
    void RunSignalHook(uint32_t object) override;
    void MarkObjects(std::vector<bool>& live) const override;

    State Save() const;
    void Restore(const State& state);

private:
    void Srv(ipc::CommandBuffer& command);
    void GetServiceHandle(ipc::CommandBuffer& command);
    void ErrF(ipc::CommandBuffer& command);
    void FsUser(ipc::CommandBuffer& command);
    bool NamesRomFs(uint32_t archive, uint32_t path_type, uint32_t path_size, uint32_t path);
    uint32_t OpenRomFsFile();
    void RomFsFile(ipc::CommandBuffer& command);
    void ReadFile(ipc::CommandBuffer& command);
    void Cfg(ipc::CommandBuffer& command);
    void GetConfigInfoBlk2(ipc::CommandBuffer& command);
    void Apt(ipc::CommandBuffer& command);
    void Dsp(ipc::CommandBuffer& command);
    void WriteProcessPipe(ipc::CommandBuffer& command);
    void ReadPipe(ipc::CommandBuffer& command);
    void LoadComponent(ipc::CommandBuffer& command);
    void RegisterInterruptEvents(ipc::CommandBuffer& command);
    uint32_t* InterruptEvent(uint32_t interrupt, uint32_t pipe);
    void LogUnhandled(std::set<uint32_t>& logged, const char* service, uint32_t header);
    void StubRequest(Stub& stub, ipc::CommandBuffer& command);
    void OnInterrupt(dsp::Interrupt interrupt, dsp::Pipe pipe);

    Kernel& kernel_;
    dsp::TeakraDsp& dsp_;
    RomFs romfs_;
    State state_;
};

} // namespace threesf::horizon
