// SPDX-License-Identifier: MIT

#include "nwsnd/engine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "nwsnd/sequence.h"
#include "nwsnd/util.h"

namespace threesf::nwsnd
{
namespace
{

// nw::snd's random number generator advances once a sound frame from the moment the sound system starts. In game mode
// the game boots and loads the sound first, and by the time the sound's sequence first runs, the generator has taken 17
// steps from its seed: 13 before the driver reports the sound playing, and 4 in the frames after. The model's generator
// starts there too, so that sounds with random commands draw the same numbers in both modes.
constexpr int kRandomStepsBeforeStart = 17;

} // namespace

Engine::Engine(nnsnd::SoundSystem& snd, Fcram& fcram, const csar::SoundArchive& archive, EngineOptions options)
    : snd_(snd), fcram_(fcram), archive_(archive), options_(options), voices_(*this), channels_(*this)
{
    global_vars_.fill(-1);
    for (int i = 0; i < kRandomStepsBeforeStart; i++)
    {
        random_.Next();
    }
}

Engine::~Engine() = default;

const csar::Bank* Engine::GetBank(uint32_t bank_item)
{
    if (auto it = banks_.find(bank_item); it != banks_.end())
    {
        return &it->second;
    }

    const uint32_t index = bank_item & 0xffffff;
    if ((bank_item >> 24) != 0x03 || index >= archive_.Banks().size())
    {
        return nullptr;
    }

    const auto file = archive_.FileData(archive_.Banks()[index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    return &banks_.emplace(bank_item, csar::Bank::Parse(file)).first->second;
}

const Engine::LoadedWarc* Engine::GetWarc(uint32_t wave_archive_item)
{
    if (auto it = warcs_.find(wave_archive_item); it != warcs_.end())
    {
        return &it->second;
    }

    const uint32_t index = wave_archive_item & 0xffffff;
    if ((wave_archive_item >> 24) != 0x05 || index >= archive_.WaveArchives().size())
    {
        return nullptr;
    }

    const auto file = archive_.FileData(archive_.WaveArchives()[index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    // The game loads the whole CWAR into its sound heap; place it in FCRAM the same way so the DSP sees the waves at
    // their real relative positions.
    LoadedWarc w;
    w.file = file;
    w.base = fcram_.Store(file.data(), file.size());
    w.archive = csar::WaveArchive::Parse(file);

    return &warcs_.emplace(wave_archive_item, std::move(w)).first->second;
}

std::optional<WaveInfo> Engine::GetWave(uint32_t wave_archive_item, uint32_t index)
{
    const auto key = std::make_pair(wave_archive_item, index);
    if (auto it = waves_.find(key); it != waves_.end())
    {
        return it->second;
    }

    std::optional<WaveInfo> result;
    const LoadedWarc* w = GetWarc(wave_archive_item);
    if (w && index < w->archive.waves.size())
    {
        const csar::Wave wave = csar::Wave::Parse(w->archive.waves[index]);
        WaveInfo info;
        bool ok = true;
        switch (wave.encoding)
        {
        case csar::WaveEncoding::kPcm8:
            info.sample_format = 0;
            break;
        case csar::WaveEncoding::kPcm16:
            info.sample_format = 1;
            break;
        case csar::WaveEncoding::kDspAdpcm:
            info.sample_format = 3;
            break;
        default: // IMA-ADPCM, which the DSP can't play
            ok = false;
            break;
        }

        if (ok && !wave.channels.empty())
        {
            info.loop = wave.loop;
            info.channel_count = static_cast<int>(std::min<std::size_t>(wave.channels.size(), 2));
            info.sample_rate = wave.sample_rate;
            info.loop_start = wave.loop_start;
            info.loop_end = wave.loop_end;
            for (int c = 0; c < info.channel_count; c++)
            {
                const auto& src = wave.channels[c];
                auto& dst = info.channels[c];
                dst.data = w->base + static_cast<PAddr>(src.data.data() - w->file.data());
                if (src.adpcm)
                {
                    dst.coefs = src.adpcm->coefs;
                    dst.context = {src.adpcm->pred_scale, src.adpcm->yn1, src.adpcm->yn2};
                    dst.loop_context = {src.adpcm->loop_pred_scale, src.adpcm->loop_yn1, src.adpcm->loop_yn2};
                }
            }

            result = info;
        }
    }

    waves_.emplace(key, result);

    return result;
}

Channel* Engine::NoteOn(const csar::Bank* bank, const NoteOnInfo& info)
{
    // Bank::NoteOn (0x48f954) with BankFileReader::ReadVelocityRegionInfo (0x48e2f8).
    if (!bank || info.prg_no >= bank->instruments.size() || !bank->instruments[info.prg_no])
    {
        return nullptr;
    }

    const csar::VelocityRegion* vr = bank->instruments[info.prg_no]->Find(info.key, info.velocity);
    if (!vr || vr->wave_id_index >= bank->waves.size())
    {
        return nullptr;
    }

    const csar::WaveId& id = bank->waves[vr->wave_id_index];
    if (id.index == 0xffffffff)
    {
        return nullptr;
    }

    const std::optional<WaveInfo> wave = GetWave(id.wave_archive_item, id.index);
    if (!wave)
    {
        return nullptr;
    }

    Channel* ch = channels_.AllocChannel(std::min(wave->channel_count, 2), info.priority, info.callback);
    if (!ch)
    {
        return nullptr;
    }

    ch->key_ = static_cast<uint8_t>(info.key);
    ch->original_key_ = vr->original_key;
    ch->init_volume_ = static_cast<float>(info.velocity * info.velocity * vr->volume) * kOneOver127Cubed;
    ch->tune_ = vr->pitch;
    ch->env_.SetAttack(vr->adshr.attack);
    ch->env_.SetHold(vr->adshr.hold);
    ch->env_.SetDecay(vr->adshr.decay);
    ch->env_.SetSustain(vr->adshr.sustain);
    ch->env_.SetRelease(vr->adshr.release);
    ch->init_pan_ = static_cast<float>(static_cast<int>(vr->pan) + info.init_pan - 64) * kOneOver63;
    ch->key_group_ = vr->key_group;
    ch->ignore_note_off_ = vr->ignore_note_off;
    ch->interpolation_type_ = vr->interpolation_type;
    ch->Start(*wave, info.length);

    return ch;
}

SequenceSoundPlayer* Engine::StartSequence(uint32_t index)
{
    if (index >= archive_.Sounds().size())
    {
        return nullptr;
    }

    const csar::SoundInfo& info = archive_.Sounds()[index];
    if (info.type != csar::SoundType::kSequence)
    {
        return nullptr;
    }

    const auto file = archive_.FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    std::array<const csar::Bank*, 4> bank_ptrs{};
    for (std::size_t i = 0; i < std::min<std::size_t>(4, info.banks.size()); i++)
    {
        bank_ptrs[i] = GetBank(info.banks[i]);
    }

    auto player = std::make_unique<SequenceSoundPlayer>(*this, info, csar::Sequence::Parse(file), bank_ptrs);
    player->Start();
    players_.push_back(std::move(player));

    return players_.back().get();
}

void Engine::FrameProcess()
{
    // SoundThread::FrameProcess (0x1666d0, which falls through to the frame's work at 0x16674c): player updates,
    // channel updates, the per-frame random advance, then voice updates.
    for (auto& p : players_)
    {
        p->Update();
    }

    channels_.UpdateAllChannel();
    random_.Next();
    voices_.UpdateAllVoices();
    channels_.CollectGarbage();
}

bool Engine::RunFrame()
{
    if (!snd_.WaitForDspSync())
    {
        return false;
    }

    FrameProcess();
    snd_.SendParameterToDsp();
    frame_count_++;

    return true;
}

bool Engine::IsBusy() const
{
    for (const auto& p : players_)
    {
        if (!p->IsFinished())
        {
            return true;
        }
    }

    return channels_.ActiveCount() > 0;
}

bool ArchiveModel::Start(std::span<const uint8_t> firmware, const csar::SoundArchive& archive, uint8_t output_mode)
{
    nnsnd::DspDefaults defaults;
    defaults.output_format = output_mode;
    if (!snd.Initialize(firmware, defaults))
    {
        return false;
    }

    EngineOptions options;
    options.output_mode = output_mode;
    engine.emplace(snd, fcram, archive, options);

    return true;
}

} // namespace threesf::nwsnd
