/**
 * @file DefaultForwardComposition.cpp
 * @brief Composition-owned Forward+, Deferred, and Hybrid RT feature wiring.
 */

#include "Render/Renderer/SceneRenderer.h"

#include "Context/RenderContextInternal.h"
#include "Core/Log.h"
#include "Features/AmbientOcclusionFeatureAdapter.h"
#include "Features/DefaultRenderComposition.h"
#include "Features/DefaultRenderPassBundle.h"
#include "Features/DeferredRecipeFeatureAdapter.h"
#include "Features/LegacyForwardFeatureAdapter.h"
#include "Features/PostProcessPipelineFeatureAdapter.h"
#include "Features/ReflectionFeatureAdapter.h"
#include "Features/TranslucencyFeatureAdapter.h"
#include "Features/VolumetricLightingFeatureAdapter.h"
#include "Render/Passes/DeferredLightingPass.h"
#include "Render/Passes/OpaquePass.h"

#include <memory>
#include <utility>
#include <vector>

namespace RVX
{
bool SceneRenderer::SetupDefaultRenderFeatures()
{
    if (m_defaultRenderComposition == nullptr)
    {
        m_defaultRenderComposition =
            std::make_unique<DefaultRenderComposition>();
    }
    else if (m_defaultRenderComposition->HasInstalledDefaultPassBundle())
    {
        return EnsureDefaultRenderComposition();
    }

    DefaultRenderPassBundleDependencies dependencies{};
    dependencies.device = m_renderContext != nullptr
        ? m_renderContext->GetDevice()
        : nullptr;
    dependencies.pipelineCache = m_pipelineCache.get();
    dependencies.materialSystem = m_materialSystem.get();
    dependencies.lightManager = m_lightManager.get();
    dependencies.clusteredLighting = m_clusteredLighting.get();
    dependencies.resourceViewCache = m_resourceViewCache.get();
    dependencies.resourceRegistry = m_renderResourceRegistry;
    dependencies.rayTracingScene = m_rayTracingSceneManager.get();
    dependencies.submissionTracker = m_renderContext != nullptr
        ? RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext)
        : nullptr;
    dependencies.shaderProgramProvider = m_shaderProgramProvider.get();
    dependencies.featureSnapshot = m_featureSnapshot.get();
    dependencies.shadowConfig = m_shadowPassConfig;
    dependencies.reverseZ = m_pipelineCache != nullptr &&
        m_pipelineCache->GetConfig().reverseZ;

    std::unique_ptr<DefaultRenderPassBundle> passBundle =
        CreateDefaultRenderPassBundle(dependencies);
    if (passBundle == nullptr)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: default pass-bundle construction failed");
        return false;
    }
    m_defaultRenderComposition->InstallDefaultPassBundle(
        std::move(passBundle));

    if (!EnsureDefaultRenderComposition())
    {
        return false;
    }

    RVX_RENDER_DEBUG(
        "SceneRenderer: raster compositions sealed with {} feature(s)",
        m_defaultRenderComposition->GetFeatureCount());
    return true;
}

bool SceneRenderer::EnsureDefaultRenderComposition()
{
    if (m_defaultRenderComposition == nullptr)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: cannot compose raster recipes without a composition owner");
        return false;
    }

    DefaultRenderPassBundle& passBundle =
        m_defaultRenderComposition->GetPassBundle();
    const bool deferredDependenciesReady =
        passBundle.FindPass<OpaquePass>() != nullptr &&
        m_pipelineCache != nullptr &&
        passBundle.FindPass<DeferredLightingPass>() != nullptr;
    const bool fullCompositionReady =
        m_defaultRenderComposition->GetFeatureRegistry() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<LegacyForwardFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<AmbientOcclusionFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<ForwardLightingFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<ReflectionFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<VolumetricLightingFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<ForwardSpecialFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<TranslucencyFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<PostProcessPipelineFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<DeferredGeometryFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<DeferredLightingFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<DeferredForwardSpecialFeatureAdapter>() != nullptr;
    if (fullCompositionReady)
    {
        return true;
    }

    const bool forwardOnlyCompositionReady =
        m_defaultRenderComposition->GetFeatureRegistry() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<LegacyForwardFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<AmbientOcclusionFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<ForwardLightingFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<ReflectionFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<VolumetricLightingFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<ForwardSpecialFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<TranslucencyFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<PostProcessPipelineFeatureAdapter>() != nullptr &&
        m_defaultRenderComposition
                ->FindFeature<DeferredGeometryFeatureAdapter>() == nullptr &&
        m_defaultRenderComposition
                ->FindFeature<DeferredLightingFeatureAdapter>() == nullptr &&
        m_defaultRenderComposition
                ->FindFeature<DeferredForwardSpecialFeatureAdapter>() == nullptr;
    if (forwardOnlyCompositionReady)
    {
        if (!deferredDependenciesReady)
        {
            return true;
        }
        m_defaultRenderComposition->ResetFeatures();
    }

    if (m_defaultRenderComposition->GetFeatureRegistry() != nullptr)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: raster composition ownership is inconsistent");
        return false;
    }

    m_defaultRenderComposition->BeginFeatureRegistration();
    std::vector<RenderPipelineRecipe> recipes = {
        RenderPipelineRecipe::ForwardStandard};
    if (deferredDependenciesReady)
    {
        recipes.push_back(RenderPipelineRecipe::DeferredQuality);
        recipes.push_back(RenderPipelineRecipe::HybridRayTracing);
    }
    for (const RenderPipelineRecipe recipe : recipes)
    {
        const RenderFeatureRegistryCode recipeCode =
            m_defaultRenderComposition->DeclareImplementedRecipe(recipe);
        if (recipeCode != RenderFeatureRegistryCode::Success)
        {
            RVX_RENDER_ERROR(
                "SceneRenderer: failed to declare '{}' recipe: {}",
                GetRenderPipelineRecipeName(recipe),
                GetRenderFeatureRegistryCodeName(recipeCode));
            m_defaultRenderComposition->ResetFeatures();
            return false;
        }
    }

    const auto registerFeature = [this](
        std::unique_ptr<IRenderFeature> feature,
        const char* name)
    {
        const RenderFeatureRegistryCode code =
            m_defaultRenderComposition->RegisterFeature(std::move(feature));
        if (code != RenderFeatureRegistryCode::Success)
        {
            RVX_RENDER_ERROR(
                "SceneRenderer: failed to register '{}' feature: {}",
                name,
                GetRenderFeatureRegistryCodeName(code));
            return false;
        }
        return true;
    };

    if (!registerFeature(
            std::make_unique<LegacyForwardFeatureAdapter>(&passBundle),
            "ForwardStandard") ||
        !registerFeature(
            std::make_unique<AmbientOcclusionFeatureAdapter>(
                m_pipelineCache.get()),
            "AmbientOcclusion") ||
        !registerFeature(
            std::make_unique<ForwardLightingFeatureAdapter>(&passBundle),
            "ForwardLighting") ||
        !registerFeature(
            std::make_unique<ReflectionFeatureAdapter>(
                &passBundle, m_pipelineCache.get()),
            "ReflectionResolve") ||
        !registerFeature(
            std::make_unique<VolumetricLightingFeatureAdapter>(
                m_pipelineCache.get()),
            "VolumetricLighting") ||
        !registerFeature(
            std::make_unique<ForwardSpecialFeatureAdapter>(
                &passBundle, m_pipelineCache.get()),
            "ForwardSpecial") ||
        !registerFeature(
            std::make_unique<TranslucencyFeatureAdapter>(
                &passBundle, m_pipelineCache.get()),
            "Translucency") ||
        !registerFeature(
            std::make_unique<PostProcessPipelineFeatureAdapter>(
                GetPostProcessStack()),
            "PostProcessPipeline"))
    {
        m_defaultRenderComposition->ResetFeatures();
        return false;
    }

    if (deferredDependenciesReady)
    {
        if (!registerFeature(
                std::make_unique<DeferredGeometryFeatureAdapter>(
                    &passBundle, m_pipelineCache.get()),
                "DeferredGeometry") ||
            !registerFeature(
                std::make_unique<DeferredLightingFeatureAdapter>(
                    &passBundle, m_pipelineCache.get()),
                "DeferredLighting") ||
            !registerFeature(
                std::make_unique<DeferredForwardSpecialFeatureAdapter>(
                    &passBundle, m_pipelineCache.get()),
                "DeferredForwardSpecial"))
        {
            m_defaultRenderComposition->ResetFeatures();
            return false;
        }
    }

    const RenderFeatureRegistryCode sealCode =
        m_defaultRenderComposition->SealFeatures();
    if (sealCode != RenderFeatureRegistryCode::Success)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: failed to seal raster compositions: {}",
            GetRenderFeatureRegistryCodeName(sealCode));
        m_defaultRenderComposition->ResetFeatures();
        return false;
    }
    return true;
}
} // namespace RVX
