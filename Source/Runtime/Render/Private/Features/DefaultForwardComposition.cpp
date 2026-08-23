/**
 * @file DefaultForwardComposition.cpp
 * @brief Compatibility-preserving ForwardStandard renderer composition.
 */

#include "Render/Renderer/SceneRenderer.h"

#include "Context/RenderContextInternal.h"
#include "Core/Log.h"
#include "Features/LegacyForwardFeatureAdapter.h"
#include "Render/Features/RenderFeatureRegistry.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/DepthPrepass.h"
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

    if (!EnsureLegacyForwardComposition())
    {
        return false;
    }

    RVX_RENDER_DEBUG(
        "SceneRenderer: ForwardStandard composition sealed with {} feature(s)",
        m_renderFeatureRegistry->GetFeatureCount());
    return true;
}

bool SceneRenderer::EnsureLegacyForwardComposition()
{
    if (m_renderFeatureRegistry != nullptr &&
        m_legacyForwardFeature != nullptr)
    {
        return true;
    }
    if (m_renderFeatureRegistry != nullptr ||
        m_legacyForwardFeature != nullptr)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: ForwardStandard composition ownership is inconsistent");
        return false;
    }
    if (m_passRegistry == nullptr)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: cannot compose ForwardStandard without a pass registry");
        return false;
    }

    auto registry = std::make_unique<RenderFeatureRegistry>();
    const RenderFeatureRegistryCode recipeCode =
        registry->DeclareImplementedRecipe(
            RenderPipelineRecipe::ForwardStandard);
    if (recipeCode != RenderFeatureRegistryCode::Success)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: failed to declare ForwardStandard recipe: {}",
            GetRenderFeatureRegistryCodeName(recipeCode));
        return false;
    }

    auto legacyForward = std::make_unique<LegacyForwardFeatureAdapter>(
        m_passRegistry.get(), m_opaquePass);
    LegacyForwardFeatureAdapter* const legacyForwardFeature =
        legacyForward.get();
    const RenderFeatureRegistryCode registrationCode =
        registry->Register(std::move(legacyForward));
    if (registrationCode != RenderFeatureRegistryCode::Success)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: failed to register ForwardStandard feature: {}",
            GetRenderFeatureRegistryCodeName(registrationCode));
        return false;
    }

    const RenderFeatureRegistryCode sealCode = registry->Seal();
    if (sealCode != RenderFeatureRegistryCode::Success)
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: failed to seal ForwardStandard composition: {}",
            GetRenderFeatureRegistryCodeName(sealCode));
        return false;
    }

    m_legacyForwardFeature = legacyForwardFeature;
    m_renderFeatureRegistry = std::move(registry);
    return true;
}
} // namespace RVX
