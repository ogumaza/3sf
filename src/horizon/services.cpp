// SPDX-License-Identifier: MIT

// HLE services (see services.h).

#include "horizon/services.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/bytes.h"
#include "horizon/ipc.h"

namespace threesf::horizon
{
namespace
{

// A service that logs each command the first time it sees it and replies with success (and zeroed outputs).
class StubService : public Cloneable<StubService, Service, Service>
{
public:
    explicit StubService(std::string name) : name_(std::move(name))
    {
    }

    void HandleRequest(Kernel& kernel, uint32_t cmdbuf) override;

private:
    std::string name_;
    std::set<uint32_t> logged_;
};

class ErrfService : public Cloneable<ErrfService, Service, Service>
{
public:
    void HandleRequest(Kernel& kernel, uint32_t cmdbuf) override;
};

class SrvService : public Cloneable<SrvService, Service, Service>
{
public:
    void HandleRequest(Kernel& kernel, uint32_t cmdbuf) override;

private:
    std::shared_ptr<Semaphore> notification_;
    std::map<std::string, std::shared_ptr<Service>> stubs_;
};

class FsService : public Cloneable<FsService, Service, Service>
{
public:
    explicit FsService(std::shared_ptr<RomFs> romfs) : romfs_(std::move(romfs))
    {
    }

    void HandleRequest(Kernel& kernel, uint32_t cmdbuf) override;

private:
    std::shared_ptr<RomFs> romfs_;
    std::set<uint32_t> logged_;
};

// dsp::DSP, on the Teakra DSP.
class DspService : public Cloneable<DspService, Service, Service>
{
public:
    explicit DspService(dsp::TeakraDsp& dsp);

    void HandleRequest(Kernel& kernel, uint32_t cmdbuf) override;
    void Attach(Kernel& kernel);

private:
    dsp::TeakraDsp* dsp_; // a pointer, so that the service can be copied into a snapshot
    Kernel* kernel_ = nullptr;
    std::array<std::shared_ptr<Event>, 3> pipe_events_{};
    std::shared_ptr<Event> interrupt_zero_, interrupt_one_;
    std::shared_ptr<Event> semaphore_event_;
    uint16_t preset_semaphore_ = 0;
    std::set<uint32_t> logged_;
};

class CfgService : public Cloneable<CfgService, Service, Service>
{
public:
    void HandleRequest(Kernel& kernel, uint32_t cmdbuf) override;

private:
    std::set<uint32_t> logged_;
};

class AptService : public Cloneable<AptService, Service, Service>
{
public:
    void HandleRequest(Kernel& kernel, uint32_t cmdbuf) override;

private:
    std::shared_ptr<Mutex> lock_;
    std::shared_ptr<Event> notification_event_, parameter_event_;
    std::set<uint32_t> logged_;
};

void StubService::HandleRequest(Kernel& kernel, uint32_t cmdbuf)
{
    IpcContext ctx(kernel, cmdbuf);
    if (logged_.insert(ctx.Header()).second)
    {
        kernel.Log("[%s] unhandled command %08x (stubbed, returns success)", name_.c_str(), ctx.Header());
    }

    ctx.Reply(1 + 4, 0);

    for (uint32_t i = 2; i < 6; i++)
    {
        ctx.SetWord(i, 0);
    }
}

// err:f takes fatal error reports, which are only logged.
void ErrfService::HandleRequest(Kernel& kernel, uint32_t cmdbuf)
{
    IpcContext ctx(kernel, cmdbuf);
    if (ctx.Command() == 0x0001) // ThrowFatalError(FatalErrorInfo)
    {
        const uint32_t w1 = ctx.Word(1);
        const uint32_t type = w1 & 0xff;
        kernel.Log("[err:f] fatal error type %u, result %08x, pc %08x, lr %08x (thread %u)", type, ctx.Word(2),
                   ctx.Word(3), type == 0 || type == 1 ? ctx.Word(9) : 0,
                   kernel.CurrentThread() ? kernel.CurrentThread()->id_ : 0);

        std::string words;
        for (uint32_t i = 1; i <= 32; i++)
        {
            char b[12];
            std::snprintf(b, sizeof(b), "%08x ", ctx.Word(i));
            words += b;
        }

        kernel.Log("[err:f] info: %s", words.c_str());
    }

    ctx.Reply(1, 0);
}

void SrvService::HandleRequest(Kernel& kernel, uint32_t cmdbuf)
{
    IpcContext ctx(kernel, cmdbuf);
    switch (ctx.Command())
    {
    case 0x0001: // RegisterClient
        ctx.Reply(1, 0);
        return;

    case 0x0002: // EnableNotification
        {
            if (!notification_)
            {
                notification_ = kernel.Make<Semaphore>();
                notification_->max_count_ = 64;
            }

            ctx.Reply(1, 2);
            ctx.SetWord(2, IpcCopyHandles(1));
            ctx.SetWord(3, kernel.AddHandle(notification_));
            return;
        }

    case 0x0005: // GetServiceHandle(name[8], length, flags)
        {
            uint8_t name[8];
            StoreLe32(name, ctx.Word(1));
            StoreLe32(name + 4, ctx.Word(2));
            const uint32_t len = std::min<uint32_t>(ctx.Word(3), 8);
            const std::string service_name(reinterpret_cast<const char*>(name), len);
            auto service = kernel.FindService(service_name);
            if (!service)
            {
                auto& stub = stubs_[service_name];
                if (!stub)
                {
                    kernel.Log("[srv:] GetServiceHandle(\"%s\"): not emulated, using a stub", service_name.c_str());
                    stub = kernel.MakeService<StubService>(service_name);
                }
                service = stub;
            }

            ctx.Reply(1, 2);
            ctx.SetWord(2, IpcMoveHandles(1));
            ctx.SetWord(3, kernel.CreateSessionHandle(service));
            return;
        }

    case 0x000b: // ReceiveNotification
        ctx.Reply(2, 0);
        ctx.SetWord(2, 0);
        return;

    default:
        kernel.Log("[srv:] command %08x (stubbed)", ctx.Header());
        ctx.Reply(1, 0);
        return;
    }
}

// fs:USER serves the application's RomFS and nothing else.
constexpr uint32_t kResultFsNotFound = 0xc8804478;
constexpr uint32_t kResultFsNotSupported = 0xe0c046be;

// Session for one open file (the RomFS image).
class RomFsFile : public Cloneable<RomFsFile, Service, Service>
{
public:
    explicit RomFsFile(std::shared_ptr<RomFs> romfs) : romfs_(std::move(romfs))
    {
    }

    void HandleRequest(Kernel& kernel, uint32_t cmdbuf) override
    {
        IpcContext ctx(kernel, cmdbuf);
        switch (ctx.Command())
        {
        case 0x0802: // Read(64-bit offset, size, [mapped buffer])
            {
                const uint64_t offset = ctx.Word64(1);
                const uint32_t size = ctx.Word(3);
                const uint32_t desc = ctx.Word(4);
                const uint32_t addr = ctx.Word(5);
                std::vector<uint8_t> buf(size);
                const uint64_t n = romfs_->Read(offset, size, buf.data());
                kernel.Memory().WriteBlock(addr, buf.data(), static_cast<std::size_t>(n));
                ctx.Reply(2, 2);
                ctx.SetWord(2, static_cast<uint32_t>(n));
                ctx.SetWord(3, desc);
                ctx.SetWord(4, addr);
                return;
            }

        case 0x0804: // GetSize
            ctx.Reply(3, 0);
            ctx.SetWord64(2, romfs_->Size());
            return;

        case 0x0808: // Close
        case 0x0809: // Flush
        case 0x080a: // SetPriority
            ctx.Reply(1, 0);
            return;

        case 0x080b: // GetPriority
            ctx.Reply(2, 0);
            ctx.SetWord(2, 0);
            return;

        case 0x080c: // OpenLinkFile
            {
                ctx.Reply(1, 2);
                ctx.SetWord(2, IpcMoveHandles(1));
                auto self = kernel.MakeService<RomFsFile>(romfs_);
                ctx.SetWord(3, kernel.CreateSessionHandle(self));
                return;
            }

        default:
            kernel.Log("[fs:USER file] unhandled command %08x", ctx.Header());
            ctx.Reply(1, 0, kResultFsNotSupported);
            return;
        }
    }

private:
    std::shared_ptr<RomFs> romfs_;
};

void FsService::HandleRequest(Kernel& kernel, uint32_t cmdbuf)
{
    IpcContext ctx(kernel, cmdbuf);

    const auto open_romfs = [&](uint32_t archive_id, uint32_t path_type, uint32_t path_size, uint32_t path_addr) -> bool
    {
        if (archive_id != 3)
        {
            return false;
        }
        if (path_type == 2 && path_size >= 4 && kernel.Read32(path_addr) != 0)
        {
            return false; // ExeFS or code: not provided
        }

        return true;
    };

    switch (ctx.Command())
    {
    case 0x0801: // Initialize
    case 0x0861: // InitializeWithSdkVersion
    case 0x0862: // SetPriority
    case 0x080e: // CloseArchive
        ctx.Reply(1, 0);
        return;

    case 0x0863: // GetPriority
        ctx.Reply(2, 0);
        ctx.SetWord(2, 0);
        return;

    case 0x0803: // OpenFileDirectly
        {
            const uint32_t archive_id = ctx.Word(2);
            const uint32_t file_path_type = ctx.Word(5);
            const uint32_t file_path_size = ctx.Word(6);
            const uint32_t file_path_addr = ctx.Word(12);
            if (!open_romfs(archive_id, file_path_type, file_path_size, file_path_addr))
            {
                kernel.Log("[fs:USER] OpenFileDirectly(archive %x, path type %u): not found", archive_id,
                           file_path_type);
                ctx.Reply(1, 0, kResultFsNotFound);
                return;
            }

            ctx.Reply(1, 2);
            ctx.SetWord(2, IpcMoveHandles(1));
            ctx.SetWord(3, kernel.CreateSessionHandle(kernel.MakeService<RomFsFile>(romfs_)));
            return;
        }

    case 0x080c: // OpenArchive(archive id, path type, path size, [path])
        {
            const uint32_t archive_id = ctx.Word(1);
            if (archive_id != 3)
            {
                kernel.Log("[fs:USER] OpenArchive(%x): not supported", archive_id);
                ctx.Reply(1, 0, kResultFsNotFound);
                return;
            }

            ctx.Reply(3, 0);
            ctx.SetWord64(2, 0x3000000003ull);
            return;
        }

    case 0x0802: // OpenFile(transaction, 64-bit archive handle, path type, path size, flags, attributes, [path])
        {
            const uint32_t path_type = ctx.Word(4);
            const uint32_t path_size = ctx.Word(5);
            const uint32_t path_addr = ctx.Word(9);
            if (!open_romfs(3, path_type, path_size, path_addr))
            {
                ctx.Reply(1, 0, kResultFsNotFound);
                return;
            }

            ctx.Reply(1, 2);
            ctx.SetWord(2, IpcMoveHandles(1));
            ctx.SetWord(3, kernel.CreateSessionHandle(kernel.MakeService<RomFsFile>(romfs_)));
            return;
        }

    case 0x0817: // IsSdmcDetected
    case 0x0818: // IsSdmcWritable
        ctx.Reply(2, 0);
        ctx.SetWord(2, 0);
        return;

    default:
        if (logged_.insert(ctx.Header()).second)
        {
            kernel.Log("[fs:USER] unhandled command %08x (stubbed)", ctx.Header());
        }

        ctx.Reply(1, 0);
        return;
    }
}

DspService::DspService(dsp::TeakraDsp& dsp) : dsp_(&dsp)
{
}

void DspService::Attach(Kernel& kernel)
{
    kernel_ = &kernel;
    semaphore_event_ = kernel.Make<Event>();
    semaphore_event_->on_signal_ = [this]
    {
        dsp_->SetSemaphore(preset_semaphore_);
    };

    const auto on_interrupt = [this](dsp::Interrupt type, dsp::Pipe pipe)
    {
        std::shared_ptr<Event> event;
        switch (type)
        {
        case dsp::Interrupt::kReply0:
            event = interrupt_zero_;
            if (!event)
            {
                dsp_->ReadReply(0);
            }
            break;

        case dsp::Interrupt::kReply1:
            event = interrupt_one_;
            if (!event)
            {
                dsp_->ReadReply(1);
            }
            break;

        case dsp::Interrupt::kPipe:
            {
                const auto index = static_cast<std::size_t>(pipe);
                if (index < pipe_events_.size())
                {
                    event = pipe_events_[index];
                }
                break;
            }
        }

        if (event)
        {
            kernel_->SignalEvent(*event);
        }
    };
    dsp_->SetInterruptHandler(on_interrupt);
}

void DspService::HandleRequest(Kernel& kernel, uint32_t cmdbuf)
{
    IpcContext ctx(kernel, cmdbuf);
    const uint64_t dsp_before = dsp_->Cycles();

    switch (ctx.Command())
    {
    case 0x0001: // RecvData(register)
        {
            const uint16_t value = dsp_->ReadReply(ctx.Word(1));
            ctx.Reply(2, 0);
            ctx.SetWord(2, value);
            break;
        }

    case 0x0002: // RecvDataIsReady(register)
        {
            const bool ready = dsp_->ReplyReady(ctx.Word(1));
            ctx.Reply(2, 0);
            ctx.SetWord(2, ready ? 1 : 0);
            break;
        }

    case 0x0007: // SetSemaphore(value)
        dsp_->SetSemaphore(static_cast<uint16_t>(ctx.Word(1)));
        ctx.Reply(1, 0);
        break;

    case 0x000c: // ConvertProcessAddressFromDspDram(address)
        {
            const uint32_t address = ctx.Word(1); // read before the reply overwrites word 1
            ctx.Reply(2, 0);
            ctx.SetWord(2, (address << 1) + kDspRamBase + 0x40000);
            break;
        }

    case 0x000d: // WriteProcessPipe(channel, size, [static buffer])
        {
            const uint32_t channel = ctx.Word(1);
            const uint32_t size = ctx.Word(2);
            const uint32_t desc = ctx.Word(3);
            const uint32_t addr = ctx.Word(4);
            auto buffer = ctx.ReadBuffer(addr, std::min(size, desc >> 14));
            const auto pipe = static_cast<dsp::Pipe>(channel);

            // Games pass stack garbage in these bytes; the DSP module overwrites them (Azahar RE).
            if (channel == 2 && buffer.size() >= 4)
            {
                buffer[2] = 0;
                buffer[3] = 0;
            }
            else if (channel == 3 && buffer.size() >= 8)
            {
                buffer[4] = 1;
                buffer[5] = buffer[6] = buffer[7] = 0;
            }

            dsp_->WritePipe(pipe, buffer);
            ctx.Reply(1, 0);
            break;
        }

    case 0x000e: // ReadPipe(channel, peer, size)
    case 0x0010: // ReadPipeIfPossible(channel, peer, size)
        {
            const auto pipe = static_cast<dsp::Pipe>(ctx.Word(1));
            const uint32_t size = ctx.Word(3) & 0xffff;
            const std::size_t readable = dsp_->PipeReadable(pipe);
            std::vector<uint8_t> data;
            if (readable >= size)
            {
                data = dsp_->ReadPipe(pipe, size);
            }

            const uint32_t addr = ctx.WriteStaticBuffer(0, data.data(), static_cast<uint32_t>(data.size()));
            if (ctx.Command() == 0x000e)
            {
                ctx.Reply(1, 2);
                ctx.SetWord(2, IpcStaticBuffer(static_cast<uint32_t>(data.size()), 0));
                ctx.SetWord(3, addr);
            }
            else
            {
                ctx.Reply(2, 2);
                ctx.SetWord(2, static_cast<uint32_t>(data.size()));
                ctx.SetWord(3, IpcStaticBuffer(static_cast<uint32_t>(data.size()), 0));
                ctx.SetWord(4, addr);
            }
            break;
        }

    case 0x000f: // GetPipeReadableSize(channel, peer)
        {
            const auto pipe = static_cast<dsp::Pipe>(ctx.Word(1));
            ctx.Reply(2, 0);
            ctx.SetWord(2, static_cast<uint32_t>(dsp_->PipeReadable(pipe)) & 0xffff);
            break;
        }

    case 0x0011: // LoadComponent(size, prog mask, data mask, [mapped buffer])
        {
            const uint32_t size = ctx.Word(1);
            const uint32_t desc = ctx.Word(4);
            const uint32_t addr = ctx.Word(5);
            const auto component = ctx.ReadBuffer(addr, size);
            dsp_->LoadComponent(component);
            ctx.Reply(2, 2);
            ctx.SetWord(2, 1);
            ctx.SetWord(3, desc);
            ctx.SetWord(4, addr);
            kernel.Log("[dsp::DSP] LoadComponent: %u bytes", size);
            break;
        }

    case 0x0012: // UnloadComponent
        dsp_->UnloadComponent();
        ctx.Reply(1, 0);
        break;

    case 0x0013: // FlushDataCache
    case 0x0014: // InvalidateDataCache
    case 0x0020: // ForceHeadphoneOut
        ctx.Reply(1, 0);
        break;

    case 0x0015: // RegisterInterruptEvents(interrupt, channel, [copy handle])
        {
            const uint32_t interrupt = ctx.Word(1);
            const uint32_t channel = ctx.Word(2);
            auto event = kernel.Get<Event>(ctx.Word(4));
            switch (interrupt)
            {
            case 0:
                interrupt_zero_ = event;
                break;

            case 1:
                interrupt_one_ = event;
                break;

            case 2:
                if (channel < pipe_events_.size())
                {
                    pipe_events_[channel] = event;
                }
                break;

            default:
                break;
            }

            ctx.Reply(1, 0);
            break;
        }

    case 0x0016: // GetSemaphoreEventHandle
        ctx.Reply(1, 2);
        ctx.SetWord(2, IpcCopyHandles(1));
        ctx.SetWord(3, kernel.AddHandle(semaphore_event_));
        break;

    case 0x0017: // SetSemaphoreMask(mask)
        preset_semaphore_ = static_cast<uint16_t>(ctx.Word(1));
        ctx.Reply(1, 0);
        break;

    case 0x001f: // GetHeadphoneStatus
    case 0x0021: // GetIsDspOccupied
        ctx.Reply(2, 0);
        ctx.SetWord(2, 0);
        break;

    default:
        if (logged_.insert(ctx.Header()).second)
        {
            kernel.Log("[dsp::DSP] unhandled command %08x (stubbed)", ctx.Header());
        }

        ctx.Reply(1, 0);
        break;
    }

    // The caller waited while the DSP ran inside a blocking call, so that time is added to the kernel's ticks.
    const uint64_t dsp_after = dsp_->Cycles();
    if (dsp_after > dsp_before)
    {
        kernel.AddTicks((dsp_after - dsp_before) * 2);
    }
}

void CfgService::HandleRequest(Kernel& kernel, uint32_t cmdbuf)
{
    IpcContext ctx(kernel, cmdbuf);
    switch (ctx.Command())
    {
    case 0x0001: // GetConfigInfoBlk2(size, block id, [mapped buffer])
        {
            const uint32_t size = ctx.Word(1);
            const uint32_t block = ctx.Word(2);
            const uint32_t desc = ctx.Word(3);
            const uint32_t addr = ctx.Word(4);
            std::vector<uint8_t> data(size, 0);
            // A block of size 0 gets nothing. Its empty vector may have no storage, and memcpy mustn't be given a null
            // pointer.
            const auto put = [&](const uint8_t* src, std::size_t n)
            {
                const std::size_t count = std::min<std::size_t>(n, size);
                if (count)
                {
                    std::memcpy(data.data(), src, count);
                }
            };

            switch (block)
            {
            case 0x00070001: // sound output mode: stereo
                {
                    constexpr uint8_t kStereo = 1;
                    put(&kStereo, 1);
                    break;
                }

            case 0x000a0002: // language: Japanese
                {
                    constexpr uint8_t kJapanese = 0;
                    put(&kJapanese, 1);
                    break;
                }

            case 0x000b0000: // country info: Japan
                {
                    constexpr uint8_t kCountryInfo[4] = {0, 0, 0, 1};
                    put(kCountryInfo, 4);
                    break;
                }

            case 0x00050005: // stereo camera settings
                {
                    constexpr float kStereoCamera[8] = {62.0f, 289.0f, 76.80000305175781f, 46.08000183105469f,
                                                        10.0f, 5.0f,   55.58000183105469f, 21.56999969482422f};
                    uint8_t bytes[sizeof(kStereoCamera)];
                    for (std::size_t i = 0; i < std::size(kStereoCamera); i++)
                    {
                        StoreLe32(bytes + 4 * i, std::bit_cast<uint32_t>(kStereoCamera[i]));
                    }
                    put(bytes, sizeof(bytes));
                    break;
                }

            case 0x000d0000: // EULA version
                {
                    constexpr uint8_t kEulaVersion[4] = {0x7f, 0x7f, 0, 0};
                    put(kEulaVersion, 4);
                    break;
                }

            case 0x000a0000: // user name
                {
                    constexpr char16_t kUserName[] = u"3SF";
                    uint8_t bytes[sizeof(kUserName)];
                    for (std::size_t i = 0; i < std::size(kUserName); i++)
                    {
                        StoreLe16(bytes + 2 * i, kUserName[i]);
                    }
                    put(bytes, sizeof(bytes));
                    break;
                }

            case 0x000a0001: // birthday
                {
                    constexpr uint8_t kBirthday[2] = {1, 1};
                    put(kBirthday, 2);
                    break;
                }

            default:
                if (logged_.insert(block).second)
                {
                    kernel.Log("[cfg:u] config block %08x (size %u): zeroes", block, size);
                }
                break;
            }

            kernel.Memory().WriteBlock(addr, data.data(), data.size());
            ctx.Reply(1, 2);
            ctx.SetWord(2, desc);
            ctx.SetWord(3, addr);
            return;
        }

    case 0x0002: // SecureInfoGetRegion: Japan
        ctx.Reply(2, 0);
        ctx.SetWord(2, 0);
        return;

    case 0x0003: // GenHashConsoleUnique
        ctx.Reply(3, 0);
        ctx.SetWord(2, 0x12345678);
        ctx.SetWord(3, 0x9abcdef0);
        return;

    case 0x0004: // GetRegionCanadaUSA
    case 0x0005: // GetSystemModel: original 3DS
        ctx.Reply(2, 0);
        ctx.SetWord(2, 0);
        return;

    case 0x0006: // GetModelNintendo2DS: not a 2DS
        ctx.Reply(2, 0);
        ctx.SetWord(2, 1);
        return;

    default:
        if (logged_.insert(ctx.Header()).second)
        {
            kernel.Log("[cfg:u] unhandled command %08x (stubbed)", ctx.Header());
        }

        ctx.Reply(1, 0);
        return;
    }
}

// APT:U does enough for the SDK's startup code to believe it's a running application.
void AptService::HandleRequest(Kernel& kernel, uint32_t cmdbuf)
{
    IpcContext ctx(kernel, cmdbuf);
    switch (ctx.Command())
    {
    case 0x0001: // GetLockHandle(flags)
        {
            if (!lock_)
            {
                lock_ = kernel.Make<Mutex>();
            }

            ctx.Reply(3, 2);
            ctx.SetWord(2, 0); // applet attributes
            ctx.SetWord(3, 0); // power button state
            ctx.SetWord(4, IpcCopyHandles(1));
            ctx.SetWord(5, kernel.AddHandle(lock_));
            return;
        }

    case 0x0002: // Initialize(app id, attributes)
        {
            if (!notification_event_)
            {
                notification_event_ = kernel.Make<Event>();
            }

            if (!parameter_event_)
            {
                parameter_event_ = kernel.Make<Event>();
            }

            ctx.Reply(1, 3);
            ctx.SetWord(2, IpcCopyHandles(2));
            ctx.SetWord(3, kernel.AddHandle(notification_event_));
            ctx.SetWord(4, kernel.AddHandle(parameter_event_));
            return;
        }

    case 0x0003: // Enable
        // The application is started: the first parameter is a wakeup.
        if (parameter_event_)
        {
            kernel.SignalEvent(*parameter_event_);
        }

        ctx.Reply(1, 0);
        return;

    case 0x000b: // InquireNotification
        ctx.Reply(2, 0);
        ctx.SetWord(2, 0);
        return;

    case 0x000d: // ReceiveParameter(app id, buffer size)
    case 0x000e: // GlanceParameter
        {
            ctx.Reply(4, 4);
            ctx.SetWord(2, 0); // sender app id
            ctx.SetWord(3, 1); // command: wakeup
            ctx.SetWord(4, 0); // actual size
            ctx.SetWord(5, IpcMoveHandles(1));
            ctx.SetWord(6, 0);
            ctx.SetWord(7, IpcStaticBuffer(0, 0));
            ctx.SetWord(8, 0);
            return;
        }

    default:
        if (logged_.insert(ctx.Header()).second)
        {
            kernel.Log("[APT:U] unhandled command %08x (stubbed)", ctx.Header());
        }

        ctx.Reply(1 + 2, 0);
        ctx.SetWord(2, 0);
        ctx.SetWord(3, 0);
        return;
    }
}

} // namespace

void InstallServices(Kernel& kernel, std::shared_ptr<RomFs> romfs, dsp::TeakraDsp& dsp)
{
    auto dsp_service = kernel.MakeService<DspService>(dsp);
    dsp_service->Attach(kernel);
    kernel.RegisterPort("srv:", kernel.MakeService<SrvService>());
    kernel.RegisterPort("err:f", kernel.MakeService<ErrfService>());
    kernel.RegisterService("fs:USER", kernel.MakeService<FsService>(std::move(romfs)));
    kernel.RegisterService("dsp::DSP", dsp_service);
    kernel.RegisterService("cfg:u", kernel.MakeService<CfgService>());
    kernel.RegisterService("APT:U", kernel.MakeService<AptService>());
}

} // namespace threesf::horizon
