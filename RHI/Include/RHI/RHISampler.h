#pragma once

#include "RHI/RHIResources.h"

namespace RVX
{
    // =============================================================================
    // Sampler Description
    // =============================================================================
    struct RHISamplerDesc
    {
        RHIFilterMode minFilter = RHIFilterMode::Linear;
        RHIFilterMode magFilter = RHIFilterMode::Linear;
        RHIFilterMode mipFilter = RHIFilterMode::Linear;
        RHIAddressMode addressU = RHIAddressMode::Repeat;
        RHIAddressMode addressV = RHIAddressMode::Repeat;
        RHIAddressMode addressW = RHIAddressMode::Repeat;
        float mipLodBias = 0.0f;
        bool anisotropyEnable = false;
        float maxAnisotropy = 1.0f;
        bool compareEnable = false;
        RHICompareOp compareOp = RHICompareOp::Never;
        float minLod = 0.0f;
        float maxLod = 1000.0f;
        float borderColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const char* debugName = nullptr;

        // Convenience constructors
        static RHISamplerDesc PointClamp()
        {
            RHISamplerDesc desc;
            desc.minFilter = RHIFilterMode::Nearest;
            desc.magFilter = RHIFilterMode::Nearest;
            desc.mipFilter = RHIFilterMode::Nearest;
            desc.addressU = RHIAddressMode::ClampToEdge;
            desc.addressV = RHIAddressMode::ClampToEdge;
            desc.addressW = RHIAddressMode::ClampToEdge;
            return desc;
        }

        static RHISamplerDesc LinearClamp()
        {
            RHISamplerDesc desc;
            desc.addressU = RHIAddressMode::ClampToEdge;
            desc.addressV = RHIAddressMode::ClampToEdge;
            desc.addressW = RHIAddressMode::ClampToEdge;
            return desc;
        }

        static RHISamplerDesc LinearWrap()
        {
            return RHISamplerDesc{};
        }

        static RHISamplerDesc Anisotropic(float maxAniso = 16.0f)
        {
            RHISamplerDesc desc;
            desc.anisotropyEnable = true;
            desc.maxAnisotropy = maxAniso;
            return desc;
        }
    };

    // =============================================================================
    // Sampler Interface
    // =============================================================================
    class RHISampler : public RHIResource
    {
    public:
        virtual ~RHISampler() = default;
    };

} // namespace RVX
