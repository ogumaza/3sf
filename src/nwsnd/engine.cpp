// SPDX-License-Identifier: MIT

#include "nwsnd/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/sar_options.h"
#include "nwsnd/sequence.h"

namespace threesf::nwsnd
{

Engine::Engine(nnsnd::SoundSystem& snd, Fcram& fcram, const ntdlib::sar::SoundArchive& archive, EngineOptions options)
    : snd_(snd), fcram_(fcram), archive_(archive), options_(options), voices_(*this)
{
    global_vars_.fill(-1);

    // nw::snd's random number generator advances once a sound frame from the moment the sound system starts. In game
    // mode the game boots and loads the sound first, and by the time the sound's sequence first runs, the generator has
    // taken 17 steps from its seed: 13 before the driver reports the sound playing, and 4 in the frames after. The
    // model's generator starts there too (the policy's kRandomStepsBeforeStart). Sounds with random commands then draw
    // the same numbers in both modes.
    for (int i = 0; i < Policy::kRandomStepsBeforeStart; i++)
    {
        random_.Next();
    }
}

const ntdlib::sar::Bank* Engine::GetBank(uint32_t bank_item)
{
    if (auto it = banks_.find(bank_item); it != banks_.end())
    {
        return it->second.get();
    }

    const std::optional<std::size_t> index =
        ntdlib::sar::ItemIndex(bank_item, ntdlib::sar::kBankItem, archive_.Banks().size());
    if (!index)
    {
        return nullptr;
    }

    const auto file = archive_.FileData(archive_.Banks()[*index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    return banks_
        .emplace(bank_item, std::make_shared<const ntdlib::sar::Bank>(ntdlib::sar::Bank::Parse(file, kSarOptions)))
        .first->second.get();
}

const Engine::LoadedWarc* Engine::GetWarc(uint32_t wave_archive_item)
{
    if (auto it = warcs_.find(wave_archive_item); it != warcs_.end())
    {
        return it->second.get();
    }

    const std::optional<std::size_t> index =
        ntdlib::sar::ItemIndex(wave_archive_item, ntdlib::sar::kWaveArchiveItem, archive_.WaveArchives().size());
    if (!index)
    {
        return nullptr;
    }

    const auto file = archive_.FileData(archive_.WaveArchives()[*index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    // The game loads the whole CWAR into its sound heap; place it in FCRAM the same way so the DSP sees the waves at
    // their real relative positions.
    LoadedWarc w;
    w.file = file;
    w.base = fcram_.Store(file.data(), file.size());
    w.archive = ntdlib::sar::WaveArchive::Parse(file, kSarOptions);

    return warcs_.emplace(wave_archive_item, std::make_shared<const LoadedWarc>(std::move(w))).first->second.get();
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
        const ntdlib::sar::Wave wave = ntdlib::sar::Wave::Parse(w->archive.waves[index], kSarOptions);
        WaveInfo info;
        bool ok = true;
        switch (wave.encoding)
        {
        case ntdlib::sar::WaveEncoding::kPcm8:
            info.format = 0;
            break;
        case ntdlib::sar::WaveEncoding::kPcm16:
            info.format = 1;
            break;
        case ntdlib::sar::WaveEncoding::kDspAdpcm:
            info.format = 3;
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
                dst.data = w->base + static_cast<ntdlib::PAddr>(src.data.data() - w->file.data());
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

Channel* Engine::NoteOn(const std::vector<uint32_t>& bank_items, int bank_index, const NoteOnInfo& info)
{
    // Bank::NoteOn (0x48f954) with BankFileReader::ReadVelocityRegionInfo (0x48e2f8). StartSequence has read the
    // player's banks.
    const ntdlib::sar::Bank* bank =
        bank_index >= 0 && bank_index < 4 && static_cast<std::size_t>(bank_index) < bank_items.size()
            ? GetBank(bank_items[static_cast<std::size_t>(bank_index)])
            : nullptr;
    if (!bank || info.prg_no >= bank->instruments.size() || !bank->instruments[info.prg_no])
    {
        return nullptr;
    }

    const ntdlib::sar::VelocityRegion* vr = bank->instruments[info.prg_no]->Find(info.key, info.velocity);
    if (!vr || vr->wave_id_index >= bank->waves.size())
    {
        return nullptr;
    }

    const ntdlib::sar::WaveId& id = bank->waves[vr->wave_id_index];
    if (id.index == 0xffffffff)
    {
        return nullptr;
    }

    const std::optional<WaveInfo> wave = GetWave(id.wave_archive_item, id.index);
    if (!wave)
    {
        return nullptr;
    }

    Channel* ch = channels_.AllocChannel(*this, std::min(wave->channel_count, 2), static_cast<uint32_t>(info.priority),
                                         info.owner_player, info.owner_track);
    if (!ch)
    {
        return nullptr;
    }

    // The 3DS's channel holds the note's whole volume, from its velocity and its region, in velocity_volume (see
    // CtrPolicy::ChannelVolume).
    ch->key = static_cast<uint8_t>(info.key);
    ch->original_key = vr->original_key;
    ch->velocity_volume = static_cast<float>(info.velocity * info.velocity * vr->volume) * Policy::kOneOver127Cubed;
    ch->tune = vr->pitch;
    ch->envelope.SetAttack(vr->adshr.attack);
    ch->envelope.SetHold(vr->adshr.hold);
    ch->envelope.SetDecay(vr->adshr.decay);
    ch->envelope.SetSustain(vr->adshr.sustain);
    ch->envelope.SetRelease(vr->adshr.release);
    ch->init_pan = static_cast<float>(static_cast<int>(vr->pan) + info.init_pan - 64) * Policy::kOneOver63;
    ch->key_group = vr->key_group;
    ch->ignore_note_off = vr->ignore_note_off;
    ch->interpolation_type = vr->interpolation;
    ch->Start(*this, *wave, info.length);

    return ch;
}

void Engine::ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus)
{
    if (player >= 0 && static_cast<std::size_t>(player) < players_.size())
    {
        players_[static_cast<std::size_t>(player)].ChannelEvent(*this, track, channel);
    }
}

SequenceSoundPlayer* Engine::StartSequence(uint32_t index)
{
    if (index >= archive_.Sounds().size())
    {
        return nullptr;
    }

    const ntdlib::sar::SoundInfo& info = archive_.Sounds()[index];
    if (info.type != ntdlib::sar::SoundType::kSequence)
    {
        return nullptr;
    }

    const auto file = archive_.FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    // The banks load when the sequence starts. The sound's data loads then too.
    for (std::size_t i = 0; i < std::min<std::size_t>(4, info.banks.size()); i++)
    {
        GetBank(info.banks[i]);
    }

    const ntdlib::sar::Sequence sequence = ntdlib::sar::Sequence::Parse(file, kSarOptions);
    SequenceSoundPlayer& player = players_.emplace_back(static_cast<int>(players_.size()), info, sequence.data);
    player.Start();

    return &player;
}

void Engine::FrameProcess()
{
    // SoundThread::FrameProcess (0x1666d0, which falls through to the frame's work at 0x16674c): player updates,
    // channel updates, the per-frame random advance, then voice updates.
    for (SequenceSoundPlayer& player : players_)
    {
        player.Update(*this);
    }

    channels_.UpdateAllChannel(*this);
    random_.Next();
    voices_.UpdateAllVoices();
}

bool Engine::RunFrame()
{
    if (!snd_.WaitForDspSync())
    {
        return false;
    }

    FrameProcess();
    snd_.SendParameterToDsp();

    return true;
}

bool Engine::IsBusy() const
{
    for (const SequenceSoundPlayer& player : players_)
    {
        if (!player.IsFinished())
        {
            return true;
        }
    }

    return channels_.ActiveCount() > 0;
}

Engine::Snapshot Engine::Save() const
{
    Snapshot snapshot;
    snapshot.voices = voices_.Save();
    snapshot.channels = channels_;
    snapshot.random = random_;
    snapshot.players = players_;
    snapshot.banks = banks_;
    snapshot.warcs = warcs_;
    snapshot.waves = waves_;
    snapshot.global_vars = global_vars_;

    return snapshot;
}

void Engine::Restore(const Snapshot& snapshot)
{
    if (snapshot.players.size() > players_.size())
    {
        throw std::invalid_argument("nw::snd snapshot with players the engine doesn't have");
    }

    // The players that started after the snapshot go. The others keep their places. Callers hold pointers to them.
    while (players_.size() > snapshot.players.size())
    {
        players_.pop_back();
    }
    for (std::size_t i = 0; i < players_.size(); i++)
    {
        players_[i] = snapshot.players[i];
    }

    voices_.Restore(snapshot.voices);
    channels_ = snapshot.channels;
    random_ = snapshot.random;
    banks_ = snapshot.banks;
    warcs_ = snapshot.warcs;
    waves_ = snapshot.waves;
    global_vars_ = snapshot.global_vars;
}

ArchiveModel::Snapshot ArchiveModel::Save(const Snapshot* previous)
{
    return {fcram.Save(previous ? &previous->fcram : nullptr), dsp.Save(previous ? &previous->dsp : nullptr), snd,
            engine ? std::optional<Engine::Snapshot>(engine->Save()) : std::nullopt};
}

void ArchiveModel::Restore(const Snapshot& snapshot)
{
    if (engine.has_value() != snapshot.engine.has_value())
    {
        throw std::invalid_argument("archive model snapshot from before or after the engine started");
    }

    fcram.Restore(snapshot.fcram);
    dsp.Restore(snapshot.dsp);
    snd = snapshot.snd;
    if (engine)
    {
        engine->Restore(*snapshot.engine);
    }
}

bool ArchiveModel::Start(std::span<const uint8_t> firmware, const ntdlib::sar::SoundArchive& archive,
                         uint8_t output_mode)
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
