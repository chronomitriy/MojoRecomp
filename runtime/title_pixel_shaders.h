#pragma once

#include <span>

#include "gpu/shader_replacement.h"

// Crash of the Titans pixel shaders the runtime knows by hash (motion blur,
// depth of field). Crash: Mind Over Mutant must get its own table once its
// shaders are identified; it does not share this one.
std::span<const mojorecomp::gpu::TitlePixelShader> CrashOfTheTitansPixelShaders();
