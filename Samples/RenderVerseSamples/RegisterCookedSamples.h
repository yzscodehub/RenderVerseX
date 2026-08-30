#pragma once

#include <string>

namespace RVX
{
    struct SampleManifestCatalog;
    class SampleRegistry;

    /** Register the only scenario compiled into the cooked GFX24 product. */
    bool RegisterRenderQualityLabCookedSample(
        const SampleManifestCatalog& manifest,
        SampleRegistry& registry,
        std::string* outError = nullptr);
} // namespace RVX
