#include "Features/DefaultRenderPassBundle.h"

#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/DeferredLightingPass.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/Passes/WaterFeaturePass.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "Renderer/RenderPassRegistry.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace RVX
{
DefaultRenderPassBundle::DefaultRenderPassBundle()
    : m_registry(std::make_unique<RenderPassRegistry>())
{
}

DefaultRenderPassBundle::~DefaultRenderPassBundle()
{
    Clear();
}
DefaultRenderPassBundle::DefaultRenderPassBundle(
    DefaultRenderPassBundle&& other) noexcept
    : m_registry(std::move(other.m_registry))
    , m_deferredLighting(std::move(other.m_deferredLighting))
{
}
DefaultRenderPassBundle& DefaultRenderPassBundle::operator=(
    DefaultRenderPassBundle&& other) noexcept
{
    if (this != &other)
    {
        Clear();
        m_registry = std::move(other.m_registry);
        m_deferredLighting = std::move(other.m_deferredLighting);
    }
    return *this;
}

RenderPassRegistry& DefaultRenderPassBundle::GetRegistry() noexcept
{
    return *m_registry;
}

const RenderPassRegistry& DefaultRenderPassBundle::GetRegistry() const noexcept
{
    return *m_registry;
}

std::vector<std::unique_ptr<IRenderPass>>&
DefaultRenderPassBundle::GetPassStorage() noexcept
{
    return m_registry->GetPasses();
}

const std::vector<std::unique_ptr<IRenderPass>>&
DefaultRenderPassBundle::GetPassStorage() const noexcept
{
    return m_registry->GetPasses();
}

void DefaultRenderPassBundle::PrependOwnedPassesFrom(
    DefaultRenderPassBundle& source)
{
    if (&source == this || source.GetPassStorage().empty())
    {
        return;
    }

    std::vector<std::unique_ptr<IRenderPass>> merged;
    merged.reserve(
        source.GetPassStorage().size() + GetPassStorage().size());
    for (std::unique_ptr<IRenderPass>& pass : source.GetPassStorage())
    {
        merged.push_back(std::move(pass));
    }
    for (std::unique_ptr<IRenderPass>& pass : GetPassStorage())
    {
        merged.push_back(std::move(pass));
    }
    source.GetPassStorage().clear();

    std::stable_sort(
        merged.begin(),
        merged.end(),
        [](const std::unique_ptr<IRenderPass>& left,
           const std::unique_ptr<IRenderPass>& right)
        {
            return left->GetPriority() < right->GetPriority();
        });
    GetPassStorage() = std::move(merged);
}

void DefaultRenderPassBundle::AddPass(
    std::unique_ptr<IRenderPass> pass,
    IRHIDevice* device)
{
    m_registry->AddPass(std::move(pass), device);
}

bool DefaultRenderPassBundle::RemovePass(const char* name)
{
    if (name == nullptr)
    {
        return false;
    }

    IRenderPass* removed = nullptr;
    for (const std::unique_ptr<IRenderPass>& pass : GetPassStorage())
    {
        if (pass != nullptr && std::strcmp(pass->GetName(), name) == 0)
        {
            removed = pass.get();
            break;
        }
    }
    if (removed == nullptr)
    {
        return false;
    }

    if (removed == FindPass<RayTracedReflectionPass>())
    {
        if (RayTracedReflectionDenoisePass* const denoise =
                FindPass<RayTracedReflectionDenoisePass>())
        {
            denoise->SetReflectionSource(nullptr);
        }
        if (RayTracedReflectionCompositePass* const composite =
                FindPass<RayTracedReflectionCompositePass>())
        {
            composite->SetReflectionSource(nullptr);
        }
    }
    if (removed == FindPass<RayTracedReflectionDenoisePass>())
    {
        if (RayTracedReflectionCompositePass* const composite =
                FindPass<RayTracedReflectionCompositePass>())
        {
            composite->SetDenoisedReflectionSource(nullptr);
        }
    }
    return m_registry->RemovePass(name);
}

void DefaultRenderPassBundle::Clear() noexcept
{
    m_deferredLighting.reset();
    if (m_registry != nullptr)
    {
        m_registry->Clear();
    }
}

size_t DefaultRenderPassBundle::GetPassCount() const noexcept
{
    return (m_registry != nullptr ? m_registry->GetPassCount() : 0u) +
        static_cast<size_t>(m_deferredLighting != nullptr);
}

std::unique_ptr<DefaultRenderPassBundle> CreateDefaultRenderPassBundle(
    const DefaultRenderPassBundleDependencies& dependencies)
{
    auto bundle = std::make_unique<DefaultRenderPassBundle>();

    auto depthPrepass = std::make_unique<DepthPrepass>();
    depthPrepass->SetResources(dependencies.pipelineCache);
    depthPrepass->SetMaterialSystem(dependencies.materialSystem);
    depthPrepass->SetResourceRegistry(dependencies.resourceRegistry);
    bundle->AddPass(std::move(depthPrepass), dependencies.device);

    auto shadowPass = std::make_unique<ShadowPass>();
    shadowPass->SetResources(
        dependencies.pipelineCache, dependencies.materialSystem);
    shadowPass->SetResourceRegistry(dependencies.resourceRegistry);
    shadowPass->SetConfig(dependencies.shadowConfig);
    bundle->AddPass(std::move(shadowPass), dependencies.device);

    auto rayTracedShadowPass = std::make_unique<RayTracedShadowPass>();
    rayTracedShadowPass->SetResources(
        dependencies.pipelineCache, dependencies.resourceViewCache);
    rayTracedShadowPass->SetResourceRegistry(dependencies.resourceRegistry);
    rayTracedShadowPass->SetRayTracingScene(dependencies.rayTracingScene);
    rayTracedShadowPass->SetSubmissionTracker(dependencies.submissionTracker);
    rayTracedShadowPass->SetConfig(dependencies.shadowConfig);
    bundle->AddPass(std::move(rayTracedShadowPass), dependencies.device);

    auto cameraVelocityPass = std::make_unique<CameraVelocityPass>();
    cameraVelocityPass->SetResources(
        dependencies.pipelineCache, dependencies.resourceViewCache);
    bundle->AddPass(std::move(cameraVelocityPass), dependencies.device);

    auto objectVelocityPass = std::make_unique<ObjectVelocityPass>();
    objectVelocityPass->SetResources(
        dependencies.pipelineCache,
        dependencies.resourceViewCache,
        dependencies.materialSystem);
    objectVelocityPass->SetResourceRegistry(dependencies.resourceRegistry);
    bundle->AddPass(std::move(objectVelocityPass), dependencies.device);

    auto rayTracedReflectionPass =
        std::make_unique<RayTracedReflectionPass>();
    rayTracedReflectionPass->SetResources(
        dependencies.pipelineCache, dependencies.resourceViewCache);
    rayTracedReflectionPass->SetResourceRegistry(
        dependencies.resourceRegistry);
    rayTracedReflectionPass->SetRayTracingScene(dependencies.rayTracingScene);
    RayTracedReflectionPass* const reflectionSource =
        rayTracedReflectionPass.get();
    bundle->AddPass(std::move(rayTracedReflectionPass), dependencies.device);

    auto rayTracedReflectionDenoisePass =
        std::make_unique<RayTracedReflectionDenoisePass>();
    rayTracedReflectionDenoisePass->SetResources(
        dependencies.pipelineCache, dependencies.resourceViewCache);
    rayTracedReflectionDenoisePass->SetReflectionSource(reflectionSource);
    RayTracedReflectionDenoisePass* const denoisedReflectionSource =
        rayTracedReflectionDenoisePass.get();
    bundle->AddPass(
        std::move(rayTracedReflectionDenoisePass), dependencies.device);

    auto rayTracedReflectionCompositePass =
        std::make_unique<RayTracedReflectionCompositePass>();
    rayTracedReflectionCompositePass->SetResources(
        dependencies.pipelineCache, dependencies.resourceViewCache);
    rayTracedReflectionCompositePass->SetReflectionSource(reflectionSource);
    rayTracedReflectionCompositePass->SetDenoisedReflectionSource(
        denoisedReflectionSource);
    bundle->AddPass(
        std::move(rayTracedReflectionCompositePass), dependencies.device);

    auto opaquePass = std::make_unique<OpaquePass>();
    opaquePass->SetResources(
        dependencies.pipelineCache,
        dependencies.materialSystem,
        dependencies.lightManager,
        dependencies.clusteredLighting);
    opaquePass->SetResourceRegistry(dependencies.resourceRegistry);
    bundle->AddPass(std::move(opaquePass), dependencies.device);

    auto skyboxPass = std::make_unique<SkyboxPass>();
    skyboxPass->SetResources(dependencies.pipelineCache);
    skyboxPass->SetResourceRegistry(dependencies.resourceRegistry);
    bundle->AddPass(std::move(skyboxPass), dependencies.device);

    auto waterFeaturePass = std::make_unique<WaterFeaturePass>();
    if (dependencies.featureSnapshot != nullptr)
    {
        waterFeaturePass->SetSnapshot(&dependencies.featureSnapshot->water);
    }
    waterFeaturePass->SetShaderProgramProvider(
        dependencies.shaderProgramProvider);
    waterFeaturePass->SetReverseZ(dependencies.reverseZ);
    bundle->AddPass(std::move(waterFeaturePass), dependencies.device);

    auto transparentPass = std::make_unique<TransparentPass>();
    transparentPass->SetResources(
        dependencies.pipelineCache,
        dependencies.materialSystem,
        dependencies.lightManager,
        dependencies.clusteredLighting);
    transparentPass->SetResourceRegistry(dependencies.resourceRegistry);
    bundle->AddPass(std::move(transparentPass), dependencies.device);

    auto particleFeaturePass = std::make_unique<ParticleFeaturePass>();
    if (dependencies.featureSnapshot != nullptr)
    {
        particleFeaturePass->SetSnapshot(
            &dependencies.featureSnapshot->particles);
    }
    particleFeaturePass->SetShaderProgramProvider(
        dependencies.shaderProgramProvider);
    particleFeaturePass->SetResourceRegistry(dependencies.resourceRegistry);
    particleFeaturePass->SetReverseZ(dependencies.reverseZ);
    bundle->AddPass(std::move(particleFeaturePass), dependencies.device);

    bundle->m_deferredLighting =
        std::make_unique<DeferredLightingPass>();
    bundle->m_deferredLighting->SetPipelineCache(dependencies.pipelineCache);
    return bundle;
}
} // namespace RVX
