#pragma once

#include <string>

namespace RVX
{
    struct SampleManifestCatalog;
    class SampleRegistry;

    /** Register the explicit cooked GFX24/GFX25 scenario allowlist. */
    bool RegisterCookedSampleScenarios(
        const SampleManifestCatalog& manifest,
        SampleRegistry& registry,
        std::string* outError = nullptr);
} // namespace RVX
