/**
 * @file DefaultForwardComposition.cpp
 * @brief Compatibility-preserving ForwardStandard renderer composition.
 */

#include "Render/Renderer/SceneRenderer.h"

#include "Context/RenderContextInternal.h"
#include "Core/Log.h"
#include "Features/DeferredRecipeFeatureAdapter.h"
#include "Features/LegacyForwardFeatureAdapter.h"
#include "Render/Features/RenderFeatureRegistry.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/DeferredLightingPass.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Renderer/RenderPassRegistry.h"

#include <memory>
#include <utility>
#include <vector>

namespace RVX
{
bool SceneRenderer::SetupDefaultRenderFeatures()
{
    auto depthPrepass = std::make_unique<DepthPrepass>();
    depthPrepass->SetResources(m_pipelineCache.get());
    depthPrepass->SetMaterialSystem(m_materialSystem.get());
    depthPrepass->SetResourceRegistry(m_renderResourceRegistry);
    m_depthPrepass = depthPrepass.get();
    AddPass(std::move(depthPrepass));

    auto shadowPass = std::make_unique<ShadowPass>();
    shadowPass->SetResources(
        m_pipelineCache.get(), m_materialSystem.get());
    shadowPass->SetResourceRegistry(m_renderResourceRegistry);
    shadowPass->SetConfig(m_shadowPassConfig);
    m_shadowPass = shadowPass.get();
    AddPass(std::move(shadowPass));

    auto rayTracedShadowPass = std::make_unique<RayTracedShadowPass>();
    rayTracedShadowPass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedShadowPass->SetResourceRegistry(m_renderResourceRegistry);
    rayTracedShadowPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    rayTracedShadowPass->SetSubmissionTracker(
        m_renderContext
            ? RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext)
            : nullptr);
    rayTracedShadowPass->SetConfig(m_shadowPassConfig);
    m_rayTracedShadowPass = rayTracedShadowPass.get();
    AddPass(std::move(rayTracedShadowPass));

    auto cameraVelocityPass = std::make_unique<CameraVelocityPass>();
    cameraVelocityPass->SetResources(
        m_pipelineCache.get(), m_resourceViewCache.get());
    m_cameraVelocityPass = cameraVelocityPass.get();
    AddPass(std::move(cameraVelocityPass));

    auto objectVelocityPass = std::make_unique<ObjectVelocityPass>();
    objectVelocityPass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get(),
        m_materialSystem.get());
    objectVelocityPass->SetResourceRegistry(m_renderResourceRegistry);
    m_objectVelocityPass = objectVelocityPass.get();
    AddPass(std::move(objectVelocityPass));

    auto rayTracedReflectionPass =
        std::make_unique<RayTracedReflectionPass>();
    rayTracedReflectionPass->SetResources(
        m_pipelineCache.get(), m_resourceViewCache.get());
    rayTracedReflectionPass->SetResourceRegistry(m_renderResourceRegistry);
    rayTracedReflectionPass->SetRayTracingScene(
        m_rayTracingSceneManager.get());
    m_rayTracedReflectionPass = rayTracedReflectionPass.get();
    AddPass(std::move(rayTracedReflectionPass));

    auto rayTracedReflectionDenoisePass =
        std::make_unique<RayTracedReflectionDenoisePass>();
    rayTracedReflectionDenoisePass->SetResources(
        m_pipelineCache.get(), m_resourceViewCache.get());
    rayTracedReflectionDenoisePass->SetReflectionSource(
        m_rayTracedReflectionPass);
    m_rayTracedReflectionDenoisePass =
        rayTracedReflectionDenoisePass.get();
    AddPass(std::move(rayTracedReflectionDenoisePass));

    auto rayTracedReflectionCompositePass =
        std::make_unique<RayTracedReflectionCompositePass>();
    rayTracedReflectionCompositePass->SetResources(
        m_pipelineCache.get(), m_resourceViewCache.get());
    rayTracedReflectionCompositePass->SetReflectionSource(
        m_rayTracedReflectionPass);
    rayTracedReflectionCompositePass->SetDenoisedReflectionSource(
        m_rayTracedReflectionDenoisePass);
    m_rayTracedReflectionCompositePass =
        rayTracedReflectionCompositePass.get();
    AddPass(std::move(rayTracedReflectionCompositePass));

    auto opaquePass = std::make_unique<OpaquePass>();
    opaquePass->SetResources(m_pipelineCache.get(),
                             m_materialSystem.get(),
                             m_lightManager.get(),
                             m_clusteredLighting.get());
    opaquePass->SetResourceRegistry(m_renderResourceRegistry);
    m_opaquePass = opaquePass.get();
    AddPass(std::move(opaquePass));

    auto skyboxPass = std::make_unique<SkyboxPass>();
    skyboxPass->SetResources(m_pipelineCache.get());
    skyboxPass->SetResourceRegistry(m_renderResourceRegistry);
    m_skyboxPass = skyboxPass.get();
    AddPass(std::move(skyboxPass));

    auto transparentPass = std::make_unique<TransparentPass>();
    transparentPass->SetResources(m_pipelineCache.get(),
                                  m_materialSystem.get(),
                                  m_lightManager.get(),
                                  m_clusteredLighting.get());
    transparentPass->SetResourceRegistry(m_renderResourceRegistry);
    m_transparentPass = transparentPass.get();
    AddPass(std::move(transparentPass));

    auto particleFeaturePass = std::make_unique<ParticleFeaturePass>();
    particleFeaturePass->SetSnapshot(&m_featureSnapshot->particles);
    m_particleFeaturePass = particleFeaturePass.get();
    AddPass(std::move(particleFeaturePass));

    m_deferredLightingPass = std::make_unique<DeferredLightingPass>();
    m_deferredLightingPass->SetPipelineCache(m_pipelineCache.get());

    if (!EnsureDefaultRenderComposition())
    {
        return false;
    }

    RVX_RENDER_DEBUG(
        "SceneRenderer: raster compositions sealed with {} feature(s)",
        m_renderFeatureRegistry->GetFeatureCount());
    return true;
}

bool SceneRenderer::EnsureDefaultRenderComposition()
{
    const bool deferredDependenciesReady = m_opaquePass != nullptr &&
        m_pipelineCache != nullptr && m_deferredLightingPass != nullptr;
    const bool fullCompositionReady = m_legacyForwardFeature != nullptr &&
        m_deferredGeometryFeature != nullptr &&
        m_deferredLightingFeature != nullptr &&
        m_deferredForwardSpecialFeature != nullptr;
    if (m_renderFeatureRegistry != nullptr && fullCompositionReady)
    {
        return true;
    }

    // A focused/tooling renderer may record ForwardStandard before the full
    // runtime is initialized. Keep that composition narrow and upgrade it
    // atomically once the Deferred dependencies become available.
    const bool forwardOnlyCompositionReady =
        m_renderFeatureRegistry != nullptr &&
        m_legacyForwardFeature != nullptr &&
        m_deferredGeometryFeature == nullptr &&
        m_deferredLightingFeature == nullptr &&
        m_deferredForwardSpecialFeature == nullptr;
    if (forwardOnlyCompositionReady)
    {
        if (!deferredDependenciesReady)
        {
            return true;
        }
        m_renderFeatureRegistry.reset();
        m_legacyForwardFeature = nullptr;
    }

    if (m_renderFeatureRegistry != nullptr || m_legacyForwardFeature != nullptr ||
        m_deferredGeometryFeature != nullptr ||
        m_deferredLightingFeature != nullptr ||
        m_deferredForwardSpecialFeature != nullptr)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: raster composition ownership is inconsistent");
        return false;
    }
    if (m_passRegistry == nullptr)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: cannot compose raster recipes without a pass registry");
        return false;
    }

    auto registry = std::make_unique<RenderFeatureRegistry>();
    std::vector<RenderPipelineRecipe> recipes = {
        RenderPipelineRecipe::ForwardStandard};
    if (deferredDependenciesReady)
    {
        recipes.push_back(RenderPipelineRecipe::DeferredQuality);
    }
    for (const RenderPipelineRecipe recipe : recipes)
    {
        const RenderFeatureRegistryCode recipeCode =
            registry->DeclareImplementedRecipe(recipe);
        if (recipeCode != RenderFeatureRegistryCode::Success)
        {
            RVX_RENDER_ERROR(
                "SceneRenderer: failed to declare '{}' recipe: {}",
                GetRenderPipelineRecipeName(recipe),
                GetRenderFeatureRegistryCodeName(recipeCode));
            return false;
        }
    }

    auto legacyForward = std::make_unique<LegacyForwardFeatureAdapter>(
        m_passRegistry.get(), m_opaquePass);
    LegacyForwardFeatureAdapter* const legacyForwardFeature =
        legacyForward.get();
    const auto registerFeature = [&registry](
        std::unique_ptr<IRenderFeature> feature,
        const char* name)
    {
        const RenderFeatureRegistryCode code =
            registry->Register(std::move(feature));
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
    if (!registerFeature(std::move(legacyForward), "ForwardStandard"))
    {
        return false;
    }

    DeferredGeometryFeatureAdapter* deferredGeometryFeature = nullptr;
    DeferredLightingFeatureAdapter* deferredLightingFeature = nullptr;
    DeferredForwardSpecialFeatureAdapter* deferredForwardSpecialFeature = nullptr;
    if (deferredDependenciesReady)
    {
        auto deferredGeometry =
            std::make_unique<DeferredGeometryFeatureAdapter>(
                m_passRegistry.get(), m_opaquePass, m_pipelineCache.get());
        deferredGeometryFeature = deferredGeometry.get();
        auto deferredLighting =
            std::make_unique<DeferredLightingFeatureAdapter>(
                m_deferredLightingPass.get(), m_pipelineCache.get());
        deferredLightingFeature = deferredLighting.get();
        auto deferredForwardSpecial =
            std::make_unique<DeferredForwardSpecialFeatureAdapter>(
                m_passRegistry.get(), m_opaquePass);
        deferredForwardSpecialFeature = deferredForwardSpecial.get();
        if (!registerFeature(std::move(deferredGeometry), "DeferredGeometry") ||
            !registerFeature(std::move(deferredLighting), "DeferredLighting") ||
            !registerFeature(std::move(deferredForwardSpecial),
                             "DeferredForwardSpecial"))
        {
            return false;
        }
    }

    const RenderFeatureRegistryCode sealCode = registry->Seal();
    if (sealCode != RenderFeatureRegistryCode::Success)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: failed to seal raster compositions: {}",
            GetRenderFeatureRegistryCodeName(sealCode));
        return false;
    }

    m_legacyForwardFeature = legacyForwardFeature;
    m_deferredGeometryFeature = deferredGeometryFeature;
    m_deferredLightingFeature = deferredLightingFeature;
    m_deferredForwardSpecialFeature = deferredForwardSpecialFeature;
    m_renderFeatureRegistry = std::move(registry);
    return true;
}
} // namespace RVX
