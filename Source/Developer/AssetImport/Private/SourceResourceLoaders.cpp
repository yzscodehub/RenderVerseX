/** @file SourceResourceLoaders.cpp @brief Development source loader adapters */

#include "AssetImport/SourceResourceLoaders.h"

namespace RVX::AssetImport
{
SourceAudioLoader::SourceAudioLoader(Resource::ResourceManager* manager)
    : Resource::AudioLoader(manager)
{
}

SourceEnvironmentLoader::SourceEnvironmentLoader(
    Resource::EnvironmentLoadOptions options)
    : Resource::EnvironmentLoader(options)
{
}

SourceTextureLoader::SourceTextureLoader(Resource::ResourceManager* manager)
    : Resource::TextureLoader(manager)
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

SourceModelLoader::SourceModelLoader(Resource::ResourceManager* manager)
    : Resource::ModelLoader(manager)
{
}
} // namespace RVX::AssetImport
