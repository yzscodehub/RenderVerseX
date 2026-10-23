/** @file SourceResourceLoaders.cpp @brief Development source loader adapters */

#include "AssetImport/SourceResourceLoaders.h"

namespace RVX::AssetImport
{
SourceEnvironmentLoader::SourceEnvironmentLoader(
    Resource::EnvironmentLoadOptions options)
    : Resource::EnvironmentLoader(options)
{
}

bool SourceTextureLoader::Decode(
    const Resource::TextureReference& reference,
    const std::string& modelPath,
    const Diagnostics::TraceContext& traceContext,
    Resource::DecodedTextureData& outData,
    std::string& outError,
    const std::function<bool()>& isCancellationRequested)
{
    return DecodeReference(
        reference,
        modelPath,
        traceContext,
        outData,
        outError,
        isCancellationRequested);
}

} // namespace RVX::AssetImport
