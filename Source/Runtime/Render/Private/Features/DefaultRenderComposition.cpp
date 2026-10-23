#include "Features/DefaultRenderComposition.h"
#include "Features/OpaqueSceneColorHistory.h"

#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ChromaticAberration.h"
#include "Render/PostProcess/ColorGrading.h"
#include "Render/PostProcess/FXAA.h"
#include "Render/PostProcess/FilmGrain.h"
#include "Render/PostProcess/PostProcessStack.h"
#include "Render/PostProcess/ScreenSpaceOutline.h"
#include "Render/PostProcess/TAA.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Render/PostProcess/Vignette.h"

#include <utility>

namespace RVX
{
DefaultRenderComposition::DefaultRenderComposition()
    : m_passBundle(std::make_unique<DefaultRenderPassBundle>())
    , m_opaqueSceneColorHistory(std::make_unique<OpaqueSceneColorHistory>())
{
}

DefaultRenderComposition::~DefaultRenderComposition()
{
    Shutdown();
}

DefaultRenderPassBundle&
DefaultRenderComposition::GetPassBundle() noexcept
{
    return *m_passBundle;
}

const DefaultRenderPassBundle&
DefaultRenderComposition::GetPassBundle() const noexcept
{
    return *m_passBundle;
}

void DefaultRenderComposition::InstallDefaultPassBundle(
    std::unique_ptr<DefaultRenderPassBundle> passBundle)
{
    if (passBundle == nullptr)
    {
        return;
    }
    if (m_defaultPassBundleInstalled)
    {
        // The incoming set has already received OnAdd during construction.
        // Rejecting a duplicate installation must still balance that lifecycle.
        passBundle->Clear();
        return;
    }

    ResetFeatures();
    if (m_passBundle != nullptr)
    {
        passBundle->PrependOwnedPassesFrom(*m_passBundle);
    }
    m_passBundle = std::move(passBundle);
    m_defaultPassBundleInstalled = true;
}

void DefaultRenderComposition::BeginFeatureRegistration()
{
    ResetFeatures();
    m_featureRegistry = std::make_unique<RenderFeatureRegistry>();
}

RenderFeatureRegistryCode DefaultRenderComposition::DeclareImplementedRecipe(
    RenderPipelineRecipe recipe)
{
    return m_featureRegistry != nullptr
        ? m_featureRegistry->DeclareImplementedRecipe(recipe)
        : RenderFeatureRegistryCode::RegistryNotSealed;
}

RenderFeatureRegistryCode DefaultRenderComposition::RegisterFeature(
    std::unique_ptr<IRenderFeature> feature)
{
    if (m_featureRegistry == nullptr || feature == nullptr)
    {
        return RenderFeatureRegistryCode::InvalidFeature;
    }

    IRenderFeature* const borrowed = feature.get();
    const RenderFeatureRegistryCode code =
        m_featureRegistry->Register(std::move(feature));
    if (code == RenderFeatureRegistryCode::Success)
    {
        m_featureIndex.push_back(borrowed);
    }
    return code;
}

RenderFeatureRegistryCode DefaultRenderComposition::SealFeatures()
{
    return m_featureRegistry != nullptr
        ? m_featureRegistry->Seal()
        : RenderFeatureRegistryCode::RegistryNotSealed;
}

void DefaultRenderComposition::ResetFeatures() noexcept
{
    m_featureIndex.clear();
    m_featureRegistry.reset();
}

RenderFeatureRegistry*
DefaultRenderComposition::GetFeatureRegistry() noexcept
{
    return m_featureRegistry.get();
}

const RenderFeatureRegistry*
DefaultRenderComposition::GetFeatureRegistry() const noexcept
{
    return m_featureRegistry.get();
}

size_t DefaultRenderComposition::GetFeatureCount() const noexcept
{
    return m_featureRegistry != nullptr
        ? m_featureRegistry->GetFeatureCount()
        : 0u;
}

bool DefaultRenderComposition::InitializePostProcess(
    IRHIDevice* device,
    PipelineCache* pipelineCache,
    ResourceViewCache* resourceViewCache)
{
    if (device == nullptr)
    {
        return false;
    }

    if (m_postProcessStack != nullptr)
    {
        m_postProcessStack->Shutdown();
    }
    m_postProcessStack = std::make_unique<PostProcessStack>();
    m_postProcessStack->Initialize(device);
    m_postProcessStack->SetExposureResources(pipelineCache);
    m_opaqueSceneColorHistory->Shutdown();
    m_opaqueSceneColorHistory->SetResources(pipelineCache, resourceViewCache);

    BloomPass* const bloom = m_postProcessStack->AddEffect<BloomPass>();
    ToneMappingPass* const toneMapping =
        m_postProcessStack->AddEffect<ToneMappingPass>();
    ColorGradingPass* const colorGrading =
        m_postProcessStack->AddEffect<ColorGradingPass>();
    ChromaticAberrationPass* const chromaticAberration =
        m_postProcessStack->AddEffect<ChromaticAberrationPass>();
    VignettePass* const vignette =
        m_postProcessStack->AddEffect<VignettePass>();
    FilmGrainPass* const filmGrain =
        m_postProcessStack->AddEffect<FilmGrainPass>();
    FXAAPass* const fxaa = m_postProcessStack->AddEffect<FXAAPass>();
    TAA* const taa = m_postProcessStack->AddEffect<TAA>();
    ScreenSpaceOutlinePass* const outline =
        m_postProcessStack->AddEffect<ScreenSpaceOutlinePass>();

    if (bloom != nullptr)
    {
        bloom->SetResources(pipelineCache, resourceViewCache);
    }
    if (toneMapping != nullptr)
    {
        toneMapping->SetResources(pipelineCache, resourceViewCache);
    }
    if (colorGrading != nullptr)
    {
        colorGrading->SetResources(pipelineCache, resourceViewCache);
    }
    if (chromaticAberration != nullptr)
    {
        chromaticAberration->SetResources(pipelineCache, resourceViewCache);
    }
    if (vignette != nullptr)
    {
        vignette->SetResources(pipelineCache, resourceViewCache);
    }
    if (filmGrain != nullptr)
    {
        filmGrain->SetResources(pipelineCache, resourceViewCache);
    }
    if (fxaa != nullptr)
    {
        fxaa->SetResources(pipelineCache, resourceViewCache);
    }
    if (taa != nullptr)
    {
        taa->SetResources(pipelineCache, resourceViewCache);
    }
    if (outline != nullptr)
    {
        outline->SetResources(pipelineCache, resourceViewCache);
    }
    return true;
}

PostProcessStack* DefaultRenderComposition::GetPostProcessStack() noexcept
{
    return m_postProcessStack.get();
}

const PostProcessStack*
DefaultRenderComposition::GetPostProcessStack() const noexcept
{
    return m_postProcessStack.get();
}

void DefaultRenderComposition::Shutdown() noexcept
{
    ResetFeatures();
    m_opaqueSceneColorHistory->Shutdown();
    if (m_postProcessStack != nullptr)
    {
        m_postProcessStack->Shutdown();
        m_postProcessStack.reset();
    }
    if (m_passBundle != nullptr)
    {
        m_passBundle->Clear();
    }
    m_defaultPassBundleInstalled = false;
}
} // namespace RVX
