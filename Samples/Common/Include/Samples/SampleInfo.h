#pragma once

/**
 * @file SampleInfo.h
 * @brief Backend-neutral metadata for one RenderVerseX sample scene.
 */

#include <string>
#include <vector>

namespace RVX
{
    /** @brief Controls whether command-line model overrides are accepted. */
    enum class SampleAssetPolicy
    {
        Fixed = 0,
        CompatibleOverride,
        UserModelOrDefault
    };

    /** @brief Declares whether a sample accepts or requires an environment. */
    enum class SampleEnvironmentPolicy
    {
        None = 0,
        Optional,
        Required
    };

    /** @brief Backend-neutral rendering strategy exposed by selectable samples. */
    enum class SampleRenderPath
    {
        Auto = 0,
        Direct,
        GPUDriven
    };

    inline const char* GetSampleRenderPathName(SampleRenderPath path)
    {
        switch (path)
        {
            case SampleRenderPath::Auto: return "auto";
            case SampleRenderPath::Direct: return "direct";
            case SampleRenderPath::GPUDriven: return "gpu-driven";
            default: return "invalid";
        }
    }

    /** @brief Static metadata used by the sample registry and asset resolver. */
    struct SampleInfo
    {
        std::string id;
        std::string displayName;
        std::string description;
        std::string defaultAssetId;
        SampleAssetPolicy assetPolicy = SampleAssetPolicy::Fixed;
        std::string defaultEnvironmentId;
        SampleEnvironmentPolicy environmentPolicy =
            SampleEnvironmentPolicy::None;
        bool supportsRenderPathSelection = false;
        SampleRenderPath defaultRenderPath = SampleRenderPath::Auto;
        std::vector<std::string> additionalDefaultAssetIds;
    };
} // namespace RVX
