#pragma once

#include "RHI/RHIResources.h"
#include <vector>

namespace RVX
{
    // =============================================================================
    // Shader Description
    // =============================================================================
    struct RHIShaderDesc
    {
        RHIShaderStage stage = RHIShaderStage::None;
        const void* bytecode = nullptr;
        uint64 bytecodeSize = 0;
        const char* entryPoint = "main";
        const char* debugName = nullptr;
    };

    // =============================================================================
    // Shader Interface
    // =============================================================================
    class RHIShader : public RHIResource
    {
    public:
        virtual ~RHIShader() = default;

        virtual RHIShaderStage GetStage() const = 0;
        virtual const std::vector<uint8>& GetBytecode() const = 0;
    };

} // namespace RVX
