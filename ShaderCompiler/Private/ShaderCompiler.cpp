#include "ShaderCompiler/ShaderCompiler.h"

#if defined(_WIN32) || defined(__APPLE__)
#include "DXCCompiler.h"
#else
#include "Core/Log.h"
#endif

namespace RVX
{
#if defined(_WIN32) || defined(__APPLE__)
    std::unique_ptr<IShaderCompiler> CreateShaderCompiler()
    {
        return CreateDXCShaderCompiler();
    }
#else
    namespace
    {
        class UnsupportedShaderCompiler final : public IShaderCompiler
        {
        public:
            ShaderCompileResult Compile(const ShaderCompileOptions& options) override
            {
                (void)options;

                ShaderCompileResult result;
                result.errorMessage = "Shader compilation is not available on this platform";
                RVX_CORE_ERROR("{}", result.errorMessage);
                return result;
            }
        };
    } // namespace

    std::unique_ptr<IShaderCompiler> CreateShaderCompiler()
    {
        return std::make_unique<UnsupportedShaderCompiler>();
    }
#endif

} // namespace RVX
