#pragma once

/**
 * @file RHIPipelineValidation.h
 * @brief Backend-neutral graphics-pipeline preflight contract.
 */

#include "RHI/RHIPipeline.h"

#include <string>
#include <vector>

namespace RVX
{
    enum class RHIPipelineValidationCode : uint8
    {
        Passed = 0,
        MissingVertexShader = 1,
        ShaderStageMismatch = 2,
        ShaderInterfaceMismatch = 3,
        MissingPipelineLayout = 4,
        InvalidRenderTargetCount = 5,
        InvalidRenderTargetFormat = 6,
        InvalidDepthStencilFormat = 7,
        InvalidSampleCount = 8,
        DuplicateShaderInput = 9,
        DuplicateInputSemantic = 10,
        DuplicateInputLocation = 11,
        MissingVertexInput = 12,
        UnexpectedVertexInput = 13,
        VertexInputFormatMismatch = 14,
        DuplicateFragmentOutput = 15,
        MissingFragmentOutput = 16,
        UnexpectedFragmentOutput = 17,
        FragmentOutputFormatMismatch = 18,
        MissingDescriptorSet = 19,
        MissingResourceBinding = 20,
        ResourceBindingTypeMismatch = 21,
        ResourceBindingCountMismatch = 22,
        ResourceBindingVisibilityMismatch = 23,
        PushConstantRangeMismatch = 24,
        InvalidVertexInputLayout = 25,
    };

    struct RHIPipelineValidationResult
    {
        bool valid = true;
        RHIPipelineValidationCode code =
            RHIPipelineValidationCode::Passed;
        std::string message;
        std::string pipelineName;
        std::string shaderName;
        std::string shaderEntryPoint;
        std::string semanticName;
        uint32 semanticIndex = 0;
        uint32 location = RVX_INVALID_INDEX;
        uint32 set = RVX_INVALID_INDEX;
        uint32 binding = RVX_INVALID_INDEX;
        RHIFormat expectedFormat = RHIFormat::Unknown;
        RHIFormat actualFormat = RHIFormat::Unknown;
        uint32 expectedCount = 0;
        uint32 actualCount = 0;

        explicit operator bool() const
        {
            return valid;
        }
    };

    struct RHIVertexInputAttributeTranslation
    {
        uint32 elementIndex = RVX_INVALID_INDEX;
        uint32 location = RVX_INVALID_INDEX;
        uint32 inputSlot = 0;
        uint32 alignedByteOffset = 0;
    };

    struct RHIVertexInputBindingTranslation
    {
        uint32 inputSlot = 0;
        uint32 stride = 0;
        bool perInstance = false;
        uint32 instanceDataStepRate = 0;
    };

    /**
     * @brief Deterministic backend-neutral vertex-layout translation.
     *
     * Attribute order follows the source layout. Bindings are sorted by slot.
     */
    struct RHIVertexInputTranslation
    {
        bool valid = true;
        std::string message;
        uint32 elementIndex = RVX_INVALID_INDEX;
        uint32 inputSlot = RVX_INVALID_INDEX;
        std::vector<RHIVertexInputAttributeTranslation> attributes;
        std::vector<RHIVertexInputBindingTranslation> bindings;

        explicit operator bool() const
        {
            return valid;
        }
    };

    const char* GetRHIPipelineValidationCodeName(
        RHIPipelineValidationCode code);

    /** @brief Build one stable diagnostic line from a validation result. */
    std::string FormatRHIPipelineValidationResult(
        const RHIPipelineValidationResult& result);

    RHIVertexInputTranslation BuildRHIVertexInputTranslation(
        const RHIInputLayoutDesc& inputLayout);

    /**
     * @brief Check whether a vertex/attachment format satisfies a shader
     * signature format.
     */
    bool AreRHIShaderInterfaceFormatsCompatible(
        RHIFormat actualFormat,
        RHIFormat shaderExpectedFormat);

    /**
     * @brief Reject invalid graphics descriptors before native API creation.
     */
    RHIPipelineValidationResult ValidateRHIGraphicsPipelineDesc(
        const RHIGraphicsPipelineDesc& desc);
} // namespace RVX
