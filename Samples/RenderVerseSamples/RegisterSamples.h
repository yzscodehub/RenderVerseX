#pragma once

#include <string>

namespace RVX
{
    struct SampleManifestCatalog;
    class SampleRegistry;

    bool RegisterRenderVerseSamples(const SampleManifestCatalog& manifest,
                                    SampleRegistry& registry,
                                    std::string* outError = nullptr);
} // namespace RVX
