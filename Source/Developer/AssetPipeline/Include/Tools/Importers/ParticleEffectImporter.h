#pragma once

/** @file ParticleEffectImporter.h @brief Strict .rvxparticle to .rvxvfx cooker. */

#include "Tools/AssetPipeline.h"

namespace RVX::Tools
{
    /**
     * @brief Cooks the deliberately small CPU-billboard particle authoring schema.
     *
     * The importer accepts no generic module graph, RHI state, GPU simulation,
     * mesh/trail mode, or embedded texture payload.  Those are rejected rather
     * than silently approximated by the runtime product.
     */
    class ParticleEffectImporter final : public IAssetImporter
    {
    public:
        const char* GetName() const override { return "ParticleEffectImporter"; }
        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".rvxparticle"};
        }
        AssetType GetAssetType() const override { return AssetType::Effect; }
        ImportResult Import(const fs::path& sourcePath,
                            const fs::path& outputPath,
                            const void* options = nullptr) override;
    };
} // namespace RVX::Tools
