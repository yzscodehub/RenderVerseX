#include "ShaderCompiler/ShaderCompiler.h"
#include "DXCCompiler.h"

namespace RVX
{
    std::unique_ptr<IShaderCompiler> CreateShaderCompiler()
    {
        return CreateDXCShaderCompiler();
    }

} // namespace RVX
