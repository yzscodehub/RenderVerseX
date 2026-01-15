#pragma once

#include "ShaderCompiler/ShaderCompiler.h"
#include <memory>

namespace RVX
{
    std::unique_ptr<IShaderCompiler> CreateDXCShaderCompiler();
} // namespace RVX
