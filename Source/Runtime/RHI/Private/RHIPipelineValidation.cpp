/**
 * @file RHIPipelineValidation.cpp
 * @brief Backend-neutral graphics-pipeline preflight contract.
 */

#include "RHI/RHIPipelineValidation.h"

#include "RHI/RHIDescriptor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string_view>
#include <vector>

namespace RVX
{
    namespace
    {
        enum class FormatClass : uint8
        {
            Unknown = 0,
            Float = 1,
            UInt = 2,
            SInt = 3,
        };

        struct FormatShape
        {
            FormatClass formatClass = FormatClass::Unknown;
            uint8 componentCount = 0;
        };

        std::string UpperAscii(std::string_view value)
        {
            std::string result(value);
            std::transform(
                result.begin(),
                result.end(),
                result.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::toupper(character));
                });
            return result;
        }

        FormatShape GetFormatShape(RHIFormat format)
        {
            switch (format)
            {
                case RHIFormat::R8_UNORM:
                case RHIFormat::R8_SNORM:
                case RHIFormat::R16_FLOAT:
                case RHIFormat::R16_UNORM:
                case RHIFormat::R32_FLOAT:
                    return {FormatClass::Float, 1};
                case RHIFormat::R8_UINT:
                case RHIFormat::R16_UINT:
                case RHIFormat::R32_UINT:
                    return {FormatClass::UInt, 1};
                case RHIFormat::R8_SINT:
                case RHIFormat::R16_SINT:
                case RHIFormat::R32_SINT:
                    return {FormatClass::SInt, 1};

                case RHIFormat::RG8_UNORM:
                case RHIFormat::RG8_SNORM:
                case RHIFormat::RG16_FLOAT:
                case RHIFormat::RG16_UNORM:
                case RHIFormat::RG32_FLOAT:
                    return {FormatClass::Float, 2};
                case RHIFormat::RG8_UINT:
                case RHIFormat::RG16_UINT:
                case RHIFormat::RG32_UINT:
                    return {FormatClass::UInt, 2};
                case RHIFormat::RG8_SINT:
                case RHIFormat::RG16_SINT:
                case RHIFormat::RG32_SINT:
                    return {FormatClass::SInt, 2};

                case RHIFormat::RGB32_FLOAT:
                case RHIFormat::RG11B10_FLOAT:
                    return {FormatClass::Float, 3};
                case RHIFormat::RGB32_UINT:
                    return {FormatClass::UInt, 3};
                case RHIFormat::RGB32_SINT:
                    return {FormatClass::SInt, 3};

                case RHIFormat::RGBA8_UNORM:
                case RHIFormat::RGBA8_UNORM_SRGB:
                case RHIFormat::RGBA8_SNORM:
                case RHIFormat::BGRA8_UNORM:
                case RHIFormat::BGRA8_UNORM_SRGB:
                case RHIFormat::RGB10A2_UNORM:
                case RHIFormat::RGBA16_FLOAT:
                case RHIFormat::RGBA16_UNORM:
                case RHIFormat::RGBA32_FLOAT:
                    return {FormatClass::Float, 4};
                case RHIFormat::RGBA8_UINT:
                case RHIFormat::RGB10A2_UINT:
                case RHIFormat::RGBA16_UINT:
                case RHIFormat::RGBA32_UINT:
                    return {FormatClass::UInt, 4};
                case RHIFormat::RGBA8_SINT:
                case RHIFormat::RGBA16_SINT:
                case RHIFormat::RGBA32_SINT:
                    return {FormatClass::SInt, 4};

                case RHIFormat::Unknown:
                default:
                    return {};
            }
        }

        bool IsValidSampleCount(RHISampleCount sampleCount)
        {
            switch (sampleCount)
            {
                case RHISampleCount::Count1:
                case RHISampleCount::Count2:
                case RHISampleCount::Count4:
                case RHISampleCount::Count8:
                case RHISampleCount::Count16:
                    return true;
                default:
                    return false;
            }
        }

        bool BindingTypesAreCompatible(RHIBindingType layoutType,
                                       RHIBindingType shaderType)
        {
            if (layoutType == shaderType)
            {
                return true;
            }
            return (layoutType == RHIBindingType::DynamicUniformBuffer &&
                    shaderType == RHIBindingType::UniformBuffer) ||
                   (layoutType == RHIBindingType::DynamicStorageBuffer &&
                    shaderType == RHIBindingType::StorageBuffer);
        }

        RHIPipelineValidationResult MakeFailure(
            const RHIGraphicsPipelineDesc& desc,
            RHIPipelineValidationCode code,
            std::string message,
            const RHIShader* shader = nullptr)
        {
            RHIPipelineValidationResult result;
            result.valid = false;
            result.code = code;
            result.message = std::move(message);
            result.pipelineName =
                desc.debugName ? desc.debugName : "<unnamed>";
            if (shader)
            {
                result.shaderName = shader->GetDebugName();
                result.shaderEntryPoint = shader->GetEntryPoint();
            }
            return result;
        }

        RHIPipelineValidationResult ValidateShaderStage(
            const RHIGraphicsPipelineDesc& desc,
            const RHIShader* shader,
            RHIShaderStage expectedStage)
        {
            if (!shader)
            {
                return {};
            }
            if (shader->GetStage() != expectedStage)
            {
                auto result = MakeFailure(
                    desc,
                    RHIPipelineValidationCode::ShaderStageMismatch,
                    "Shader object is bound to the wrong graphics stage.",
                    shader);
                result.expectedCount =
                    static_cast<uint32>(expectedStage);
                result.actualCount =
                    static_cast<uint32>(shader->GetStage());
                return result;
            }
            if (shader->HasInterface() &&
                (shader->GetInterface().schemaVersion !=
                     RVX_RHI_SHADER_INTERFACE_SCHEMA_VERSION ||
                 shader->GetInterface().stage != expectedStage))
            {
                return MakeFailure(
                    desc,
                    RHIPipelineValidationCode::ShaderInterfaceMismatch,
                    "Shader interface stage/schema does not match its RHI shader.",
                    shader);
            }
            return {};
        }

        bool SameSemantic(const RHIInputElement& element,
                          const RHIShaderInterfaceVariable& variable)
        {
            return !variable.semanticName.empty() &&
                   UpperAscii(
                       element.semanticName ? element.semanticName : "") ==
                       UpperAscii(variable.semanticName) &&
                   element.semanticIndex == variable.semanticIndex;
        }

        uint32 EffectiveLocation(const RHIInputElement& element,
                                 size_t elementIndex)
        {
            return element.location != RVX_INVALID_INDEX
                       ? element.location
                       : static_cast<uint32>(elementIndex);
        }

        RHIPipelineValidationResult ValidateVertexInputs(
            const RHIGraphicsPipelineDesc& desc)
        {
            const RHIShader* shader = desc.vertexShader;
            const auto& elements = desc.inputLayout.elements;
            for (size_t i = 0; i < elements.size(); ++i)
            {
                for (size_t j = i + 1; j < elements.size(); ++j)
                {
                    if (UpperAscii(elements[i].semanticName
                                       ? elements[i].semanticName
                                       : "") ==
                            UpperAscii(elements[j].semanticName
                                           ? elements[j].semanticName
                                           : "") &&
                        elements[i].semanticIndex ==
                            elements[j].semanticIndex)
                    {
                        auto result = MakeFailure(
                            desc,
                            RHIPipelineValidationCode::
                                DuplicateInputSemantic,
                            "Input layout contains a duplicate semantic.",
                            shader);
                        result.semanticName =
                            elements[i].semanticName
                                ? elements[i].semanticName
                                : "";
                        result.semanticIndex =
                            elements[i].semanticIndex;
                        return result;
                    }
                    if (EffectiveLocation(elements[i], i) ==
                        EffectiveLocation(elements[j], j))
                    {
                        auto result = MakeFailure(
                            desc,
                            RHIPipelineValidationCode::
                                DuplicateInputLocation,
                            "Input layout contains a duplicate effective location.",
                            shader);
                        result.location =
                            EffectiveLocation(elements[i], i);
                        return result;
                    }
                }
            }

            if (!shader || !shader->HasInterface())
            {
                return {};
            }

            const auto& shaderInputs = shader->GetInterface().inputs;
            for (size_t i = 0; i < shaderInputs.size(); ++i)
            {
                if (shaderInputs[i].systemValue)
                {
                    continue;
                }
                for (size_t j = i + 1; j < shaderInputs.size(); ++j)
                {
                    if (shaderInputs[j].systemValue)
                    {
                        continue;
                    }
                    const bool sameLocation =
                        shaderInputs[i].location != RVX_INVALID_INDEX &&
                        shaderInputs[i].location ==
                            shaderInputs[j].location;
                    const bool sameSemantic =
                        !shaderInputs[i].semanticName.empty() &&
                        UpperAscii(shaderInputs[i].semanticName) ==
                            UpperAscii(shaderInputs[j].semanticName) &&
                        shaderInputs[i].semanticIndex ==
                            shaderInputs[j].semanticIndex;
                    if (sameLocation || sameSemantic)
                    {
                        auto result = MakeFailure(
                            desc,
                            RHIPipelineValidationCode::DuplicateShaderInput,
                            "Vertex shader interface contains a duplicate input.",
                            shader);
                        result.semanticName =
                            shaderInputs[i].semanticName;
                        result.semanticIndex =
                            shaderInputs[i].semanticIndex;
                        result.location = shaderInputs[i].location;
                        return result;
                    }
                }
            }

            std::vector<bool> matched(elements.size(), false);
            for (const RHIShaderInterfaceVariable& input : shaderInputs)
            {
                if (input.systemValue)
                {
                    continue;
                }

                size_t matchIndex = elements.size();
                if (!input.semanticName.empty())
                {
                    for (size_t i = 0; i < elements.size(); ++i)
                    {
                        if (SameSemantic(elements[i], input))
                        {
                            matchIndex = i;
                            break;
                        }
                    }
                }
                if (matchIndex == elements.size() &&
                    input.location != RVX_INVALID_INDEX)
                {
                    for (size_t i = 0; i < elements.size(); ++i)
                    {
                        const bool explicitLocationMatch =
                            elements[i].location != RVX_INVALID_INDEX &&
                            elements[i].location == input.location;
                        const bool unnamedImplicitMatch =
                            input.semanticName.empty() &&
                            elements[i].location == RVX_INVALID_INDEX &&
                            EffectiveLocation(elements[i], i) ==
                                input.location;
                        if (explicitLocationMatch || unnamedImplicitMatch)
                        {
                            matchIndex = i;
                            break;
                        }
                    }
                }

                if (matchIndex == elements.size())
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::MissingVertexInput,
                        "Input layout is missing a vertex shader input.",
                        shader);
                    result.semanticName = input.semanticName;
                    result.semanticIndex = input.semanticIndex;
                    result.location = input.location;
                    result.expectedFormat = input.format;
                    return result;
                }

                const RHIInputElement& element = elements[matchIndex];
                if (!AreRHIShaderInterfaceFormatsCompatible(
                        element.format,
                        input.format))
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            VertexInputFormatMismatch,
                        "Input layout format is incompatible with the vertex shader input.",
                        shader);
                    result.semanticName = input.semanticName;
                    result.semanticIndex = input.semanticIndex;
                    result.location = input.location;
                    result.expectedFormat = input.format;
                    result.actualFormat = element.format;
                    return result;
                }
                matched[matchIndex] = true;
            }

            const auto extra = std::find(matched.begin(),
                                         matched.end(),
                                         false);
            if (extra != matched.end())
            {
                const size_t index =
                    static_cast<size_t>(extra - matched.begin());
                auto result = MakeFailure(
                    desc,
                    RHIPipelineValidationCode::UnexpectedVertexInput,
                    "Input layout contains an input absent from the vertex shader interface.",
                    shader);
                result.semanticName =
                    elements[index].semanticName
                        ? elements[index].semanticName
                        : "";
                result.semanticIndex = elements[index].semanticIndex;
                result.location =
                    EffectiveLocation(elements[index], index);
                result.actualFormat = elements[index].format;
                return result;
            }
            return {};
        }

        bool IsFragmentColorOutput(
            const RHIShaderInterfaceVariable& output)
        {
            const std::string semantic = UpperAscii(output.semanticName);
            return !output.systemValue ||
                   semantic == "SV_TARGET" ||
                   semantic == "SV_TARGET0" ||
                   semantic.rfind("SV_TARGET", 0) == 0;
        }

        uint32 GetFragmentOutputLocation(
            const RHIShaderInterfaceVariable& output)
        {
            if (output.location != RVX_INVALID_INDEX)
            {
                return output.location;
            }
            const std::string semantic = UpperAscii(output.semanticName);
            if (semantic.rfind("SV_TARGET", 0) == 0)
            {
                return output.semanticIndex;
            }
            return RVX_INVALID_INDEX;
        }

        RHIPipelineValidationResult ValidateFragmentOutputs(
            const RHIGraphicsPipelineDesc& desc)
        {
            if (!desc.pixelShader)
            {
                if (desc.numRenderTargets != 0)
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::MissingFragmentOutput,
                        "A pipeline without a pixel shader cannot declare color attachments.");
                    result.expectedCount = 0;
                    result.actualCount = desc.numRenderTargets;
                    return result;
                }
                return {};
            }
            if (!desc.pixelShader->HasInterface())
            {
                return {};
            }

            std::array<const RHIShaderInterfaceVariable*,
                       RVX_MAX_RENDER_TARGETS>
                outputs{};
            for (const RHIShaderInterfaceVariable& output :
                 desc.pixelShader->GetInterface().outputs)
            {
                if (!IsFragmentColorOutput(output))
                {
                    continue;
                }
                const uint32 location =
                    GetFragmentOutputLocation(output);
                if (location == RVX_INVALID_INDEX ||
                    location >= RVX_MAX_RENDER_TARGETS)
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            UnexpectedFragmentOutput,
                        "Pixel shader output has no valid color-attachment location.",
                        desc.pixelShader);
                    result.semanticName = output.semanticName;
                    result.semanticIndex = output.semanticIndex;
                    result.location = location;
                    return result;
                }
                if (outputs[location])
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            DuplicateFragmentOutput,
                        "Pixel shader interface contains duplicate color outputs.",
                        desc.pixelShader);
                    result.location = location;
                    return result;
                }
                outputs[location] = &output;
            }

            for (uint32 location = 0;
                 location < RVX_MAX_RENDER_TARGETS;
                 ++location)
            {
                if (location < desc.numRenderTargets)
                {
                    if (!outputs[location])
                    {
                        auto result = MakeFailure(
                            desc,
                            RHIPipelineValidationCode::
                                MissingFragmentOutput,
                            "Pipeline color attachment has no matching pixel shader output.",
                            desc.pixelShader);
                        result.location = location;
                        result.expectedCount =
                            desc.numRenderTargets;
                        return result;
                    }
                    if (!AreRHIShaderInterfaceFormatsCompatible(
                            desc.renderTargetFormats[location],
                            outputs[location]->format))
                    {
                        auto result = MakeFailure(
                            desc,
                            RHIPipelineValidationCode::
                                FragmentOutputFormatMismatch,
                            "Color attachment format is incompatible with the pixel shader output.",
                            desc.pixelShader);
                        result.location = location;
                        result.expectedFormat =
                            outputs[location]->format;
                        result.actualFormat =
                            desc.renderTargetFormats[location];
                        return result;
                    }
                }
                else if (outputs[location])
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            UnexpectedFragmentOutput,
                        "Pixel shader writes a color output without a pipeline attachment.",
                        desc.pixelShader);
                    result.location = location;
                    result.actualCount = desc.numRenderTargets;
                    return result;
                }
            }
            return {};
        }

        RHIPipelineValidationResult ValidateShaderLayout(
            const RHIGraphicsPipelineDesc& desc,
            const RHIShader* shader)
        {
            if (!shader || !shader->HasInterface() ||
                !desc.pipelineLayout ||
                !desc.pipelineLayout->HasDescription())
            {
                return {};
            }

            const auto& setLayouts =
                desc.pipelineLayout->GetDescriptorSetLayouts();
            for (const RHIShaderInterfaceBinding& binding :
                 shader->GetInterface().bindings)
            {
                if (binding.set >= setLayouts.size() ||
                    !setLayouts[binding.set])
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::MissingDescriptorSet,
                        "Pipeline layout is missing a shader descriptor set.",
                        shader);
                    result.set = binding.set;
                    result.binding = binding.binding;
                    return result;
                }
                const RHIBindingLayoutEntry* entry =
                    FindRHIBindingLayoutEntry(
                        *setLayouts[binding.set],
                        binding.binding);
                if (!entry)
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            MissingResourceBinding,
                        "Pipeline layout is missing a shader resource binding.",
                        shader);
                    result.set = binding.set;
                    result.binding = binding.binding;
                    return result;
                }
                if (!BindingTypesAreCompatible(entry->type,
                                               binding.type))
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            ResourceBindingTypeMismatch,
                        "Pipeline layout binding type does not match the shader interface.",
                        shader);
                    result.set = binding.set;
                    result.binding = binding.binding;
                    result.expectedCount =
                        static_cast<uint32>(binding.type);
                    result.actualCount =
                        static_cast<uint32>(entry->type);
                    return result;
                }
                if (entry->count < binding.count)
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            ResourceBindingCountMismatch,
                        "Pipeline layout binding count is smaller than the shader requirement.",
                        shader);
                    result.set = binding.set;
                    result.binding = binding.binding;
                    result.expectedCount = binding.count;
                    result.actualCount = entry->count;
                    return result;
                }
                if (!HasFlag(entry->visibility, shader->GetStage()))
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            ResourceBindingVisibilityMismatch,
                        "Pipeline layout binding is not visible to the shader stage.",
                        shader);
                    result.set = binding.set;
                    result.binding = binding.binding;
                    return result;
                }
            }

            for (const RHIShaderInterfacePushConstantRange& range :
                 shader->GetInterface().pushConstants)
            {
                const uint64 rangeEnd =
                    static_cast<uint64>(range.offset) + range.size;
                if (rangeEnd >
                        desc.pipelineLayout->
                            GetDeclaredPushConstantSize() ||
                    !HasFlag(
                        desc.pipelineLayout->
                            GetDeclaredPushConstantStages(),
                        shader->GetStage()))
                {
                    auto result = MakeFailure(
                        desc,
                        RHIPipelineValidationCode::
                            PushConstantRangeMismatch,
                        "Pipeline layout push-constant range/stages do not satisfy the shader interface.",
                        shader);
                    result.expectedCount =
                        static_cast<uint32>(rangeEnd);
                    result.actualCount =
                        desc.pipelineLayout->
                            GetDeclaredPushConstantSize();
                    return result;
                }
            }
            return {};
        }
    } // namespace

    const char* GetRHIPipelineValidationCodeName(
        RHIPipelineValidationCode code)
    {
        switch (code)
        {
            case RHIPipelineValidationCode::Passed:
                return "Passed";
            case RHIPipelineValidationCode::MissingVertexShader:
                return "MissingVertexShader";
            case RHIPipelineValidationCode::ShaderStageMismatch:
                return "ShaderStageMismatch";
            case RHIPipelineValidationCode::ShaderInterfaceMismatch:
                return "ShaderInterfaceMismatch";
            case RHIPipelineValidationCode::MissingPipelineLayout:
                return "MissingPipelineLayout";
            case RHIPipelineValidationCode::InvalidRenderTargetCount:
                return "InvalidRenderTargetCount";
            case RHIPipelineValidationCode::InvalidRenderTargetFormat:
                return "InvalidRenderTargetFormat";
            case RHIPipelineValidationCode::InvalidDepthStencilFormat:
                return "InvalidDepthStencilFormat";
            case RHIPipelineValidationCode::InvalidSampleCount:
                return "InvalidSampleCount";
            case RHIPipelineValidationCode::DuplicateShaderInput:
                return "DuplicateShaderInput";
            case RHIPipelineValidationCode::DuplicateInputSemantic:
                return "DuplicateInputSemantic";
            case RHIPipelineValidationCode::DuplicateInputLocation:
                return "DuplicateInputLocation";
            case RHIPipelineValidationCode::MissingVertexInput:
                return "MissingVertexInput";
            case RHIPipelineValidationCode::UnexpectedVertexInput:
                return "UnexpectedVertexInput";
            case RHIPipelineValidationCode::VertexInputFormatMismatch:
                return "VertexInputFormatMismatch";
            case RHIPipelineValidationCode::DuplicateFragmentOutput:
                return "DuplicateFragmentOutput";
            case RHIPipelineValidationCode::MissingFragmentOutput:
                return "MissingFragmentOutput";
            case RHIPipelineValidationCode::UnexpectedFragmentOutput:
                return "UnexpectedFragmentOutput";
            case RHIPipelineValidationCode::FragmentOutputFormatMismatch:
                return "FragmentOutputFormatMismatch";
            case RHIPipelineValidationCode::MissingDescriptorSet:
                return "MissingDescriptorSet";
            case RHIPipelineValidationCode::MissingResourceBinding:
                return "MissingResourceBinding";
            case RHIPipelineValidationCode::ResourceBindingTypeMismatch:
                return "ResourceBindingTypeMismatch";
            case RHIPipelineValidationCode::ResourceBindingCountMismatch:
                return "ResourceBindingCountMismatch";
            case RHIPipelineValidationCode::
                ResourceBindingVisibilityMismatch:
                return "ResourceBindingVisibilityMismatch";
            case RHIPipelineValidationCode::PushConstantRangeMismatch:
                return "PushConstantRangeMismatch";
            case RHIPipelineValidationCode::InvalidVertexInputLayout:
                return "InvalidVertexInputLayout";
            default:
                return "Unknown";
        }
    }

    std::string FormatRHIPipelineValidationResult(
        const RHIPipelineValidationResult& result)
    {
        std::ostringstream stream;
        stream << GetRHIPipelineValidationCodeName(result.code)
               << ": " << result.message;
        if (!result.pipelineName.empty())
        {
            stream << " pipeline='" << result.pipelineName << "'";
        }
        if (!result.shaderName.empty())
        {
            stream << " shader='" << result.shaderName << "'";
        }
        if (!result.shaderEntryPoint.empty())
        {
            stream << " entry='" << result.shaderEntryPoint << "'";
        }
        if (!result.semanticName.empty())
        {
            stream << " semantic='" << result.semanticName
                   << result.semanticIndex << "'";
        }
        if (result.location != RVX_INVALID_INDEX)
        {
            stream << " location=" << result.location;
        }
        if (result.set != RVX_INVALID_INDEX)
        {
            stream << " set=" << result.set;
        }
        if (result.binding != RVX_INVALID_INDEX)
        {
            stream << " binding=" << result.binding;
        }
        if (result.expectedFormat != RHIFormat::Unknown ||
            result.actualFormat != RHIFormat::Unknown)
        {
            stream << " expectedFormat="
                   << static_cast<uint32>(result.expectedFormat)
                   << " actualFormat="
                   << static_cast<uint32>(result.actualFormat);
        }
        if (result.expectedCount != 0 || result.actualCount != 0)
        {
            stream << " expected=" << result.expectedCount
                   << " actual=" << result.actualCount;
        }
        return stream.str();
    }

    RHIVertexInputTranslation BuildRHIVertexInputTranslation(
        const RHIInputLayoutDesc& inputLayout)
    {
        RHIVertexInputTranslation translation;
        translation.attributes.reserve(inputLayout.elements.size());

        for (size_t elementIndex = 0;
             elementIndex < inputLayout.elements.size();
             ++elementIndex)
        {
            const RHIInputElement& element =
                inputLayout.elements[elementIndex];
            const uint32 location =
                element.location != RVX_INVALID_INDEX
                    ? element.location
                    : static_cast<uint32>(elementIndex);
            const auto duplicateLocation = std::find_if(
                translation.attributes.begin(),
                translation.attributes.end(),
                [location](
                    const RHIVertexInputAttributeTranslation& attribute)
                {
                    return attribute.location == location;
                });
            if (duplicateLocation != translation.attributes.end())
            {
                translation.valid = false;
                translation.message =
                    "Vertex input elements must have unique effective locations.";
                translation.elementIndex =
                    static_cast<uint32>(elementIndex);
                translation.inputSlot = element.inputSlot;
                return translation;
            }
            if (element.inputSlot >= RVX_MAX_VERTEX_BUFFERS)
            {
                translation.valid = false;
                translation.message =
                    "Vertex input slot exceeds the portable RHI limit.";
                translation.elementIndex =
                    static_cast<uint32>(elementIndex);
                translation.inputSlot = element.inputSlot;
                return translation;
            }
            if ((!element.perInstance &&
                 element.instanceDataStepRate != 0) ||
                (element.perInstance &&
                 element.instanceDataStepRate != 1))
            {
                translation.valid = false;
                translation.message =
                    "Portable vertex input requires step rate 0 for per-vertex data and 1 for per-instance data.";
                translation.elementIndex =
                    static_cast<uint32>(elementIndex);
                translation.inputSlot = element.inputSlot;
                return translation;
            }
            auto bindingIt = std::find_if(
                translation.bindings.begin(),
                translation.bindings.end(),
                [&element](
                    const RHIVertexInputBindingTranslation& binding)
                {
                    return binding.inputSlot == element.inputSlot;
                });
            if (bindingIt == translation.bindings.end())
            {
                translation.bindings.push_back(
                    {element.inputSlot,
                     0,
                     element.perInstance,
                     element.instanceDataStepRate});
                bindingIt = translation.bindings.end() - 1;
            }
            else if (
                bindingIt->perInstance != element.perInstance ||
                bindingIt->instanceDataStepRate !=
                    element.instanceDataStepRate)
            {
                translation.valid = false;
                translation.message =
                    "Elements in one vertex-buffer slot must use the same input rate and step rate.";
                translation.elementIndex =
                    static_cast<uint32>(elementIndex);
                translation.inputSlot = element.inputSlot;
                return translation;
            }

            const uint32 elementSize =
                GetFormatBytesPerPixel(element.format);
            if (elementSize == 0 ||
                GetFormatShape(element.format).componentCount == 0)
            {
                translation.valid = false;
                translation.message =
                    "Vertex input element uses a format unsupported by the portable vertex contract.";
                translation.elementIndex =
                    static_cast<uint32>(elementIndex);
                translation.inputSlot = element.inputSlot;
                return translation;
            }

            const uint32 offset =
                element.alignedByteOffset == RVX_INVALID_INDEX
                    ? bindingIt->stride
                    : element.alignedByteOffset;
            const uint64 elementEnd =
                static_cast<uint64>(offset) + elementSize;
            if (elementEnd > UINT32_MAX)
            {
                translation.valid = false;
                translation.message =
                    "Vertex input element offset and size overflow the RHI stride range.";
                translation.elementIndex =
                    static_cast<uint32>(elementIndex);
                translation.inputSlot = element.inputSlot;
                return translation;
            }

            bindingIt->stride = std::max(
                bindingIt->stride,
                static_cast<uint32>(elementEnd));
            translation.attributes.push_back(
                {static_cast<uint32>(elementIndex),
                 location,
                 element.inputSlot,
                 offset});
        }

        std::sort(
            translation.bindings.begin(),
            translation.bindings.end(),
            [](const RHIVertexInputBindingTranslation& lhs,
               const RHIVertexInputBindingTranslation& rhs)
            {
                return lhs.inputSlot < rhs.inputSlot;
            });
        return translation;
    }

    bool AreRHIShaderInterfaceFormatsCompatible(
        RHIFormat actualFormat,
        RHIFormat shaderExpectedFormat)
    {
        const FormatShape actual = GetFormatShape(actualFormat);
        const FormatShape expected =
            GetFormatShape(shaderExpectedFormat);
        return actual.componentCount != 0 &&
               actual.componentCount == expected.componentCount &&
               actual.formatClass == expected.formatClass;
    }

    RHIPipelineValidationResult ValidateRHIGraphicsPipelineDesc(
        const RHIGraphicsPipelineDesc& desc)
    {
        if (!desc.vertexShader)
        {
            return MakeFailure(
                desc,
                RHIPipelineValidationCode::MissingVertexShader,
                "Graphics pipelines require a vertex shader.");
        }
        if (!desc.pipelineLayout)
        {
            return MakeFailure(
                desc,
                RHIPipelineValidationCode::MissingPipelineLayout,
                "Graphics pipelines require a pipeline layout.");
        }
        if (desc.numRenderTargets > RVX_MAX_RENDER_TARGETS)
        {
            auto result = MakeFailure(
                desc,
                RHIPipelineValidationCode::InvalidRenderTargetCount,
                "Graphics pipeline color-attachment count exceeds the RHI limit.");
            result.expectedCount = RVX_MAX_RENDER_TARGETS;
            result.actualCount = desc.numRenderTargets;
            return result;
        }
        for (uint32 i = 0; i < desc.numRenderTargets; ++i)
        {
            if (desc.renderTargetFormats[i] == RHIFormat::Unknown ||
                IsDepthFormat(desc.renderTargetFormats[i]))
            {
                auto result = MakeFailure(
                    desc,
                    RHIPipelineValidationCode::
                        InvalidRenderTargetFormat,
                    "Graphics pipeline color attachment has an invalid format.");
                result.location = i;
                result.actualFormat = desc.renderTargetFormats[i];
                return result;
            }
        }
        const bool requiresDepthStencil =
            desc.depthStencilState.depthTestEnable ||
            desc.depthStencilState.depthWriteEnable ||
            desc.depthStencilState.stencilTestEnable;
        if ((requiresDepthStencil &&
             !IsDepthFormat(desc.depthStencilFormat)) ||
            (!requiresDepthStencil &&
             desc.depthStencilFormat != RHIFormat::Unknown &&
             !IsDepthFormat(desc.depthStencilFormat)))
        {
            auto result = MakeFailure(
                desc,
                RHIPipelineValidationCode::
                    InvalidDepthStencilFormat,
                "Graphics pipeline depth/stencil state and format are incompatible.");
            result.actualFormat = desc.depthStencilFormat;
            return result;
        }
        if (!IsValidSampleCount(desc.sampleCount))
        {
            return MakeFailure(
                desc,
                RHIPipelineValidationCode::InvalidSampleCount,
                "Graphics pipeline sample count is invalid.");
        }

        const std::array<std::pair<const RHIShader*, RHIShaderStage>, 5>
            shaderStages = {{
                {desc.vertexShader, RHIShaderStage::Vertex},
                {desc.pixelShader, RHIShaderStage::Pixel},
                {desc.geometryShader, RHIShaderStage::Geometry},
                {desc.hullShader, RHIShaderStage::Hull},
                {desc.domainShader, RHIShaderStage::Domain},
            }};
        for (const auto& [shader, stage] : shaderStages)
        {
            RHIPipelineValidationResult result =
                ValidateShaderStage(desc, shader, stage);
            if (!result)
            {
                return result;
            }
        }
        if ((desc.hullShader == nullptr) !=
            (desc.domainShader == nullptr))
        {
            return MakeFailure(
                desc,
                RHIPipelineValidationCode::ShaderStageMismatch,
                "Hull and domain shaders must be supplied together.");
        }

        RHIPipelineValidationResult result =
            ValidateVertexInputs(desc);
        if (!result)
        {
            return result;
        }
        const RHIVertexInputTranslation vertexInputTranslation =
            BuildRHIVertexInputTranslation(desc.inputLayout);
        if (!vertexInputTranslation)
        {
            result = MakeFailure(
                desc,
                RHIPipelineValidationCode::InvalidVertexInputLayout,
                vertexInputTranslation.message,
                desc.vertexShader);
            result.location =
                vertexInputTranslation.elementIndex;
            result.binding =
                vertexInputTranslation.inputSlot;
            return result;
        }
        result = ValidateFragmentOutputs(desc);
        if (!result)
        {
            return result;
        }
        for (const auto& [shader, stage] : shaderStages)
        {
            (void)stage;
            result = ValidateShaderLayout(desc, shader);
            if (!result)
            {
                return result;
            }
        }
        return {};
    }
} // namespace RVX
