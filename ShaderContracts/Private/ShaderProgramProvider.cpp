/** @file ShaderProgramProvider.cpp @brief Runtime shader reflection utilities */

#include "ShaderContracts/ShaderProgramProvider.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace RVX
{
namespace
{
    std::string UpperAscii(std::string value)
    {
        std::transform(
            value.begin(), value.end(), value.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::toupper(character));
            });
        return value;
    }
} // namespace

RHIShaderInterface BuildRHIShaderInterface(
    RHIShaderStage stage,
    const ShaderReflection& reflection)
{
    RHIShaderInterface shaderInterface;
    shaderInterface.available = reflection.valid;
    shaderInterface.stage = stage;

    const auto appendVariables = [](
        const std::vector<ShaderReflection::InputAttribute>& source,
        std::vector<RHIShaderInterfaceVariable>& destination)
    {
        destination.reserve(source.size());
        for (const ShaderReflection::InputAttribute& attribute : source)
        {
            RHIShaderInterfaceVariable variable;
            variable.location = attribute.location;
            variable.format = attribute.format;
            variable.systemValue = attribute.systemValue;
            variable.semanticName = UpperAscii(attribute.semantic);
            variable.semanticIndex = attribute.semanticIndex;
            destination.push_back(std::move(variable));
        }
    };
    appendVariables(reflection.inputs, shaderInterface.inputs);
    appendVariables(reflection.outputs, shaderInterface.outputs);

    shaderInterface.bindings.reserve(reflection.resources.size());
    for (const ShaderReflection::ResourceBinding& resource :
         reflection.resources)
    {
        shaderInterface.bindings.push_back({
            resource.set,
            resource.binding,
            resource.type,
            resource.count,
        });
    }
    shaderInterface.pushConstants.reserve(
        reflection.pushConstants.size());
    for (const ShaderReflection::PushConstantRange& range :
         reflection.pushConstants)
    {
        shaderInterface.pushConstants.push_back(
            {range.offset, range.size});
    }

    return FinalizeRHIShaderInterface(std::move(shaderInterface));
}
} // namespace RVX
