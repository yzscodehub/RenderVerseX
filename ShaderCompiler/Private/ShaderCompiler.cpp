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
            ShaderCompileSupport QuerySupport(
                const ShaderCompileOptions& options) const override
            {
                (void)options;
                return {
                    ShaderCompileSupportCode::RuntimeCompilerUnavailable,
                    "Shader compilation is not available on this platform"};
            }

            ShaderCompileResult Compile(const ShaderCompileOptions& options) override
            {
                ShaderCompileResult result;
                result.errorMessage = QuerySupport(options).reason;
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
