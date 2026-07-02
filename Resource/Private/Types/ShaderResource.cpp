#include "Resource/Types/ShaderResource.h"

#include <utility>

namespace RVX::Resource
{

ShaderResource::ShaderResource() = default;
ShaderResource::~ShaderResource() = default;

void ShaderResource::SetData(std::vector<std::uint8_t> bytecode,
                             std::string glslSource,
                             std::string mslSource,
                             const ShaderMetadata& metadata)
{
    m_bytecode = std::move(bytecode);
    m_glslSource = std::move(glslSource);
    m_mslSource = std::move(mslSource);
    m_metadata = metadata;
}

size_t ShaderResource::GetMemoryUsage() const
{
    return sizeof(*this) +
           m_metadata.sourcePath.size() +
           m_metadata.entryPoint.size() +
           m_metadata.targetProfile.size() +
           m_bytecode.size() +
           m_glslSource.size() +
           m_mslSource.size();
}

size_t ShaderResource::GetGPUMemoryUsage() const
{
    return 0;
}

} // namespace RVX::Resource
