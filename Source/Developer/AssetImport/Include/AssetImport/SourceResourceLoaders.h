#pragma once

/** @file SourceResourceLoaders.h @brief Development source loader adapters */

#include "AssetImport/AudioLoader.h"
#include "AssetImport/EnvironmentLoader.h"
#include "AssetImport/ModelLoader.h"
#include "AssetImport/TextureLoader.h"

namespace RVX::AssetImport
{
    /** @brief Audio decoder selected exclusively for Source requests. */
    class SourceAudioLoader final : public Resource::AudioLoader
    {
    };

    /** @brief HDR/EXR IBL importer selected exclusively for Source requests. */
    class SourceEnvironmentLoader final : public Resource::EnvironmentLoader
    {
    public:
        explicit SourceEnvironmentLoader(
            Resource::EnvironmentLoadOptions options = {});
    };

    /** @brief Texture importer selected exclusively for Source requests. */
    class SourceTextureLoader final
        : public Resource::TextureLoader,
          public Resource::ITextureSourceDecoder
    {
    public:
        SourceTextureLoader() = default;

        bool Decode(
            const Resource::TextureReference& reference,
            const std::string& modelPath,
            const Diagnostics::TraceContext& traceContext,
            Resource::DecodedTextureData& outData,
            std::string& outError,
            const std::function<bool()>& isCancellationRequested = {}) override;
    };

    /** @brief Model importer selected exclusively for Source requests. */
    class SourceModelLoader final : public Resource::ModelLoader
    {
    };
} // namespace RVX::AssetImport
