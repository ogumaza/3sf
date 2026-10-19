// SPDX-License-Identifier: MIT

// The services (see services.h). Each handler reads the parameters it uses from fixed words of the request, whatever
// the header's counts say, before it writes the reply.

#include "horizon/services.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "dsp/dsp.h"
#include "dsp/teakra_dsp.h"
#include "horizon/ipc.h"
#include "horizon/kernel.h"
#include "horizon/romfs.h"

namespace threesf::horizon
{
namespace
{

using ipc::CopyHandles;
using ipc::MoveHandles;

// The services that sessions refer to. The stubs that srv: makes come after them.
enum ServiceId : uint32_t
{
    kSrv,
    kErrF,
    kFsUser,
    kRomFsFile,
    kDsp,
    kCfg,
    kApt,
    kFirstStub,
};

// A service that the process finds by name.
struct NamedService
{
    const char* name;
    ServiceId service;
};

// The ports that ConnectToPort finds.
constexpr std::array<NamedService, 2> kPorts = {{
    {"srv:", kSrv},
    {"err:f", kErrF},
}};

// The services that srv: finds. Ports aren't among them.
constexpr std::array<NamedService, 4> kRegistered = {{
    {"fs:USER", kFsUser},
    {"dsp::DSP", kDsp},
    {"cfg:u", kCfg},
    {"APT:U", kApt},
}};

// fs:USER's results: a path that doesn't name the RomFS, and a command that a file doesn't handle.
constexpr uint32_t kFsNotFound = 0xc8804478;
constexpr uint32_t kFsUnhandled = 0xe0c046be;

// The archive that holds the RomFS, a binary path, and the handle that OpenArchive gives for it.
constexpr uint32_t kRomFsArchive = 3;
constexpr uint32_t kBinaryPath = 2;
constexpr uint64_t kRomFsArchiveHandle = 0x0000003000000003;

// srv:'s notification semaphore.
constexpr int32_t kSrvSemaphoreMaximum = 64;

// The ARM11's view of DSP data memory (ConvertProcessAddressFromDspDram).
constexpr uint32_t kDspDataMemory = 0x1ff40000;

// cfg:u's configuration blocks (GetConfigInfoBlk2). A block that isn't here reads as zeros.
struct ConfigBlock
{
    uint32_t id;
    std::vector<uint8_t> contents;
};

const std::vector<ConfigBlock>& ConfigBlocks()
{
    static const std::vector<ConfigBlock> kBlocks = {
        {0x00070001, {0x01}},                   // sound output mode: stereo
        {0x000a0002, {0x00}},                   // language: Japanese
        {0x000b0000, {0x00, 0x00, 0x00, 0x01}}, // country: Japan
        // Stereo camera settings: eight floats (62.0, 289.0, 76.8, 46.08, 10.0, 5.0, 55.58, 21.57).
        {0x00050005, {0x00, 0x00, 0x78, 0x42, 0x00, 0x80, 0x90, 0x43, 0x9a, 0x99, 0x99, 0x42, 0xec, 0x51, 0x38, 0x42,
                      0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0xa0, 0x40, 0xec, 0x51, 0x5e, 0x42, 0x5c, 0x8f, 0xac, 0x41}},
        {0x000d0000, {0x7f, 0x7f, 0x00, 0x00}},                         // EULA version
        {0x000a0000, {0x33, 0x00, 0x53, 0x00, 0x46, 0x00, 0x00, 0x00}}, // user name: "3SF" in UTF-16LE
        {0x000a0001, {0x01, 0x01}},                                     // birthday: 1 January
    };

    return kBlocks;
}

// The service in `table` with the name `name`, or kNone.
uint32_t FindService(std::span<const NamedService> table, const std::string& name)
{
    for (const NamedService& entry : table)
    {
        if (name == entry.name)
        {
            return entry.service;
        }
    }

    return kNone;
}

void Mark(std::vector<bool>& live, uint32_t object)
{
    if (object != kNone)
    {
        live[object] = true;
    }
}

} // namespace

Services::Services(Kernel& kernel, dsp::TeakraDsp& dsp, std::vector<RomFs::File> files)
    : kernel_(kernel), dsp_(dsp), romfs_(std::move(files))
{
    state_.dsp_semaphore = kernel_.NewEvent(0);
    kernel_.SetSignalHook(state_.dsp_semaphore);
    dsp_.SetInterruptHandler([this](dsp::Interrupt interrupt, dsp::Pipe pipe) { OnInterrupt(interrupt, pipe); });
}

// The DSP's interrupt handler calls the services. The DSP can outlive them. The handler therefore goes with them.
Services::~Services()
{
    dsp_.SetInterruptHandler({});
}

uint32_t Services::PortService(const std::string& name) const
{
    return FindService(kPorts, name);
}

void Services::HandleRequest(uint32_t service, uint32_t command_buffer)
{
    ipc::CommandBuffer command(kernel_.Memory(), command_buffer);
    switch (service)
    {
    case kSrv:
        Srv(command);
        break;

    case kErrF:
        ErrF(command);
        break;

    case kFsUser:
        FsUser(command);
        break;

    case kRomFsFile:
        RomFsFile(command);
        break;

    case kDsp:
        {
            // The calling thread waits while the DSP runs during a command. The ARM11 clock runs at twice the DSP's.
            const uint64_t cycles = dsp_.Cycles();
            Dsp(command);
            kernel_.AddPending(2 * (dsp_.Cycles() - cycles));
            break;
        }

    case kCfg:
        Cfg(command);
        break;

    case kApt:
        Apt(command);
        break;

    default:
        StubRequest(state_.stubs[service - kFirstStub], command);
        break;
    }
}

// The guest's signal of the DSP's semaphore event first sets the DSP's semaphore to the preset mask.
void Services::RunSignalHook(uint32_t object)
{
    if (object == state_.dsp_semaphore)
    {
        dsp_.SetSemaphore(state_.dsp_mask);
    }
}

void Services::MarkObjects(std::vector<bool>& live) const
{
    Mark(live, state_.srv_semaphore);
    Mark(live, state_.apt_lock);
    Mark(live, state_.apt_notification);
    Mark(live, state_.apt_parameter);
    Mark(live, state_.dsp_semaphore);
    for (const uint32_t event : state_.dsp_reply_events)
    {
        Mark(live, event);
    }

    for (const uint32_t event : state_.dsp_pipe_events)
    {
        Mark(live, event);
    }
}

Services::State Services::Save() const
{
    return state_;
}

void Services::Restore(const State& state)
{
    state_ = state;
}

void Services::Srv(ipc::CommandBuffer& command)
{
    switch (command.CommandId())
    {
    case 0x0001: // RegisterClient
        command.Reply(1, 0, {kSuccess});
        break;

    case 0x0002: // EnableNotification
        if (state_.srv_semaphore == kNone)
        {
            state_.srv_semaphore = kernel_.NewSemaphore(0, kSrvSemaphoreMaximum);
        }

        command.Reply(1, 2, {kSuccess, CopyHandles(1), kernel_.NewHandle(state_.srv_semaphore)});
        break;

    case 0x0005:
        GetServiceHandle(command);
        break;

    case 0x000b: // ReceiveNotification
        command.Reply(2, 0, {kSuccess, 0});
        break;

    default:
        kernel_.Log("[srv:] command %08x (stubbed)", command.RequestHeader());
        command.Reply(1, 0, {kSuccess});
        break;
    }
}

// A session to the service with the name in words 1 and 2, cut to the length in word 3. A name that no service has,
// NULs and ports included, gets a stub, made at the first request for it.
void Services::GetServiceHandle(ipc::CommandBuffer& command)
{
    uint8_t bytes[8];
    xsflib::StoreLe32(bytes, command.Word(1));
    xsflib::StoreLe32(bytes + 4, command.Word(2));
    const uint32_t length = std::min<uint32_t>(command.Word(3), sizeof(bytes));
    const std::string name(reinterpret_cast<const char*>(bytes), length);

    uint32_t service = FindService(kRegistered, name);
    if (service == kNone)
    {
        const auto stub =
            std::find_if(state_.stubs.begin(), state_.stubs.end(), [&](const Stub& s) { return s.name == name; });
        service = kFirstStub + static_cast<uint32_t>(stub - state_.stubs.begin());
        if (stub == state_.stubs.end())
        {
            kernel_.Log("[srv:] GetServiceHandle(\"%s\"): not emulated, using a stub", name.c_str());
            state_.stubs.push_back({name, {}});
        }
    }

    command.Reply(1, 2, {kSuccess, MoveHandles(1), kernel_.NewHandle(kernel_.NewSession(service))});
}

void Services::ErrF(ipc::CommandBuffer& command)
{
    if (command.CommandId() != 0x0001)
    {
        command.Reply(1, 0, {kSuccess});
        return;
    }

    // ThrowFatalError: the error's type, result, pc and lr, and every word of the error's information.
    std::array<uint32_t, 33> words{};
    for (uint32_t i = 1; i < words.size(); i++)
    {
        words[i] = command.Word(i);
    }

    const uint32_t type = words[1] & 0xff;
    kernel_.Log("[err:f] fatal error type %u, result %08x, pc %08x, lr %08x (thread %u)", type, words[2], words[3],
                type <= 1 ? words[9] : 0, kernel_.CurrentThreadId());
    std::string info = "[err:f] info: ";
    for (uint32_t i = 1; i < words.size(); i++)
    {
        char word[16];
        std::snprintf(word, sizeof(word), "%08x ", words[i]);
        info += word;
    }

    kernel_.Log("%s", info.c_str());
    command.Reply(1, 0, {kSuccess});
}

void Services::FsUser(ipc::CommandBuffer& command)
{
    switch (command.CommandId())
    {
    case 0x0801: // Initialize
    case 0x0861: // InitializeWithSdkVersion
    case 0x0862: // SetPriority
    case 0x080e: // CloseArchive
        command.Reply(1, 0, {kSuccess});
        break;

    case 0x0863: // GetPriority
        command.Reply(2, 0, {kSuccess, 0});
        break;

    case 0x0803: // OpenFileDirectly
        {
            const uint32_t archive = command.Word(2);
            const uint32_t path_type = command.Word(5);
            if (!NamesRomFs(archive, path_type, command.Word(6), command.Word(12)))
            {
                kernel_.Log("[fs:USER] OpenFileDirectly(archive %x, path type %u): not found", archive, path_type);
                command.Reply(1, 0, {kFsNotFound});
                break;
            }

            command.Reply(1, 2, {kSuccess, MoveHandles(1), OpenRomFsFile()});
            break;
        }

    case 0x080c: // OpenArchive
        {
            const uint32_t archive = command.Word(1);
            if (archive != kRomFsArchive)
            {
                kernel_.Log("[fs:USER] OpenArchive(%x): not supported", archive);
                command.Reply(1, 0, {kFsNotFound});
                break;
            }

            command.Reply(3, 0,
                          {kSuccess, static_cast<uint32_t>(kRomFsArchiveHandle),
                           static_cast<uint32_t>(kRomFsArchiveHandle >> 32)});
            break;
        }

    case 0x0802: // OpenFile, in the RomFS whatever archive handle the request gives
        if (!NamesRomFs(kRomFsArchive, command.Word(4), command.Word(5), command.Word(9)))
        {
            command.Reply(1, 0, {kFsNotFound});
            break;
        }

        command.Reply(1, 2, {kSuccess, MoveHandles(1), OpenRomFsFile()});
        break;

    case 0x0817: // IsSdmcDetected
    case 0x0818: // IsSdmcWritable
        command.Reply(2, 0, {kSuccess, 0});
        break;

    default:
        LogUnhandled(state_.fs_logged, "fs:USER", command.RequestHeader());
        command.Reply(1, 0, {kSuccess});
        break;
    }
}

// A path names the RomFS in archive 3, unless it's a binary path whose first word isn't 0. Such a path names the ExeFS
// or the code. 3SF provides neither.
bool Services::NamesRomFs(uint32_t archive, uint32_t path_type, uint32_t path_size, uint32_t path)
{
    if (archive != kRomFsArchive)
    {
        return false;
    }

    return !(path_type == kBinaryPath && path_size >= 4 && kernel_.Read32(path) != 0);
}

// Opens a RomFS file. Returns the handle of a new session with the file.
uint32_t Services::OpenRomFsFile()
{
    return kernel_.NewHandle(kernel_.NewSession(kRomFsFile));
}

// A RomFS file: the whole image. A file keeps no position or other state.
void Services::RomFsFile(ipc::CommandBuffer& command)
{
    switch (command.CommandId())
    {
    case 0x0802:
        ReadFile(command);
        break;

    case 0x0804: // GetSize
        command.Reply(3, 0,
                      {kSuccess, static_cast<uint32_t>(romfs_.Size()), static_cast<uint32_t>(romfs_.Size() >> 32)});
        break;

    case 0x0808: // Close: the session stays open
    case 0x0809: // Flush
    case 0x080a: // SetPriority
        command.Reply(1, 0, {kSuccess});
        break;

    case 0x080b: // GetPriority
        command.Reply(2, 0, {kSuccess, 0});
        break;

    case 0x080c: // OpenLinkFile
        command.Reply(1, 2, {kSuccess, MoveHandles(1), OpenRomFsFile()});
        break;

    default:
        kernel_.Log("[fs:USER file] unhandled command %08x", command.RequestHeader());
        command.Reply(1, 0, {kFsUnhandled});
        break;
    }
}

// Reads the image from the offset in words 1 and 2, up to the size in word 3. The bytes go to the buffer as a block
// copy. The copy stops at the first unmapped page. The reply counts the bytes that the read covers, mapped or not.
void Services::ReadFile(ipc::CommandBuffer& command)
{
    const uint64_t offset = command.Word(1) | uint64_t{command.Word(2)} << 32;
    const uint32_t size = command.Word(3);
    const uint32_t descriptor = command.Word(4);
    const uint32_t buffer = command.Word(5);

    const uint64_t count = offset < romfs_.Size() ? std::min<uint64_t>(size, romfs_.Size() - offset) : 0;
    const std::size_t copied = kernel_.Memory().MappedLength(buffer, static_cast<std::size_t>(count));
    std::vector<uint8_t> bytes(copied);
    romfs_.Read(offset, copied, bytes.data());
    kernel_.Memory().WriteBlock(buffer, bytes.data(), bytes.size());
    command.Reply(2, 2, {kSuccess, static_cast<uint32_t>(count), descriptor, buffer});
}

void Services::Cfg(ipc::CommandBuffer& command)
{
    switch (command.CommandId())
    {
    case 0x0001:
        GetConfigInfoBlk2(command);
        break;

    case 0x0002: // SecureInfoGetRegion: Japan
        command.Reply(2, 0, {kSuccess, 0});
        break;

    case 0x0003: // GenHashConsoleUnique
        command.Reply(3, 0, {kSuccess, 0x12345678, 0x9abcdef0});
        break;

    case 0x0004: // GetRegionCanadaUSA: neither
    case 0x0005: // GetSystemModel: an original 3DS
        command.Reply(2, 0, {kSuccess, 0});
        break;

    case 0x0006: // GetModelNintendo2DS: not a 2DS
        command.Reply(2, 0, {kSuccess, 1});
        break;

    default:
        LogUnhandled(state_.cfg_logged, "cfg:u", command.RequestHeader());
        command.Reply(1, 0, {kSuccess});
        break;
    }
}

// The buffer gets as much of the contents of the block with the id in word 2 as the size in word 1 allows. Zeros fill
// the rest of the size. Block copies write both. They stop at the first unmapped page. cfg:u logs the first request
// for a missing block. The same set records both those block ids and the headers of unhandled commands.
void Services::GetConfigInfoBlk2(ipc::CommandBuffer& command)
{
    const uint32_t size = command.Word(1);
    const uint32_t id = command.Word(2);
    const uint32_t descriptor = command.Word(3);
    const uint32_t address = command.Word(4);

    const std::vector<ConfigBlock>& blocks = ConfigBlocks();
    const auto block = std::find_if(blocks.begin(), blocks.end(), [id](const ConfigBlock& b) { return b.id == id; });
    if (block == blocks.end() && state_.cfg_logged.insert(id).second)
    {
        kernel_.Log("[cfg:u] config block %08x (size %u): zeroes", id, size);
    }

    const auto mapped = static_cast<uint32_t>(kernel_.Memory().MappedLength(address, size));
    const uint32_t contents =
        block == blocks.end() ? 0 : std::min(mapped, static_cast<uint32_t>(block->contents.size()));
    if (contents > 0)
    {
        kernel_.Memory().WriteBlock(address, block->contents.data(), contents);
    }

    kernel_.Memory().ZeroBlock(address + contents, mapped - contents);
    command.Reply(1, 2, {kSuccess, descriptor, address});
}

void Services::Apt(ipc::CommandBuffer& command)
{
    switch (command.CommandId())
    {
    case 0x0001: // GetLockHandle: no applet attributes, the power button not pressed, and APT's lock
        if (state_.apt_lock == kNone)
        {
            state_.apt_lock = kernel_.NewMutex();
        }

        command.Reply(3, 2, {kSuccess, 0, 0, CopyHandles(1), kernel_.NewHandle(state_.apt_lock)});
        break;

    case 0x0002: // Initialize: the notification event and the parameter event
        if (state_.apt_notification == kNone)
        {
            state_.apt_notification = kernel_.NewEvent(0);
        }

        if (state_.apt_parameter == kNone)
        {
            state_.apt_parameter = kernel_.NewEvent(0);
        }

        command.Reply(1, 3,
                      {kSuccess, CopyHandles(2), kernel_.NewHandle(state_.apt_notification),
                       kernel_.NewHandle(state_.apt_parameter)});
        break;

    case 0x0003: // Enable: the first parameter is a wakeup
        if (state_.apt_parameter != kNone)
        {
            kernel_.Signal(state_.apt_parameter);
        }

        command.Reply(1, 0, {kSuccess});
        break;

    case 0x000b: // InquireNotification
        command.Reply(2, 0, {kSuccess, 0});
        break;

    case 0x000d: // ReceiveParameter
    case 0x000e: // GlanceParameter: a wakeup from no sender, with no handle and no data
        command.Reply(4, 4, {kSuccess, 0, 1, 0, MoveHandles(1), 0, ipc::StaticBuffer(0, 0), 0});
        break;

    default:
        LogUnhandled(state_.apt_logged, "APT:U", command.RequestHeader());
        command.Reply(3, 0, {kSuccess, 0, 0});
        break;
    }
}

void Services::Dsp(ipc::CommandBuffer& command)
{
    switch (command.CommandId())
    {
    case 0x0001: // RecvData
        {
            const uint16_t value = dsp_.ReadReply(command.Word(1));
            command.Reply(2, 0, {kSuccess, value});
            break;
        }

    case 0x0002: // RecvDataIsReady
        {
            const bool ready = dsp_.ReplyReady(command.Word(1));
            command.Reply(2, 0, {kSuccess, ready ? 1u : 0u});
            break;
        }

    case 0x0007: // SetSemaphore
        dsp_.SetSemaphore(static_cast<uint16_t>(command.Word(1)));
        command.Reply(1, 0, {kSuccess});
        break;

    case 0x000c: // ConvertProcessAddressFromDspDram
        {
            const uint32_t address = command.Word(1);
            command.Reply(2, 0, {kSuccess, kDspDataMemory + 2 * address});
            break;
        }

    case 0x000d:
        WriteProcessPipe(command);
        break;

    case 0x000e: // ReadPipe
    case 0x0010: // ReadPipeIfPossible
        ReadPipe(command);
        break;

    case 0x000f: // GetPipeReadableSize
        {
            const auto pipe = static_cast<dsp::Pipe>(command.Word(1) & 0xff);
            const auto readable = static_cast<uint32_t>(dsp_.PipeReadable(pipe) & 0xffff);
            command.Reply(2, 0, {kSuccess, readable});
            break;
        }

    case 0x0011:
        LoadComponent(command);
        break;

    case 0x0012: // UnloadComponent
        dsp_.UnloadComponent();
        command.Reply(1, 0, {kSuccess});
        break;

    case 0x0013: // FlushDataCache
    case 0x0014: // InvalidateDataCache
    case 0x0020: // ForceHeadphoneOut
        command.Reply(1, 0, {kSuccess});
        break;

    case 0x0015:
        RegisterInterruptEvents(command);
        break;

    case 0x0016: // GetSemaphoreEventHandle
        command.Reply(1, 2, {kSuccess, CopyHandles(1), kernel_.NewHandle(state_.dsp_semaphore)});
        break;

    case 0x0017: // SetSemaphoreMask
        state_.dsp_mask = static_cast<uint16_t>(command.Word(1));
        command.Reply(1, 0, {kSuccess});
        break;

    case 0x001f: // GetHeadphoneStatus
    case 0x0021: // GetIsDspOccupied
        command.Reply(2, 0, {kSuccess, 0});
        break;

    default:
        LogUnhandled(state_.dsp_logged, "dsp::DSP", command.RequestHeader());
        command.Reply(1, 0, {kSuccess});
        break;
    }
}

// Writes as much of the static buffer's data to a pipe as both sizes allow. With a channel word of 2 or 3, some bytes
// of the data get fixed values first. Games leave stack garbage in those bytes. The console's DSP module overwrites
// them. The pipe is the channel word's low 8 bits.
void Services::WriteProcessPipe(ipc::CommandBuffer& command)
{
    const uint32_t channel = command.Word(1);
    const uint32_t size = command.Word(2);
    const uint32_t descriptor = command.Word(3);
    const uint32_t address = command.Word(4);

    std::vector<uint8_t> data(std::min(size, descriptor >> 14), 0);
    kernel_.Memory().ReadBlock(address, data.data(), data.size());
    if (channel == 2 && data.size() >= 4)
    {
        data[2] = 0;
        data[3] = 0;
    }
    else if (channel == 3 && data.size() >= 8)
    {
        data[4] = 1;
        data[5] = 0;
        data[6] = 0;
        data[7] = 0;
    }

    dsp_.WritePipe(static_cast<dsp::Pipe>(channel & 0xff), data);
    command.Reply(1, 0, {kSuccess});
}

// ReadPipe and ReadPipeIfPossible: the data, if the pipe holds the size asked for, goes to the caller's receive buffer
// 0. Neither waits for data.
void Services::ReadPipe(ipc::CommandBuffer& command)
{
    const auto pipe = static_cast<dsp::Pipe>(command.Word(1) & 0xff);
    const uint32_t size = command.Word(3) & 0xffff;

    std::vector<uint8_t> data;
    if (dsp_.PipeReadable(pipe) >= size)
    {
        data = dsp_.ReadPipe(pipe, size);
    }

    const auto count = static_cast<uint32_t>(data.size());
    const uint32_t buffer = kernel_.Read32(kernel_.CurrentTls() + ipc::kReceiveBuffers + 4);
    kernel_.Memory().WriteBlock(buffer, data.data(), data.size());
    if (command.CommandId() == 0x0010)
    {
        command.Reply(2, 2, {kSuccess, count, ipc::StaticBuffer(count, 0), buffer});
    }
    else
    {
        command.Reply(1, 2, {kSuccess, ipc::StaticBuffer(count, 0), buffer});
    }
}

// Loads a DSP firmware image from the buffer, with zeros after an unmapped page. The masks are ignored.
void Services::LoadComponent(ipc::CommandBuffer& command)
{
    const uint32_t size = command.Word(1);
    const uint32_t descriptor = command.Word(4);
    const uint32_t address = command.Word(5);

    std::vector<uint8_t> image(size, 0);
    kernel_.Memory().ReadBlock(address, image.data(), image.size());
    dsp_.LoadComponent(image);
    command.Reply(2, 2, {kSuccess, 1, descriptor, address});
    kernel_.Log("[dsp::DSP] LoadComponent: %u bytes", size);
}

// Sets the event that an interrupt signals. A handle that doesn't refer to an event unsets the event. An interrupt
// without an event slot changes nothing.
void Services::RegisterInterruptEvents(ipc::CommandBuffer& command)
{
    const uint32_t interrupt = command.Word(1);
    const uint32_t channel = command.Word(2);
    const uint32_t object = kernel_.Lookup(command.Word(4));

    uint32_t* const event = InterruptEvent(interrupt, channel);
    if (event != nullptr)
    {
        *event = kernel_.IsEvent(object) ? object : kNone;
    }

    command.Reply(1, 0, {kSuccess});
}

// The slot of the event that an interrupt signals: for reply register 0 or 1, or for a pipe below 3. Null for any
// other interrupt or pipe.
uint32_t* Services::InterruptEvent(uint32_t interrupt, uint32_t pipe)
{
    if (interrupt < state_.dsp_reply_events.size())
    {
        return &state_.dsp_reply_events[interrupt];
    }

    if (interrupt == static_cast<uint32_t>(dsp::Interrupt::kPipe) && pipe < state_.dsp_pipe_events.size())
    {
        return &state_.dsp_pipe_events[pipe];
    }

    return nullptr;
}

// Logs a command that `service` doesn't handle, the first time that `logged` gets its header.
void Services::LogUnhandled(std::set<uint32_t>& logged, const char* service, uint32_t header)
{
    if (logged.insert(header).second)
    {
        kernel_.Log("[%s] unhandled command %08x (stubbed)", service, header);
    }
}

void Services::StubRequest(Stub& stub, ipc::CommandBuffer& command)
{
    const uint32_t header = command.RequestHeader();
    if (stub.logged.insert(header).second)
    {
        kernel_.Log("[%s] unhandled command %08x (stubbed, returns success)", stub.name.c_str(), header);
    }

    command.Reply(5, 0, {kSuccess, 0, 0, 0, 0});
}

// An interrupt from the DSP signals the event registered for it. Data in a reply register without an event is read
// and dropped.
void Services::OnInterrupt(dsp::Interrupt interrupt, dsp::Pipe pipe)
{
    const uint32_t* const event = InterruptEvent(static_cast<uint32_t>(interrupt), static_cast<uint32_t>(pipe));
    if (event != nullptr && *event != kNone)
    {
        kernel_.Signal(*event);
    }
    else if (interrupt == dsp::Interrupt::kReply0 || interrupt == dsp::Interrupt::kReply1)
    {
        dsp_.ReadReply(static_cast<uint32_t>(interrupt));
    }
}

} // namespace threesf::horizon
