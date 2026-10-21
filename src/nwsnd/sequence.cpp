// SPDX-License-Identifier: MIT

#include "nwsnd/sequence.h"

#include <array>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"

#include "nwsnd/engine.h"
#include "nwsnd/voice.h"

namespace threesf::nwsnd
{

Outputs Policy::CombineOutputs(const Outputs&, const Outputs&, float pan, float span, float main_send,
                               const std::array<float, 3>& fx_send)
{
    Outputs outputs;
    outputs.pan = pan;
    outputs.span = span;
    outputs.main_send = main_send;
    outputs.fx_send = {fx_send[0], fx_send[1]};

    return outputs;
}

void Policy::SetFrontBypass(Engine& engine, Channel& channel, bool front_bypass)
{
    if (channel.voice != ntdlib::nwsnd::kNoVoice)
    {
        channel.Voice(engine).SetFrontBypass(front_bypass);
    }
}

} // namespace threesf::nwsnd

template class ntdlib::nwsnd::Channel<threesf::nwsnd::Policy>;
template class ntdlib::nwsnd::ChannelManager<threesf::nwsnd::Policy>;
template class ntdlib::nwsnd::SequenceTrack<threesf::nwsnd::Policy>;
template class ntdlib::nwsnd::SequenceSoundPlayer<threesf::nwsnd::Policy>;
