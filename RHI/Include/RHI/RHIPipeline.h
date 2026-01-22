#pragma once

#include "RHI/RHIResources.h"
#include <vector>

namespace RVX
{
    // =============================================================================
    // Input Element Description
    // =============================================================================
    struct RHIInputElement
    {
        const char* semanticName = "POSITION";
        uint32 semanticIndex = 0;
        RHIFormat format = RHIFormat::RGB32_FLOAT;
        uint32 inputSlot = 0;
        uint32 alignedByteOffset = 0;
        bool perInstance = false;
        uint32 instanceDataStepRate = 0;
    };

    // =============================================================================
    // Input Layout Description
    // =============================================================================
    struct RHIInputLayoutDesc
    {
        std::vector<RHIInputElement> elements;

        RHIInputLayoutDesc& AddElement(const char* semantic, RHIFormat format, uint32 slot = 0)
        {
            RHIInputElement elem;
            elem.semanticName = semantic;
            elem.format = format;
            elem.inputSlot = slot;
            elem.alignedByteOffset = 0xFFFFFFFF;  // Append
            elements.push_back(elem);
            return *this;
        }
    };

    // =============================================================================
    // Rasterizer State
    // =============================================================================
    struct RHIRasterizerState
    {
        RHIFillMode fillMode = RHIFillMode::Solid;
        RHICullMode cullMode = RHICullMode::Back;
        RHIFrontFace frontFace = RHIFrontFace::CounterClockwise;
        float depthBias = 0.0f;
        float depthBiasClamp = 0.0f;
        float slopeScaledDepthBias = 0.0f;
        bool depthClipEnable = true;
        bool multisampleEnable = false;
        bool antialiasedLineEnable = false;
        bool conservativeRasterEnable = false;

        static RHIRasterizerState Default() { return RHIRasterizerState{}; }
        static RHIRasterizerState NoCull() { RHIRasterizerState s; s.cullMode = RHICullMode::None; return s; }
        static RHIRasterizerState Wireframe() { RHIRasterizerState s; s.fillMode = RHIFillMode::Wireframe; return s; }
    };

    // =============================================================================
    // Depth Stencil State
    // =============================================================================
    struct RHIStencilOpState
    {
        RHIStencilOp failOp = RHIStencilOp::Keep;
        RHIStencilOp depthFailOp = RHIStencilOp::Keep;
        RHIStencilOp passOp = RHIStencilOp::Keep;
        RHICompareOp compareOp = RHICompareOp::Always;
    };

    struct RHIDepthStencilState
    {
        bool depthTestEnable = true;
        bool depthWriteEnable = true;
        RHICompareOp depthCompareOp = RHICompareOp::Less;
        bool stencilTestEnable = false;
        uint8 stencilReadMask = 0xFF;
        uint8 stencilWriteMask = 0xFF;
        RHIStencilOpState frontFace;
        RHIStencilOpState backFace;

        static RHIDepthStencilState Default() { return RHIDepthStencilState{}; }
        static RHIDepthStencilState Disabled() { RHIDepthStencilState s; s.depthTestEnable = false; s.depthWriteEnable = false; return s; }
        static RHIDepthStencilState ReadOnly() { RHIDepthStencilState s; s.depthWriteEnable = false; return s; }
    };

    // =============================================================================
    // Blend State
    // =============================================================================
    struct RHIRenderTargetBlendState
    {
        bool blendEnable = false;
        RHIBlendFactor srcColorBlend = RHIBlendFactor::One;
        RHIBlendFactor dstColorBlend = RHIBlendFactor::Zero;
        RHIBlendOp colorBlendOp = RHIBlendOp::Add;
        RHIBlendFactor srcAlphaBlend = RHIBlendFactor::One;
        RHIBlendFactor dstAlphaBlend = RHIBlendFactor::Zero;
        RHIBlendOp alphaBlendOp = RHIBlendOp::Add;
        uint8 colorWriteMask = 0xF;  // R=1, G=2, B=4, A=8

        static RHIRenderTargetBlendState Opaque() { return RHIRenderTargetBlendState{}; }

        static RHIRenderTargetBlendState AlphaBlend()
        {
            RHIRenderTargetBlendState s;
            s.blendEnable = true;
            s.srcColorBlend = RHIBlendFactor::SrcAlpha;
            s.dstColorBlend = RHIBlendFactor::InvSrcAlpha;
            s.srcAlphaBlend = RHIBlendFactor::One;
            s.dstAlphaBlend = RHIBlendFactor::InvSrcAlpha;
            return s;
        }

        static RHIRenderTargetBlendState Additive()
        {
            RHIRenderTargetBlendState s;
            s.blendEnable = true;
            s.srcColorBlend = RHIBlendFactor::One;
            s.dstColorBlend = RHIBlendFactor::One;
            return s;
        }
    };

    struct RHIBlendState
    {
        bool alphaToCoverageEnable = false;
        bool independentBlendEnable = false;
        RHIRenderTargetBlendState renderTargets[RVX_MAX_RENDER_TARGETS];

        static RHIBlendState Default() { return RHIBlendState{}; }
    };

    // =============================================================================
    // Graphics Pipeline Description
    // =============================================================================
    struct RHIGraphicsPipelineDesc
    {
        // Shaders
        RHIShader* vertexShader = nullptr;
        RHIShader* pixelShader = nullptr;
        RHIShader* geometryShader = nullptr;
        RHIShader* hullShader = nullptr;
        RHIShader* domainShader = nullptr;

        // Tessellation settings
        uint32 tessellationControlPoints = 3;  // Patch control point count (e.g., 3 for triangles)

        // Fixed-function state
        RHIInputLayoutDesc inputLayout;
        RHIRasterizerState rasterizerState;
        RHIDepthStencilState depthStencilState;
        RHIBlendState blendState;
        RHIPrimitiveTopology primitiveTopology = RHIPrimitiveTopology::TriangleList;

        // Render target formats
        uint32 numRenderTargets = 1;
        RHIFormat renderTargetFormats[RVX_MAX_RENDER_TARGETS] = {RHIFormat::RGBA8_UNORM};
        RHIFormat depthStencilFormat = RHIFormat::D24_UNORM_S8_UINT;
        RHISampleCount sampleCount = RHISampleCount::Count1;

        // Pipeline layout
        RHIPipelineLayout* pipelineLayout = nullptr;

        const char* debugName = nullptr;
    };

    // =============================================================================
    // Compute Pipeline Description
    // =============================================================================
    struct RHIComputePipelineDesc
    {
        RHIShader* computeShader = nullptr;
        RHIPipelineLayout* pipelineLayout = nullptr;
        const char* debugName = nullptr;
    };

    // =============================================================================
    // Pipeline Interface
    // =============================================================================
    class RHIPipeline : public RHIResource
    {
    public:
        virtual ~RHIPipeline() = default;

        virtual bool IsCompute() const = 0;
    };

} // namespace RVX
