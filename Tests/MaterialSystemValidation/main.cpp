#include "Render/Material/MaterialClassification.h"
#include "Scene/Material.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{
    TEST(MaterialSystemValidation, ClassifiesMaterialAlphaModes)
    {
        auto opaque = std::make_shared<RVX::Material>();
        opaque->SetAlphaMode(RVX::Material::AlphaMode::Opaque);
        EXPECT_EQ(RVX::MaterialRenderMode::Opaque,
                  RVX::ClassifyMaterialRenderMode(opaque.get()));

        auto masked = std::make_shared<RVX::Material>();
        masked->SetAlphaMode(RVX::Material::AlphaMode::Mask);
        EXPECT_EQ(RVX::MaterialRenderMode::Masked,
                  RVX::ClassifyMaterialRenderMode(masked.get()));

        auto transparent = std::make_shared<RVX::Material>();
        transparent->SetAlphaMode(RVX::Material::AlphaMode::Blend);
        EXPECT_EQ(RVX::MaterialRenderMode::Transparent,
                  RVX::ClassifyMaterialRenderMode(transparent.get()));

        EXPECT_EQ(RVX::MaterialPipelineVariant::Opaque,
                  RVX::GetPipelineVariantForRenderMode(RVX::MaterialRenderMode::Opaque));
        EXPECT_EQ(RVX::MaterialPipelineVariant::Masked,
                  RVX::GetPipelineVariantForRenderMode(RVX::MaterialRenderMode::Masked));
        EXPECT_EQ(RVX::MaterialPipelineVariant::Transparent,
                  RVX::GetPipelineVariantForRenderMode(RVX::MaterialRenderMode::Transparent));
    }
} // namespace
