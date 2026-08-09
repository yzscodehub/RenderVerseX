#pragma once

#include "Core/RefCounted.h"
#include "RHI/RHIDefinitions.h"

#include <string>

namespace RVX
{
    // =============================================================================
    // Forward Declarations
    // =============================================================================
    class RHIBuffer;
    class RHITexture;
    class RHITextureView;
    class RHISampler;
    class RHIShader;
    class RHIPipeline;
    class RHIDescriptorSetLayout;
    class RHIDescriptorSet;
    class RHIPipelineLayout;
    class RHISwapChain;
    class RHIFence;
    class RHICommandContext;
    class RHIHeap;
    class RHIAccelerationStructure;
    class RHIShaderTable;

    // =============================================================================
    // Type Aliases
    // =============================================================================
    using RHIBufferRef              = Ref<RHIBuffer>;
    using RHITextureRef             = Ref<RHITexture>;
    using RHITextureViewRef         = Ref<RHITextureView>;
    using RHISamplerRef             = Ref<RHISampler>;
    using RHIShaderRef              = Ref<RHIShader>;
    using RHIPipelineRef            = Ref<RHIPipeline>;
    using RHIDescriptorSetLayoutRef = Ref<RHIDescriptorSetLayout>;
    using RHIDescriptorSetRef       = Ref<RHIDescriptorSet>;
    using RHIPipelineLayoutRef      = Ref<RHIPipelineLayout>;
    using RHISwapChainRef           = Ref<RHISwapChain>;
    using RHIFenceRef               = Ref<RHIFence>;
    using RHICommandContextRef      = Ref<RHICommandContext>;
    using RHIHeapRef                = Ref<RHIHeap>;
    using RHIAccelerationStructureRef = Ref<RHIAccelerationStructure>;
    using RHIShaderTableRef         = Ref<RHIShaderTable>;

    // =============================================================================
    // Resource Instance Identity
    // =============================================================================
    /**
     * @brief Process-unique identity for one concrete RHI resource instance.
     *
     * Native handles and C++ addresses may be recycled after destruction.  This
     * identity is assigned once by RHIResource and is therefore safe to use in
     * caches whose lifetime can overlap native allocator reuse.
     */
    struct RHIResourceInstanceId
    {
        uint64 value = 0;

        constexpr bool IsValid() const { return value != 0; }
        constexpr explicit operator bool() const { return IsValid(); }

        friend constexpr bool operator==(RHIResourceInstanceId,
                                         RHIResourceInstanceId) = default;
    };

    // =============================================================================
    // RHI Resource Base Class
    // =============================================================================
    class RHIResource : public RefCounted
    {
    public:
        RHIResource();
        virtual ~RHIResource() = default;

        void SetDebugName(const char* name) { m_debugName = name ? name : ""; }
        const std::string& GetDebugName() const { return m_debugName; }
        RHIResourceInstanceId GetResourceInstanceId() const
        {
            return m_resourceInstanceId;
        }

    protected:
        std::string m_debugName;

    private:
        RHIResourceInstanceId m_resourceInstanceId;
    };

    // =============================================================================
    // Subresource Range
    // =============================================================================
    struct RHISubresourceRange
    {
        uint32 baseMipLevel   = 0;
        uint32 mipLevelCount  = RVX_ALL_MIPS;
        uint32 baseArrayLayer = 0;
        uint32 arrayLayerCount = RVX_ALL_LAYERS;
        RHITextureAspect aspect = RHITextureAspect::Color;

        static RHISubresourceRange All()
        {
            return RHISubresourceRange{};
        }

        static RHISubresourceRange Mip(uint32 mipLevel)
        {
            return RHISubresourceRange{mipLevel, 1, 0, RVX_ALL_LAYERS, RHITextureAspect::Color};
        }

        static RHISubresourceRange Layer(uint32 arrayLayer)
        {
            return RHISubresourceRange{0, RVX_ALL_MIPS, arrayLayer, 1, RHITextureAspect::Color};
        }
    };

    // =============================================================================
    // Viewport & Scissor
    // =============================================================================
    struct RHIViewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    struct RHIRect
    {
        int32 x = 0;
        int32 y = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    // =============================================================================
    // Clear Values
    // =============================================================================
    struct RHIClearColor
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
    };

    struct RHIClearDepthStencil
    {
        float depth = 1.0f;
        uint8 stencil = 0;
    };

} // namespace RVX
