#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// When stereo is false the ancillary eye bit folds to 0 (mono / left eye). When true the shader is
// a stereo-dynamic variant: the eye bit is left live so it reads PushData::StereoEyeDword at draw
// time, letting one variant rasterize both eyes.
void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64,
                             bool stereo = false);

} // namespace Libs::Graphics::ShaderRecompiler::IR
