// SPDX-License-Identifier: MIT

#include "nwsnd/sequence.h"

#include <array>
#include <cstddef>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"

#include "nwsnd/engine.h"
#include "nwsnd/voice.h"

namespace threesf::nwsnd
{

Outputs Policy::CombineOutputs(const Outputs& player, const Outputs&, float pan, float span, float main_send,
                               const std::array<float, 3>& fx_send)
{
    Outputs outputs;
    outputs.pan = pan;
    outputs.span = span;
    outputs.main_send = main_send;
    outputs.fx_send = {player.fx_send[0] + fx_send[0], player.fx_send[1] + fx_send[1]};

    return outputs;
}

void Policy::SetFrontBypass(Engine& engine, Channel& channel, bool front_bypass)
{
    if (channel.voice != ntdlib::nwsnd::kNoVoice)
    {
        channel.Voice(engine).SetFrontBypass(front_bypass);
    }
}

void ApplySequenceStart(Engine& engine, SequenceSoundPlayer& player, const ntdlib::ctr::snd::SequenceStart& start)
{
    for (const ntdlib::ctr::snd::SequenceVariable& variable : start.variables)
    {
        if (variable.index == ntdlib::ctr::snd::SequenceVariable::kNone)
        {
            continue;
        }

        if (variable.frame == 0)
        {
            player.SetVariable(engine, variable.index, variable.value);
        }
        else
        {
            engine.ScheduleVariable(player, variable.index, variable.value, variable.frame);
        }
    }

    if (start.seed)
    {
        engine.Rng() = ntdlib::nwsnd::Random(*start.seed);
    }

    // SeqTrack::SetMute's modes: 1 is SEQ_MUTE_NO_STOP.
    constexpr int kMuteNoStop = 1;
    if (start.muted != 0)
    {
        player.SetTrackMute(engine, start.muted, kMuteNoStop);
    }
    if (start.silenced != 0)
    {
        player.SetTrackSilence(engine, start.silenced, true, 0);
    }

    // The volumes that the game gives the tracks through the sound's handle (SequenceSoundHandle::SetTrackVolume). The
    // driver multiplies each into the volume of the track's channels (UpdateChannelParam, 0x31de18).
    for (std::size_t track = 0; track < start.track_volumes.size(); ++track)
    {
        if (start.track_volumes[track] != 1.0f)
        {
            player.SetTrackVolume(1u << track, start.track_volumes[track]);
        }
    }

    // The tempo ratio and the pitch that the game sets through the sound's handle (SequenceSoundHandle::SetTempoRatio
    // and SoundHandle::SetPitch). Both are 1 unless the game sets them.
    player.SetTempoRatio(start.tempo_ratio);
    player.SetPitch(start.pitch);

    // The sends that the sound's handle and its sound player give it on the aux buses (BasicSound's update, 0x52B7F8
    // to 0x52B818 in Animal Crossing: New Leaf's code.bin). Both are 0 unless the game sets them.
    player.PlayerOutputs().fx_send = start.fx_send;
}

} // namespace threesf::nwsnd

template class ntdlib::nwsnd::Channel<threesf::nwsnd::Policy>;
template class ntdlib::nwsnd::ChannelManager<threesf::nwsnd::Policy>;
template class ntdlib::nwsnd::SequenceTrack<threesf::nwsnd::Policy>;
template class ntdlib::nwsnd::SequenceSoundPlayer<threesf::nwsnd::Policy>;
