#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// stereo_eye is the eye a stereo expansion variant rasterizes, as read from the ancillary input.
void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64,
                             uint32_t stereo_eye = 0);

} // namespace Libs::Graphics::ShaderRecompiler::IR
