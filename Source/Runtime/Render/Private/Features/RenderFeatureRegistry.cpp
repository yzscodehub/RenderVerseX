#include "Render/Features/RenderFeatureRegistry.h"

#include "RenderContracts/RenderFrameValidation.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] constexpr size_t ToIndex(
        RenderBlackboardResourceId resource) noexcept
    {
        return static_cast<size_t>(resource);
    }

    [[nodiscard]] constexpr bool IsDeclaredResource(
        RenderBlackboardResourceId resource) noexcept
    {
        return resource >= RenderBlackboardResourceId::SceneTextures &&
               resource < RenderBlackboardResourceId::Count;
    }

    [[nodiscard]] constexpr bool IsDeclaredPhase(
        RenderFeaturePhase phase) noexcept
    {
        return phase >= RenderFeaturePhase::FrameSetup &&
               phase <= RenderFeaturePhase::Present;
    }

    [[nodiscard]] constexpr uint32 GetResourceSchemaVersion(
        RenderBlackboardResourceId resource) noexcept
    {
        switch (resource)
        {
            case RenderBlackboardResourceId::SceneTextures:
                return SceneTextures::SchemaVersion;
            case RenderBlackboardResourceId::VisibilityOutputs:
                return VisibilityOutputs::SchemaVersion;
            case RenderBlackboardResourceId::ShadowOutputs:
                return ShadowOutputs::SchemaVersion;
            case RenderBlackboardResourceId::LocalShadowOutputs:
                return LocalShadowOutputs::SchemaVersion;
            case RenderBlackboardResourceId::LightingOutputs:
                return LightingOutputs::SchemaVersion;
            case RenderBlackboardResourceId::ReflectionOutputs:
                return ReflectionOutputs::SchemaVersion;
            case RenderBlackboardResourceId::VolumetricLightingOutputs:
                return VolumetricLightingOutputs::SchemaVersion;
            case RenderBlackboardResourceId::PreTranslucencyOutputs:
                return PreTranslucencyOutputs::SchemaVersion;
            case RenderBlackboardResourceId::TranslucencyOutputs:
                return TranslucencyOutputs::SchemaVersion;
            case RenderBlackboardResourceId::PostProcessOutputs:
                return PostProcessOutputs::SchemaVersion;
            case RenderBlackboardResourceId::AmbientOcclusionOutputs:
                return AmbientOcclusionOutputs::SchemaVersion;
            default:
                return 0U;
        }
    }

    [[nodiscard]] bool ContainsResource(
        const std::vector<RenderFeatureResourceContract>& resources,
        const RenderFeatureResourceContract& target) noexcept
    {
        return std::find(resources.begin(), resources.end(), target) !=
               resources.end();
    }

    [[nodiscard]] bool HasDuplicateResources(
        const std::vector<RenderFeatureResourceContract>& resources) noexcept
    {
        for (size_t left = 0; left < resources.size(); ++left)
        {
            for (size_t right = left + 1; right < resources.size(); ++right)
            {
                if (resources[left].resource == resources[right].resource)
                {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] bool HasEquivalentResourceContracts(
        const std::vector<RenderFeatureResourceContract>& left,
        const std::vector<RenderFeatureResourceContract>& right) noexcept
    {
        return left.size() == right.size() &&
               std::all_of(
                   left.begin(), left.end(), [&right](const auto& resource)
                   {
                       return ContainsResource(right, resource);
                   });
    }

    [[nodiscard]] bool IsValidResourceList(
        const std::vector<RenderFeatureResourceContract>& resources) noexcept
    {
        return std::all_of(
            resources.begin(),
            resources.end(),
            [](const RenderFeatureResourceContract& resource)
            {
                return IsDeclaredResource(resource.resource) &&
                       resource.schemaVersion ==
                           GetResourceSchemaVersion(resource.resource);
            });
    }

    [[nodiscard]] bool HasCapabilities(
        RenderFeatureCapabilityMask available,
        RenderFeatureCapabilityMask required) noexcept
    {
        return (available & required) == required;
    }

    [[nodiscard]] constexpr RenderPipelineRecipeMask GetRecipeMask(
        const RenderFeatureDescriptor& descriptor) noexcept
    {
        return descriptor.requiredRecipes | descriptor.optionalRecipes;
    }

    [[nodiscard]] constexpr bool ActivationDomainsOverlap(
        const RenderFeatureDescriptor& left,
        const RenderFeatureDescriptor& right,
        RenderPipelineRecipeMask implementedRecipes) noexcept
    {
        return (GetRecipeMask(left) & GetRecipeMask(right) &
                implementedRecipes) != 0 &&
               (left.qualityTiers & right.qualityTiers) != 0;
    }

    [[nodiscard]] constexpr bool FallbackDomainsOverlap(
        const RenderFeatureDescriptor& left,
        const RenderFeatureDescriptor& right) noexcept
    {
        return (left.optionalRecipes & right.optionalRecipes) != 0 &&
               (left.qualityTiers & right.qualityTiers) != 0;
    }

    [[nodiscard]] constexpr bool IsActiveForProfile(
        const RenderFeatureDescriptor& descriptor,
        RenderPipelineRecipe recipe,
        RenderQualityTier quality) noexcept
    {
        return (GetRecipeMask(descriptor) & ToMask(recipe)) != 0 &&
               (descriptor.qualityTiers & ToMask(quality)) != 0;
    }

    [[nodiscard]] bool IsValidFeatureViewFamily(
        const RenderViewFamilySnapshot& family) noexcept
    {
        if (!IsValidRenderFrameStamp(family.frame) || !family.identity.IsValid() ||
            !IsValidEffectFrameTime(family.effectTime) || !family.extent.IsValid() ||
            family.familyId == 0 || family.views.empty() ||
            family.primaryViewIndex >= family.views.size() ||
            family.frame.sequence != family.identity.frameSequence)
        {
            return false;
        }

        std::vector<RenderViewIdentity> identities;
        identities.reserve(family.views.size());
        for (const RenderViewSnapshot& view : family.views)
        {
            const uint64 viewportRight = static_cast<uint64>(view.viewportX) +
                                         static_cast<uint64>(view.viewportWidth);
            const uint64 viewportBottom = static_cast<uint64>(view.viewportY) +
                                          static_cast<uint64>(view.viewportHeight);
            if (!IsValidRenderViewSnapshot(view) ||
                viewportRight > family.extent.width ||
                viewportBottom > family.extent.height ||
                view.absoluteTime != family.frame.absoluteTimeSeconds ||
                view.deltaTime != family.frame.deltaTimeSeconds ||
                std::find(identities.begin(), identities.end(), view.identity) !=
                    identities.end())
            {
                return false;
            }
            identities.push_back(view.identity);
        }
        return true;
    }

    struct RenderFeatureSnapshotBindingValidation
    {
        bool valid = false;
        const char* diagnostic = "Render feature snapshot input is invalid";
    };

    template<typename SnapshotType>
    [[nodiscard]] RenderFeatureSnapshotBindingValidation
    ValidateSnapshotChannel(const SnapshotType& snapshot,
                            const RenderFeatureDescriptor& descriptor,
                            const RenderFrameIdentity& frameIdentity) noexcept
    {
        const auto metadata = snapshot.GetMetadata();
        using Status = std::decay_t<decltype(metadata.status)>;
        if (metadata.schemaVersion != descriptor.snapshotChannelSchemaVersion)
        {
            return {false, "Render feature snapshot channel schema mismatches its descriptor"};
        }
        if (!metadata.complete || metadata.status != Status::Complete)
        {
            return {false, "Render feature snapshot channel is incomplete"};
        }
        if (metadata.sequence != frameIdentity.frameSequence)
        {
            return {false, "Render feature snapshot channel generation is stale"};
        }

        for (size_t itemIndex = 0; itemIndex < snapshot.items.size(); ++itemIndex)
        {
            const auto& item = snapshot.items[itemIndex];
            const RenderFeatureChannelValidationCode headerCode =
                item.header.Validate(descriptor.snapshotChannel,
                                     descriptor.snapshotChannelSchemaVersion);
            if (headerCode == RenderFeatureChannelValidationCode::SchemaMismatch)
            {
                return {false, "Render feature snapshot item schema mismatches its descriptor"};
            }
            if (headerCode != RenderFeatureChannelValidationCode::Valid)
            {
                return {false, "Render feature snapshot item channel identity is invalid"};
            }
            for (size_t previousIndex = 0; previousIndex < itemIndex;
                 ++previousIndex)
            {
                if (snapshot.items[previousIndex].header.source ==
                    item.header.source)
                {
                    return {false,
                            "Render feature snapshot channel contains duplicate producer source"};
                }
            }
            // The aggregate snapshot sequence is the Render-frame generation.
            // Individual item headers deliberately retain their producer-owned
            // capture/configuration revisions.  Equating those independent
            // clocks incorrectly rejects a stable source published into a
            // newer Render frame.
        }
        return {true, {}};
    }

    [[nodiscard]] RenderFeatureSnapshotBindingValidation
    ValidateSnapshotBinding(const RenderFeatureDescriptor& descriptor,
                            const RenderFeatureContext& context) noexcept
    {
        if (descriptor.snapshotChannel == RenderFeatureChannelId::Invalid)
        {
            return {true, {}};
        }

        const RenderFeatureSnapshot* const snapshot =
            context.snapshotInputs.GetSnapshot();
        if (snapshot == nullptr)
        {
            return {false, "Required render feature snapshot input is missing"};
        }
        if (snapshot->metadata.schemaVersion !=
            RVX_RENDER_FEATURE_SNAPSHOT_SCHEMA_VERSION)
        {
            return {false, "Render feature snapshot schema is invalid"};
        }
        if (!snapshot->metadata.complete ||
            snapshot->metadata.status != RenderFeatureSnapshotStatus::Complete)
        {
            return {false, "Render feature snapshot is incomplete"};
        }
        if (snapshot->metadata.sequence != context.frameIdentity.frameSequence)
        {
            return {false, "Render feature snapshot generation is stale"};
        }

        const RenderFeatureChannelRegistry* const channelRegistry =
            context.snapshotInputs.GetChannelRegistry();
        if (channelRegistry == nullptr)
        {
            return {false, "Render feature channel registry is missing"};
        }
        if (!channelRegistry->IsSealed())
        {
            return {false, "Render feature channel registry is not sealed"};
        }
        if (channelRegistry->ValidateRoute(
                descriptor.snapshotChannel,
                descriptor.snapshotChannelSchemaVersion,
                context.snapshotInputs.GetProducerEndpoint(),
                context.snapshotInputs.GetConsumerEndpoint()) !=
            RenderFeatureChannelRegistryCode::Success)
        {
            return {false, "Render feature snapshot channel route mismatches composition"};
        }

        switch (descriptor.snapshotChannel)
        {
            case RenderFeatureChannelId::Particle:
                return ValidateSnapshotChannel(
                    snapshot->particles, descriptor, context.frameIdentity);
            case RenderFeatureChannelId::Water:
                return ValidateSnapshotChannel(
                    snapshot->water, descriptor, context.frameIdentity);
            case RenderFeatureChannelId::Terrain:
                return ValidateSnapshotChannel(
                    snapshot->terrain, descriptor, context.frameIdentity);
            case RenderFeatureChannelId::Invalid:
            default:
                return {false, "Render feature snapshot channel is invalid"};
        }
    }

} // namespace

const char* GetRenderFeatureRegistryCodeName(
    RenderFeatureRegistryCode code) noexcept
{
    switch (code)
    {
        case RenderFeatureRegistryCode::Success: return "Success";
        case RenderFeatureRegistryCode::RegistrySealed: return "RegistrySealed";
        case RenderFeatureRegistryCode::RegistryNotSealed: return "RegistryNotSealed";
        case RenderFeatureRegistryCode::InvalidFeature: return "InvalidFeature";
        case RenderFeatureRegistryCode::DuplicateFeatureId: return "DuplicateFeatureId";
        case RenderFeatureRegistryCode::DuplicateFeatureName: return "DuplicateFeatureName";
        case RenderFeatureRegistryCode::InvalidRecipeMask: return "InvalidRecipeMask";
        case RenderFeatureRegistryCode::InvalidQualityMask: return "InvalidQualityMask";
        case RenderFeatureRegistryCode::InvalidResourceContract: return "InvalidResourceContract";
        case RenderFeatureRegistryCode::InvalidSnapshotChannel: return "InvalidSnapshotChannel";
        case RenderFeatureRegistryCode::DuplicateResourceContract: return "DuplicateResourceContract";
        case RenderFeatureRegistryCode::DuplicateProducer: return "DuplicateProducer";
        case RenderFeatureRegistryCode::MissingProducer: return "MissingProducer";
        case RenderFeatureRegistryCode::ResourceSchemaMismatch: return "ResourceSchemaMismatch";
        case RenderFeatureRegistryCode::DependencyCycle: return "DependencyCycle";
        case RenderFeatureRegistryCode::IllegalPhaseEdge: return "IllegalPhaseEdge";
        case RenderFeatureRegistryCode::InvalidFallback: return "InvalidFallback";
        case RenderFeatureRegistryCode::MissingFallback: return "MissingFallback";
        case RenderFeatureRegistryCode::RecipeNotImplemented: return "RecipeNotImplemented";
        case RenderFeatureRegistryCode::InvalidFrameContext: return "InvalidFrameContext";
        case RenderFeatureRegistryCode::RequiredFeatureUnavailable: return "RequiredFeatureUnavailable";
        case RenderFeatureRegistryCode::OptionalFallbackUnavailable: return "OptionalFallbackUnavailable";
        case RenderFeatureRegistryCode::GraphCompositionFailed: return "GraphCompositionFailed";
        default: return "Invalid";
    }
}

RenderFeatureRegistryCode RenderFeatureRegistry::Register(
    std::unique_ptr<IRenderFeature> feature)
{
    if (m_sealed)
    {
        return RenderFeatureRegistryCode::RegistrySealed;
    }
    if (!feature)
    {
        return RenderFeatureRegistryCode::InvalidFeature;
    }

    RenderFeatureDescriptor descriptor = feature->Describe();
    if (!descriptor.featureId.IsValid() || descriptor.schemaVersion == 0 ||
        descriptor.name.empty() || !IsDeclaredPhase(descriptor.phase))
    {
        return RenderFeatureRegistryCode::InvalidFeature;
    }
    if (FindEntry(descriptor.featureId) != nullptr)
    {
        return RenderFeatureRegistryCode::DuplicateFeatureId;
    }
    if (std::any_of(
            m_features.begin(),
            m_features.end(),
            [&descriptor](const Entry& entry)
            {
                return entry.descriptor.name == descriptor.name;
            }))
    {
        return RenderFeatureRegistryCode::DuplicateFeatureName;
    }
    if ((descriptor.requiredRecipes | descriptor.optionalRecipes) == 0 ||
        (descriptor.requiredRecipes & descriptor.optionalRecipes) != 0 ||
        ((descriptor.requiredRecipes | descriptor.optionalRecipes) &
         ~RVX_ALL_RENDER_RECIPES) != 0)
    {
        return RenderFeatureRegistryCode::InvalidRecipeMask;
    }
    if (descriptor.qualityTiers == 0 ||
        (descriptor.qualityTiers & ~RVX_ALL_RENDER_QUALITY_TIERS) != 0)
    {
        return RenderFeatureRegistryCode::InvalidQualityMask;
    }
    if (!IsValidResourceList(descriptor.consumedResources) ||
        !IsValidResourceList(descriptor.producedResources))
    {
        return RenderFeatureRegistryCode::InvalidResourceContract;
    }
    if (descriptor.allowsDataOnlyCompletion &&
        descriptor.producedResources.empty())
    {
        // A data-only completion is meaningful only when the feature owns a
        // declared typed output that the blackboard can prove was published.
        return RenderFeatureRegistryCode::InvalidResourceContract;
    }
    const bool hasSnapshotChannel =
        descriptor.snapshotChannel != RenderFeatureChannelId::Invalid;
    if ((hasSnapshotChannel &&
         (!IsKnownRenderFeatureChannel(descriptor.snapshotChannel) ||
          descriptor.snapshotChannelSchemaVersion !=
              GetRenderFeatureChannelSchemaVersion(
                  descriptor.snapshotChannel))) ||
        (!hasSnapshotChannel &&
         descriptor.snapshotChannelSchemaVersion != 0))
    {
        return RenderFeatureRegistryCode::InvalidSnapshotChannel;
    }
    if (HasDuplicateResources(descriptor.consumedResources) ||
        HasDuplicateResources(descriptor.producedResources))
    {
        return RenderFeatureRegistryCode::DuplicateResourceContract;
    }
    for (const RenderFeatureResourceContract& produced :
         descriptor.producedResources)
    {
        if (ContainsResource(descriptor.consumedResources, produced))
        {
            return RenderFeatureRegistryCode::DependencyCycle;
        }
    }

    Entry entry;
    entry.feature = std::move(feature);
    entry.descriptor = std::move(descriptor);
    entry.registrationOrdinal = m_features.size();
    m_features.push_back(std::move(entry));
    return RenderFeatureRegistryCode::Success;
}

RenderFeatureRegistryCode RenderFeatureRegistry::DeclareExternalResource(
    RenderFeatureResourceContract resource) noexcept
{
    if (m_sealed)
    {
        return RenderFeatureRegistryCode::RegistrySealed;
    }
    if (!IsDeclaredResource(resource.resource) ||
        resource.schemaVersion != GetResourceSchemaVersion(resource.resource))
    {
        return RenderFeatureRegistryCode::InvalidResourceContract;
    }
    std::optional<uint32>& schema = m_externalResources[ToIndex(resource.resource)];
    if (schema.has_value())
    {
        return RenderFeatureRegistryCode::DuplicateProducer;
    }
    schema = resource.schemaVersion;
    return RenderFeatureRegistryCode::Success;
}

RenderFeatureRegistryCode RenderFeatureRegistry::DeclareImplementedRecipe(
    RenderPipelineRecipe recipe) noexcept
{
    if (m_sealed)
    {
        return RenderFeatureRegistryCode::RegistrySealed;
    }
    if (!IsDeclaredRenderPipelineRecipe(recipe))
    {
        return RenderFeatureRegistryCode::InvalidRecipeMask;
    }
    const RenderPipelineRecipeMask mask = ToMask(recipe);
    if ((m_implementedRecipes & mask) != 0)
    {
        return RenderFeatureRegistryCode::InvalidRecipeMask;
    }
    m_implementedRecipes |= mask;
    return RenderFeatureRegistryCode::Success;
}

RenderFeatureRegistryCode RenderFeatureRegistry::Seal()
{
    if (m_sealed)
    {
        return RenderFeatureRegistryCode::RegistrySealed;
    }
    if (m_features.empty() || m_implementedRecipes == 0)
    {
        return RenderFeatureRegistryCode::InvalidFeature;
    }

    for (const Entry& entry : m_features)
    {
        const RenderFeatureDescriptor& descriptor = entry.descriptor;
        if (descriptor.optionalRecipes == 0 &&
            descriptor.fallbackPolicy != RenderFeatureFallbackPolicy::None)
        {
            return RenderFeatureRegistryCode::InvalidFallback;
        }
        if (descriptor.optionalRecipes != 0 &&
            descriptor.fallbackPolicy == RenderFeatureFallbackPolicy::None)
        {
            return RenderFeatureRegistryCode::InvalidFallback;
        }
        if (descriptor.fallbackPolicy ==
            RenderFeatureFallbackPolicy::ExplicitFeature)
        {
            if (!descriptor.fallbackFeatureId.IsValid() ||
                descriptor.fallbackFeatureId == descriptor.featureId)
            {
                return RenderFeatureRegistryCode::InvalidFallback;
            }
            if (FindEntry(descriptor.fallbackFeatureId) == nullptr)
            {
                return RenderFeatureRegistryCode::MissingFallback;
            }
            const RenderFeatureDescriptor& fallback =
                FindEntry(descriptor.fallbackFeatureId)->descriptor;
            if ((fallback.optionalRecipes & descriptor.optionalRecipes) !=
                    descriptor.optionalRecipes ||
                (fallback.qualityTiers & descriptor.qualityTiers) !=
                    descriptor.qualityTiers ||
                fallback.phase != descriptor.phase ||
                !HasEquivalentResourceContracts(
                    fallback.producedResources,
                    descriptor.producedResources))
            {
                return RenderFeatureRegistryCode::InvalidFallback;
            }
        }
        else if (descriptor.fallbackFeatureId.IsValid())
        {
            return RenderFeatureRegistryCode::InvalidFallback;
        }
    }

    // One profile may only have a linear fallback chain. Converging fallback
    // parents would prepare the same feature twice and make receipt ownership
    // ambiguous even if the graph happened to deduplicate its passes.
    for (size_t left = 0; left < m_features.size(); ++left)
    {
        const RenderFeatureDescriptor& leftDescriptor =
            m_features[left].descriptor;
        if (leftDescriptor.fallbackPolicy !=
            RenderFeatureFallbackPolicy::ExplicitFeature)
        {
            continue;
        }
        for (size_t right = left + 1; right < m_features.size(); ++right)
        {
            const RenderFeatureDescriptor& rightDescriptor =
                m_features[right].descriptor;
            if (rightDescriptor.fallbackPolicy ==
                    RenderFeatureFallbackPolicy::ExplicitFeature &&
                leftDescriptor.fallbackFeatureId ==
                    rightDescriptor.fallbackFeatureId &&
                FallbackDomainsOverlap(leftDescriptor, rightDescriptor))
            {
                return RenderFeatureRegistryCode::InvalidFallback;
            }
        }
    }

    // A declared recipe is a construction-time promise for every public
    // quality tier. Do not defer an empty recipe/profile combination until
    // frame execution where it could be mistaken for a successful no-op.
    for (uint8 recipeValue = 0;
         recipeValue < static_cast<uint8>(RenderPipelineRecipe::Count);
         ++recipeValue)
    {
        const RenderPipelineRecipe recipe =
            static_cast<RenderPipelineRecipe>(recipeValue);
        if ((m_implementedRecipes & ToMask(recipe)) == 0)
        {
            continue;
        }
        for (uint8 qualityValue = 0;
             qualityValue < static_cast<uint8>(RenderQualityTier::Count);
             ++qualityValue)
        {
            const RenderQualityTier quality =
                static_cast<RenderQualityTier>(qualityValue);
            const bool covered = std::any_of(
                m_features.begin(),
                m_features.end(),
                [recipe, quality](const Entry& candidate)
                {
                    return IsActiveForProfile(
                        candidate.descriptor, recipe, quality);
                });
            if (!covered)
            {
                return RenderFeatureRegistryCode::RecipeNotImplemented;
            }
        }
    }

    const RenderFeatureRegistryCode validation =
        ValidateAndBuildExecutionOrder();
    if (validation != RenderFeatureRegistryCode::Success)
    {
        m_executionOrder.clear();
        return validation;
    }
    m_sealed = true;
    return RenderFeatureRegistryCode::Success;
}

const RenderFeatureDescriptor* RenderFeatureRegistry::FindDescriptor(
    RenderFeatureId featureId) const noexcept
{
    const Entry* entry = FindEntry(featureId);
    return entry != nullptr ? &entry->descriptor : nullptr;
}

RenderFeatureFramePrepareResult RenderFeatureRegistry::PrepareFrame(
    RenderFeatureContext context)
{
    ResetFrame();
    RenderFeatureFramePrepareResult result;
    if (!m_sealed)
    {
        result.code = RenderFeatureRegistryCode::RegistryNotSealed;
        result.diagnostic = "Render feature registry is not sealed";
        return result;
    }
    const auto rejectInvalidContext = [&result](const char* diagnostic)
    {
        result.code = RenderFeatureRegistryCode::InvalidFrameContext;
        result.diagnostic = diagnostic;
        return result;
    };
    if (!context.frameIdentity.IsValid())
        return rejectInvalidContext("Render feature frame identity is invalid");
    if (!IsValidRenderFrameStamp(context.frameStamp))
        return rejectInvalidContext("Render feature frame stamp is invalid");
    if (context.frameStamp.sequence != context.frameIdentity.frameSequence)
        return rejectInvalidContext(
            "Render feature frame stamp does not match the frame identity");
    if (!IsValidEffectFrameTime(context.effectTime))
        return rejectInvalidContext("Render feature effect time is invalid");
    if (!context.outputExtent.IsValid())
        return rejectInvalidContext("Render feature output extent is invalid");
    if (context.viewFamily == nullptr ||
        !IsValidFeatureViewFamily(*context.viewFamily))
    {
        return rejectInvalidContext(
            "Render feature view family snapshot is invalid");
    }
    const RenderViewFamilySnapshot& viewFamily = *context.viewFamily;
    if (context.frameIdentity != viewFamily.identity ||
        context.frameStamp != viewFamily.frame ||
        context.effectTime != viewFamily.effectTime ||
        context.outputExtent != viewFamily.extent ||
        context.viewFamilyId != viewFamily.familyId ||
        context.primaryViewIndex != viewFamily.primaryViewIndex ||
        context.viewCount != viewFamily.views.size())
    {
        return rejectInvalidContext(
            "Render feature context does not match its frozen view family");
    }
    if (context.recordingViewOrdinal >= viewFamily.views.size() ||
        context.recordEpoch == 0)
    {
        return rejectInvalidContext(
            "Render feature recording identity is invalid");
    }
    if (!context.viewIdentity.IsValid() ||
        context.viewIdentity !=
            viewFamily.views[context.recordingViewOrdinal].identity)
    {
        return rejectInvalidContext(
            "Render feature recording view identity is invalid");
    }
    if (context.backend == RHIBackendType::None)
        return rejectInvalidContext("Render feature backend is invalid");
    if (!IsValidRenderPipelineRequest(context.pipeline))
        return rejectInvalidContext("Render feature pipeline request is invalid");

    result.pipeline = ResolveRenderPipelineRequest(context.pipeline);
    if (!result.pipeline.IsSuccessful())
    {
        result.code = RenderFeatureRegistryCode::InvalidFrameContext;
        result.diagnostic = "Render pipeline request was rejected";
        return result;
    }
    context.resolvedRecipe = result.pipeline.resolvedRecipe;
    context.resolvedQuality = result.pipeline.resolvedQuality;
    context.profileHash = result.pipeline.profileHash;
    m_activeContext = context;
    m_activeResolution = result.pipeline;
    m_frameAttempted = true;
    if (!IsRecipeImplemented(result.pipeline.resolvedRecipe))
    {
        result.code = RenderFeatureRegistryCode::RecipeNotImplemented;
        result.diagnostic = std::string("Pipeline recipe is not implemented: ") +
                            GetRenderPipelineRecipeName(
                                result.pipeline.resolvedRecipe);
        m_activeDiagnostic = result.diagnostic;
        return result;
    }

    for (RenderFeatureId featureId : m_executionOrder)
    {
        Entry* entry = FindEntry(featureId);
        if (entry == nullptr)
        {
            result.code = RenderFeatureRegistryCode::InvalidFeature;
            result.diagnostic = "Sealed feature order references a missing feature";
            m_activeDiagnostic = result.diagnostic;
            return result;
        }

        const RenderPipelineRecipeMask recipeMask =
            ToMask(result.pipeline.resolvedRecipe);
        if (!IsActiveForProfile(entry->descriptor,
                                result.pipeline.resolvedRecipe,
                                result.pipeline.resolvedQuality))
        {
            continue;
        }
        const bool required =
            (entry->descriptor.requiredRecipes & recipeMask) != 0;
        const bool optional =
            (entry->descriptor.optionalRecipes & recipeMask) != 0;
        if (!required && !optional)
        {
            continue;
        }
        if (std::any_of(
                m_activeFeatures.begin(),
                m_activeFeatures.end(),
                [featureId](const ActiveFeature& active)
                {
                    return active.featureId == featureId;
                }))
        {
            continue;
        }
        // A feature referenced by an applicable descriptor is selected only
        // through that descriptor's explicit fallback path. Otherwise a ready
        // primary and its ready fallback could both publish the same semantic
        // resource during one graph recording.
        if (IsFallbackTargetForProfile(
                featureId,
                result.pipeline.resolvedRecipe,
                result.pipeline.resolvedQuality))
        {
            continue;
        }

        const RenderFeatureRegistryCode selection = SelectFeature(
            *entry,
            required ? RenderFeatureRequirement::Required
                     : RenderFeatureRequirement::Optional,
            context,
            false,
            {},
            result.diagnostic);
        if (selection != RenderFeatureRegistryCode::Success)
        {
            result.code = selection;
            m_activeDiagnostic = result.diagnostic;
            return result;
        }
    }

    m_framePrepared = true;
    result.code = RenderFeatureRegistryCode::Success;
    return result;
}

RenderFeatureRegistryCode RenderFeatureRegistry::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    m_graphCompositionSucceeded = false;
    m_executionEvidenceFinalized = false;
    m_featureGraphRegistrations.clear();
    m_recordedGraphIdentity = 0;
    m_recordedGraphGeneration = 0;
    const RenderGraphBlackboardProvenance& provenance =
        blackboard.GetProvenance();
    if (!m_framePrepared || !builder.IsValid() ||
        !provenance.IsValid() || provenance != builder.GetProvenance() ||
        provenance.graphIdentity != builder.GetGraphIdentity() ||
        provenance.recordingGeneration != builder.GetRecordingGeneration() ||
        provenance.frameSequence !=
            m_activeContext.frameIdentity.frameSequence ||
        provenance.viewOrdinal != m_activeContext.recordingViewOrdinal ||
        provenance.recordEpoch != m_activeContext.recordEpoch)
    {
        return RenderFeatureRegistryCode::InvalidFrameContext;
    }
    m_recordedGraphIdentity = builder.GetGraphIdentity();
    m_recordedGraphGeneration = builder.GetRecordingGeneration();

    for (RenderFeatureId featureId : m_executionOrder)
    {
        const auto activeIt = std::find_if(
            m_activeFeatures.begin(),
            m_activeFeatures.end(),
            [featureId](const ActiveFeature& active)
            {
                return active.featureId == featureId && active.addPasses;
            });
        if (activeIt == m_activeFeatures.end())
        {
            continue;
        }
        Entry* entry = FindEntry(featureId);
        if (entry == nullptr)
        {
            return RenderFeatureRegistryCode::InvalidFeature;
        }

        const RenderGraph::Diagnostics beforeRegistration =
            builder.GetGraph().GetDiagnostics();
        const size_t firstPassIndex = beforeRegistration.passes.size();
        entry->feature->AddPasses(builder, blackboard);
        const RenderGraph::Diagnostics afterRegistration =
            builder.GetGraph().GetDiagnostics();

        FeatureGraphRegistration graphRegistration;
        graphRegistration.featureId = featureId;
        for (size_t passIndex = firstPassIndex;
             passIndex < afterRegistration.passes.size();
             ++passIndex)
        {
            graphRegistration.passIndices.push_back(
                static_cast<uint32>(passIndex));
            graphRegistration.passNames.push_back(
                afterRegistration.passes[passIndex].name);
        }
        m_featureGraphRegistrations.push_back(std::move(graphRegistration));
        if (blackboard.GetFailureCode() !=
            RenderGraphBlackboardCode::Success)
        {
            m_activeDiagnostic =
                "Render feature '" + entry->descriptor.name +
                "' violated the typed blackboard contract: " +
                GetRenderGraphBlackboardCodeName(
                    blackboard.GetFailureCode());
            return RenderFeatureRegistryCode::GraphCompositionFailed;
        }
        for (const RenderFeatureResourceContract& produced :
             entry->descriptor.producedResources)
        {
            if (!blackboard.WasPublishedBy(produced, featureId))
            {
                m_activeDiagnostic =
                    "Render feature '" + entry->descriptor.name +
                    "' did not publish a declared typed output";
                return RenderFeatureRegistryCode::GraphCompositionFailed;
            }
        }
        for (const RenderFeatureResourceContract& consumed :
             entry->descriptor.consumedResources)
        {
            if (!blackboard.WasConsumedBy(consumed, featureId))
            {
                m_activeDiagnostic =
                    "Render feature '" + entry->descriptor.name +
                    "' did not consume a declared typed input";
                return RenderFeatureRegistryCode::GraphCompositionFailed;
            }
        }
        const RenderFeatureReadiness afterComposition =
            entry->feature->ResolveReadiness();
        activeIt->readiness = afterComposition;
        if (!afterComposition.IsReadyToExecute())
        {
            m_activeDiagnostic = afterComposition.diagnostic.empty()
                ? "Selected feature failed during graph composition"
                : afterComposition.diagnostic;
            return RenderFeatureRegistryCode::GraphCompositionFailed;
        }
    }

    m_graphCompositionSucceeded = true;
    return RenderFeatureRegistryCode::Success;
}

RenderFeatureRegistryCode RenderFeatureRegistry::FinalizeGraphExecution(
    const RenderGraph& graph)
{
    m_executionEvidenceFinalized = false;
    if (!m_framePrepared || !m_graphCompositionSucceeded ||
        m_recordedGraphIdentity == 0 || m_recordedGraphGeneration == 0 ||
        !graph.GetCompileStats().compileValid ||
        graph.GetGraphIdentity() != m_recordedGraphIdentity ||
        graph.GetRecordingGeneration() != m_recordedGraphGeneration)
    {
        m_activeDiagnostic =
            "Render feature graph execution evidence does not match the composed graph";
        return RenderFeatureRegistryCode::GraphCompositionFailed;
    }

    const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    for (FeatureGraphRegistration& registration :
         m_featureGraphRegistrations)
    {
        registration.observedGraphPassCount = 0;
        registration.observedExecutedGraphPassCount = 0;
        registration.observedGraphPassNames.clear();
        registration.semanticExecutionDiagnostic.clear();
        registration.executionEvidenceFinalized = false;

        for (size_t offset = 0; offset < registration.passIndices.size();
             ++offset)
        {
            const uint32 diagnosticIndex = registration.passIndices[offset];
            if (diagnosticIndex == RVX_INVALID_INDEX ||
                diagnosticIndex >= diagnostics.passes.size())
            {
                m_activeDiagnostic =
                    "Render feature graph execution evidence is incomplete";
                return RenderFeatureRegistryCode::GraphCompositionFailed;
            }

            const RenderGraph::PassDiagnostic& pass = diagnostics.passes[
                static_cast<size_t>(diagnosticIndex)];
            if (pass.name != registration.passNames[offset])
            {
                m_activeDiagnostic =
                    "Render feature graph pass identity changed after composition";
                return RenderFeatureRegistryCode::GraphCompositionFailed;
            }

            // Culled nodes have no execution evidence. This includes empty
            // workload nodes deliberately removed by RenderGraph compilation.
            if (pass.culled)
            {
                continue;
            }

            ++registration.observedGraphPassCount;
            registration.observedGraphPassNames.push_back(pass.name);
            if (pass.executedLastRun &&
                pass.semanticExecutionSucceededLastRun)
            {
                ++registration.observedExecutedGraphPassCount;
            }
            else if (pass.executedLastRun &&
                     registration.semanticExecutionDiagnostic.empty())
            {
                registration.semanticExecutionDiagnostic =
                    pass.semanticExecutionDiagnostic.empty()
                        ? "RenderGraph pass reported semantic execution failure"
                        : pass.semanticExecutionDiagnostic;
            }
        }

        registration.executionEvidenceFinalized = true;
    }

    m_executionEvidenceFinalized = true;

    // Features may publish detailed receipts only after the registry has
    // finalized all graph-owned evidence for this exact recording. This
    // prevents a scheduled callback from being reported as an executed pass.
    for (const FeatureGraphRegistration& registration :
         m_featureGraphRegistrations)
    {
        Entry* const entry = FindEntry(registration.featureId);
        if (entry == nullptr)
        {
            m_executionEvidenceFinalized = false;
            m_activeDiagnostic =
                "Render feature execution evidence lost its registered feature";
            return RenderFeatureRegistryCode::GraphCompositionFailed;
        }

        RenderFeatureGraphExecutionEvidence evidence;
        evidence.featureId = registration.featureId;
        evidence.graphIdentity = m_recordedGraphIdentity;
        evidence.recordingGeneration = m_recordedGraphGeneration;
        evidence.frameIdentity = m_activeContext.frameIdentity;
        evidence.viewIdentity = m_activeContext.viewIdentity;
        evidence.graphPassCount = registration.observedGraphPassCount;
        evidence.executedGraphPassCount =
            registration.observedExecutedGraphPassCount;
        evidence.semanticExecutionSucceeded =
            evidence.graphPassCount != 0 &&
            evidence.executedGraphPassCount == evidence.graphPassCount &&
            registration.semanticExecutionDiagnostic.empty();
        evidence.diagnostic = registration.semanticExecutionDiagnostic;
        const auto activeIt = std::find_if(
            m_activeFeatures.begin(), m_activeFeatures.end(),
            [&registration](const ActiveFeature& active)
            {
                return active.featureId == registration.featureId &&
                       active.addPasses;
            });
        evidence.contractCompletedWithoutGraphWork =
            entry->descriptor.allowsDataOnlyCompletion &&
            !entry->descriptor.producedResources.empty() &&
            registration.passIndices.empty() &&
            registration.observedGraphPassCount == 0 &&
            registration.observedExecutedGraphPassCount == 0 &&
            registration.executionEvidenceFinalized &&
            registration.semanticExecutionDiagnostic.empty() &&
            activeIt != m_activeFeatures.end() &&
            !activeIt->fallbackUsed &&
            activeIt->readiness.IsReadyToExecute() &&
            activeIt->readiness.diagnostic.empty();
        entry->feature->OnGraphExecutionFinalized(evidence);
    }

    return RenderFeatureRegistryCode::Success;
}

RenderExecutionReceipt RenderFeatureRegistry::CollectExecutionReceipt(
    const RenderExecutionBuildIdentity& buildIdentity) const
{
    RenderExecutionReceipt receipt;
    // The registry owns feature-graph evidence only. SceneRenderer upgrades
    // this intermediate candidate to schema five after it has bound exact
    // lighting, deformation, and render-pass evidence.
    receipt.schemaVersion = 2;
    receipt.available = m_frameAttempted;
    receipt.graphComposed = m_framePrepared && m_graphCompositionSucceeded;
    receipt.executionSucceeded = receipt.graphComposed;
    receipt.build = buildIdentity;
    receipt.frameIdentity = m_activeContext.frameIdentity;
    receipt.viewIdentity = m_activeContext.viewIdentity;
    receipt.backend = m_activeContext.backend;
    receipt.requestedRecipe = m_activeContext.pipeline.recipe;
    receipt.resolvedRecipe = m_activeResolution.resolvedRecipe;
    receipt.quality = m_activeResolution.resolvedQuality;
    receipt.profileHash = m_activeResolution.profileHash;
    receipt.diagnostic = m_activeDiagnostic;

    for (const ActiveFeature& active : m_activeFeatures)
    {
        const Entry* entry = FindEntry(active.featureId);
        if (entry == nullptr)
        {
            receipt.executionSucceeded = false;
            continue;
        }
        RenderFeatureExecutionReceipt featureReceipt = active.addPasses
            ? entry->feature->CollectReceipt()
            : RenderFeatureExecutionReceipt{};
        const bool featureClaimedExecution = active.addPasses &&
            featureReceipt.readiness.executed;
        const bool featureClaimedDataOnlyCompletion = active.addPasses &&
            featureReceipt.contractCompletedWithoutGraphWork;
        featureReceipt.featureId = active.featureId;
        featureReceipt.featureSchemaVersion = entry->descriptor.schemaVersion;
        featureReceipt.phase = entry->descriptor.phase;
        featureReceipt.requirement = active.requirement;
        featureReceipt.selected = true;
        featureReceipt.fallbackUsed = active.fallbackUsed;
        featureReceipt.selectedFallbackFeatureId =
            active.selectedFallbackFeatureId;
        featureReceipt.fallbackForFeatureId = active.fallbackForFeatureId;
        if (!active.addPasses)
        {
            featureReceipt.readiness = active.readiness;
            featureReceipt.diagnostic = active.readiness.diagnostic;
        }
        else
        {
            const FeatureGraphRegistration* registration =
                FindFeatureGraphRegistration(active.featureId);
            featureReceipt.readiness = active.readiness;
            featureReceipt.graphPassCount = 0;
            featureReceipt.executedGraphPassCount = 0;
            featureReceipt.graphPassNames.clear();
            featureReceipt.contractCompletedWithoutGraphWork = false;

            if (registration == nullptr)
            {
                receipt.executionSucceeded = false;
                featureReceipt.readiness.executed = false;
                featureReceipt.diagnostic =
                    "Render feature has no registry-owned graph registration";
            }
            else if (m_executionEvidenceFinalized &&
                     registration->executionEvidenceFinalized)
            {
                featureReceipt.graphPassCount =
                    registration->observedGraphPassCount;
                featureReceipt.executedGraphPassCount =
                    registration->observedExecutedGraphPassCount;
                featureReceipt.graphPassNames =
                    registration->observedGraphPassNames;
                featureReceipt.contractCompletedWithoutGraphWork =
                    entry->descriptor.allowsDataOnlyCompletion &&
                    !entry->descriptor.producedResources.empty() &&
                    registration->passIndices.empty() &&
                    registration->observedGraphPassCount == 0 &&
                    registration->observedExecutedGraphPassCount == 0 &&
                    registration->semanticExecutionDiagnostic.empty() &&
                    !active.fallbackUsed &&
                    active.readiness.IsReadyToExecute() &&
                    active.readiness.diagnostic.empty() &&
                    featureReceipt.diagnostic.empty();
                featureReceipt.readiness.executed =
                    featureReceipt.readiness.IsReadyToExecute() &&
                    (featureReceipt.contractCompletedWithoutGraphWork ||
                     (featureReceipt.graphPassCount != 0 &&
                      featureReceipt.executedGraphPassCount ==
                          featureReceipt.graphPassCount));
                if (!featureReceipt.readiness.executed &&
                    !registration->semanticExecutionDiagnostic.empty())
                {
                    featureReceipt.diagnostic =
                        registration->semanticExecutionDiagnostic;
                }
            }
            else
            {
                featureReceipt.graphPassCount = static_cast<uint32>(
                    registration->passIndices.size());
                featureReceipt.graphPassNames = registration->passNames;
                featureReceipt.readiness.executed = false;
            }

            if (featureClaimedExecution)
            {
                receipt.executionSucceeded = false;
                featureReceipt.readiness.executed = false;
                featureReceipt.contractCompletedWithoutGraphWork = false;
                featureReceipt.diagnostic =
                    "Render feature self-reported execution; graph evidence is registry-owned";
            }
            else if (featureClaimedDataOnlyCompletion)
            {
                receipt.executionSucceeded = false;
                featureReceipt.readiness.executed = false;
                featureReceipt.contractCompletedWithoutGraphWork = false;
                featureReceipt.diagnostic =
                    "Render feature self-reported data-only completion; graph evidence is registry-owned";
            }

            if (!featureReceipt.readiness.IsReadyToExecute() ||
                !featureReceipt.readiness.executed)
            {
                receipt.executionSucceeded = false;
                if (receipt.diagnostic.empty())
                {
                    receipt.diagnostic = featureReceipt.diagnostic.empty()
                        ? "Selected render feature '" +
                              entry->descriptor.name +
                              "' did not execute (observed " +
                              std::to_string(
                                  featureReceipt.executedGraphPassCount) +
                              "/" +
                              std::to_string(featureReceipt.graphPassCount) +
                              " graph passes)"
                        : entry->descriptor.name + ": " +
                              featureReceipt.diagnostic;
                }
            }
        }
        receipt.features.push_back(std::move(featureReceipt));
    }
    receipt.succeeded = receipt.executionSucceeded && receipt.presented;
    return receipt;
}

void RenderFeatureRegistry::ResetFrame() noexcept
{
    m_activeFeatures.clear();
    m_featureGraphRegistrations.clear();
    m_activeContext = {};
    m_activeResolution = {};
    m_activeDiagnostic.clear();
    m_frameAttempted = false;
    m_framePrepared = false;
    m_graphCompositionSucceeded = false;
    m_recordedGraphIdentity = 0;
    m_recordedGraphGeneration = 0;
    m_executionEvidenceFinalized = false;
}

RenderFeatureRegistry::Entry* RenderFeatureRegistry::FindEntry(
    RenderFeatureId featureId) noexcept
{
    const auto it = std::find_if(
        m_features.begin(),
        m_features.end(),
        [featureId](const Entry& entry)
        {
            return entry.descriptor.featureId == featureId;
        });
    return it != m_features.end() ? &*it : nullptr;
}

const RenderFeatureRegistry::Entry* RenderFeatureRegistry::FindEntry(
    RenderFeatureId featureId) const noexcept
{
    const auto it = std::find_if(
        m_features.begin(),
        m_features.end(),
        [featureId](const Entry& entry)
        {
            return entry.descriptor.featureId == featureId;
        });
    return it != m_features.end() ? &*it : nullptr;
}

const RenderFeatureRegistry::FeatureGraphRegistration*
RenderFeatureRegistry::FindFeatureGraphRegistration(
    RenderFeatureId featureId) const noexcept
{
    const auto it = std::find_if(
        m_featureGraphRegistrations.begin(),
        m_featureGraphRegistrations.end(),
        [featureId](const FeatureGraphRegistration& registration)
        {
            return registration.featureId == featureId;
        });
    return it != m_featureGraphRegistrations.end() ? &*it : nullptr;
}

bool RenderFeatureRegistry::IsRecipeImplemented(
    RenderPipelineRecipe recipe) const noexcept
{
    return IsDeclaredRenderPipelineRecipe(recipe) &&
           (m_implementedRecipes & ToMask(recipe)) != 0;
}

bool RenderFeatureRegistry::IsFallbackTargetForProfile(
    RenderFeatureId featureId,
    RenderPipelineRecipe recipe,
    RenderQualityTier quality) const noexcept
{
    if (!featureId.IsValid() || !IsDeclaredRenderPipelineRecipe(recipe) ||
        !IsDeclaredRenderQualityTier(quality))
    {
        return false;
    }
    return std::any_of(
        m_features.begin(),
        m_features.end(),
        [featureId, recipe, quality](const Entry& entry)
        {
            const RenderFeatureDescriptor& descriptor = entry.descriptor;
            return descriptor.fallbackPolicy ==
                       RenderFeatureFallbackPolicy::ExplicitFeature &&
                   descriptor.fallbackFeatureId == featureId &&
                   (descriptor.optionalRecipes & ToMask(recipe)) != 0 &&
                   (descriptor.qualityTiers & ToMask(quality)) != 0;
        });
}

bool RenderFeatureRegistry::ReachesFallback(
    RenderFeatureId source,
    RenderFeatureId target) const noexcept
{
    if (!source.IsValid() || !target.IsValid() || source == target)
    {
        return false;
    }

    RenderFeatureId current = source;
    for (size_t depth = 0; depth < m_features.size(); ++depth)
    {
        const Entry* entry = FindEntry(current);
        if (entry == nullptr ||
            entry->descriptor.fallbackPolicy !=
                RenderFeatureFallbackPolicy::ExplicitFeature)
        {
            return false;
        }
        current = entry->descriptor.fallbackFeatureId;
        if (current == target)
        {
            return true;
        }
        if (!current.IsValid())
        {
            return false;
        }
    }
    return false;
}

RenderFeatureRegistryCode
RenderFeatureRegistry::ValidateAndBuildExecutionOrder()
{
    const size_t featureCount = m_features.size();
    std::array<std::vector<size_t>,
               static_cast<size_t>(RenderBlackboardResourceId::Count)>
        producersByResource{};

    for (size_t featureIndex = 0; featureIndex < featureCount; ++featureIndex)
    {
        for (const RenderFeatureResourceContract& produced :
             m_features[featureIndex].descriptor.producedResources)
        {
            const size_t resourceIndex = ToIndex(produced.resource);
            if (m_externalResources[resourceIndex].has_value())
            {
                return RenderFeatureRegistryCode::DuplicateProducer;
            }
            producersByResource[resourceIndex].push_back(featureIndex);
        }
    }

    // Multiple static producers are legal when their recipe/quality domains
    // cannot overlap, or when overlapping members form one explicit fallback
    // chain. Runtime selection still activates exactly one producer for every
    // semantic slot in a recording.
    for (const std::vector<size_t>& producers : producersByResource)
    {
        for (size_t left = 0; left < producers.size(); ++left)
        {
            for (size_t right = left + 1; right < producers.size(); ++right)
            {
                const RenderFeatureId leftId =
                    m_features[producers[left]].descriptor.featureId;
                const RenderFeatureId rightId =
                    m_features[producers[right]].descriptor.featureId;
                if (ActivationDomainsOverlap(
                        m_features[producers[left]].descriptor,
                        m_features[producers[right]].descriptor,
                        m_implementedRecipes) &&
                    !ReachesFallback(leftId, rightId) &&
                    !ReachesFallback(rightId, leftId))
                {
                    return RenderFeatureRegistryCode::DuplicateProducer;
                }
            }
        }
    }

    std::vector<std::vector<size_t>> edges(featureCount);
    std::vector<size_t> indegree(featureCount, 0);
    const auto addEdge = [&edges, &indegree](size_t producer, size_t consumer)
    {
        if (std::find(edges[producer].begin(),
                      edges[producer].end(),
                      consumer) == edges[producer].end())
        {
            edges[producer].push_back(consumer);
            ++indegree[consumer];
        }
    };

    for (size_t consumerIndex = 0; consumerIndex < featureCount;
         ++consumerIndex)
    {
        const RenderFeatureDescriptor& consumer =
            m_features[consumerIndex].descriptor;
        if ((GetRecipeMask(consumer) & m_implementedRecipes) == 0)
        {
            continue;
        }
        for (const RenderFeatureResourceContract& consumed :
             consumer.consumedResources)
        {
            const size_t resourceIndex = ToIndex(consumed.resource);
            if (!producersByResource[resourceIndex].empty())
            {
                bool matchingProducer = false;
                for (const size_t producerIndex :
                     producersByResource[resourceIndex])
                {
                    const RenderFeatureDescriptor& producer =
                        m_features[producerIndex].descriptor;
                    if (!ActivationDomainsOverlap(
                            producer, consumer, m_implementedRecipes))
                    {
                        continue;
                    }
                    matchingProducer = true;
                    const auto producedIt = std::find_if(
                        producer.producedResources.begin(),
                        producer.producedResources.end(),
                        [&consumed](
                            const RenderFeatureResourceContract& value)
                        {
                            return value.resource == consumed.resource;
                        });
                    if (producedIt == producer.producedResources.end())
                    {
                        return RenderFeatureRegistryCode::MissingProducer;
                    }
                    if (producedIt->schemaVersion != consumed.schemaVersion)
                    {
                        return RenderFeatureRegistryCode::ResourceSchemaMismatch;
                    }
                    if (producer.phase > consumer.phase)
                    {
                        return RenderFeatureRegistryCode::IllegalPhaseEdge;
                    }
                    addEdge(producerIndex, consumerIndex);
                }
                if (!matchingProducer)
                {
                    return RenderFeatureRegistryCode::MissingProducer;
                }

                // A producer must cover every recipe/quality combination in
                // which the consumer can run. This prevents a graph from
                // sealing when one profile would discover its missing input
                // only at frame execution time.
                for (uint8 recipeValue = 0;
                     recipeValue < static_cast<uint8>(
                         RenderPipelineRecipe::Count);
                     ++recipeValue)
                {
                    const RenderPipelineRecipe recipe =
                        static_cast<RenderPipelineRecipe>(recipeValue);
                    if ((m_implementedRecipes & ToMask(recipe)) == 0)
                    {
                        continue;
                    }
                    for (uint8 qualityValue = 0;
                         qualityValue < static_cast<uint8>(
                             RenderQualityTier::Count);
                         ++qualityValue)
                    {
                        const RenderQualityTier quality =
                            static_cast<RenderQualityTier>(qualityValue);
                        if (!IsActiveForProfile(consumer, recipe, quality))
                        {
                            continue;
                        }
                        const bool covered = std::any_of(
                            producersByResource[resourceIndex].begin(),
                            producersByResource[resourceIndex].end(),
                            [this, recipe, quality](size_t index)
                            {
                                return IsActiveForProfile(
                                    m_features[index].descriptor,
                                    recipe,
                                    quality);
                            });
                        if (!covered)
                        {
                            return RenderFeatureRegistryCode::MissingProducer;
                        }
                    }
                }
            }
            else if (m_externalResources[resourceIndex].has_value())
            {
                if (*m_externalResources[resourceIndex] !=
                    consumed.schemaVersion)
                {
                    return RenderFeatureRegistryCode::ResourceSchemaMismatch;
                }
            }
            else
            {
                return RenderFeatureRegistryCode::MissingProducer;
            }
        }

        if (consumer.fallbackPolicy ==
            RenderFeatureFallbackPolicy::ExplicitFeature)
        {
            const Entry* fallback = FindEntry(consumer.fallbackFeatureId);
            if (fallback == nullptr)
            {
                return RenderFeatureRegistryCode::MissingFallback;
            }
            addEdge(consumerIndex, fallback->registrationOrdinal);
        }
    }

    m_executionOrder.clear();
    m_executionOrder.reserve(featureCount);
    std::vector<bool> emitted(featureCount, false);
    while (m_executionOrder.size() < featureCount)
    {
        size_t selected = std::numeric_limits<size_t>::max();
        for (size_t index = 0; index < featureCount; ++index)
        {
            if (!emitted[index] && indegree[index] == 0)
            {
                selected = index;
                break;
            }
        }
        if (selected == std::numeric_limits<size_t>::max())
        {
            return RenderFeatureRegistryCode::DependencyCycle;
        }
        emitted[selected] = true;
        m_executionOrder.push_back(m_features[selected].descriptor.featureId);
        for (size_t dependent : edges[selected])
        {
            if (indegree[dependent] == 0)
            {
                return RenderFeatureRegistryCode::DependencyCycle;
            }
            --indegree[dependent];
        }
    }
    return RenderFeatureRegistryCode::Success;
}

RenderFeatureRegistryCode RenderFeatureRegistry::SelectFeature(
    Entry& entry,
    RenderFeatureRequirement requirement,
    const RenderFeatureContext& context,
    bool selectedAsFallback,
    RenderFeatureId fallbackFor,
    std::string& outDiagnostic)
{
    const RenderFeatureSnapshotBindingValidation snapshotValidation =
        ValidateSnapshotBinding(entry.descriptor, context);
    RenderFeatureReadiness readiness;
    if (snapshotValidation.valid)
    {
        entry.feature->Prepare(context);
        readiness = entry.feature->ResolveReadiness();
    }
    else
    {
        readiness.inputsReady = false;
        readiness.diagnostic = snapshotValidation.diagnostic;
    }
    if ((GetRecipeMask(entry.descriptor) &
         ToMask(context.resolvedRecipe)) == 0)
    {
        readiness.implementationSupported = false;
        readiness.executed = false;
        readiness.diagnostic =
            "Feature is unavailable for the selected pipeline recipe";
    }
    if (!HasCapabilities(context.hardwareCapabilities,
                         entry.descriptor.requiredCapabilities))
    {
        readiness.hardwareSupported = false;
        readiness.executed = false;
        if (readiness.diagnostic.empty())
        {
            readiness.diagnostic = "Required RHI capabilities are unavailable";
        }
    }
    if ((entry.descriptor.qualityTiers & ToMask(context.resolvedQuality)) == 0)
    {
        readiness.inputsReady = false;
        readiness.executed = false;
        readiness.diagnostic = "Feature is unavailable for the selected quality tier";
    }

    if (readiness.IsReadyToExecute())
    {
        ActiveFeature active;
        active.featureId = entry.descriptor.featureId;
        active.requirement = requirement;
        active.fallbackUsed = selectedAsFallback;
        active.fallbackForFeatureId = fallbackFor;
        active.readiness = readiness;
        m_activeFeatures.push_back(std::move(active));
        return RenderFeatureRegistryCode::Success;
    }
    if (requirement == RenderFeatureRequirement::Required)
    {
        ActiveFeature unavailable;
        unavailable.featureId = entry.descriptor.featureId;
        unavailable.requirement = requirement;
        unavailable.fallbackUsed = false;
        unavailable.addPasses = false;
        unavailable.readiness = readiness;
        m_activeFeatures.push_back(std::move(unavailable));
        outDiagnostic = readiness.diagnostic.empty()
            ? "Required render feature is unavailable"
            : readiness.diagnostic;
        return RenderFeatureRegistryCode::RequiredFeatureUnavailable;
    }

    ActiveFeature skipped;
    skipped.featureId = entry.descriptor.featureId;
    skipped.requirement = RenderFeatureRequirement::Optional;
    skipped.fallbackUsed = selectedAsFallback ||
        entry.descriptor.fallbackPolicy != RenderFeatureFallbackPolicy::None;
    skipped.selectedFallbackFeatureId =
        entry.descriptor.fallbackPolicy ==
                RenderFeatureFallbackPolicy::ExplicitFeature
            ? entry.descriptor.fallbackFeatureId
            : RenderFeatureId{};
    skipped.fallbackForFeatureId = selectedAsFallback
        ? fallbackFor
        : RenderFeatureId{};
    skipped.addPasses = false;
    skipped.readiness = readiness;
    m_activeFeatures.push_back(skipped);

    if (entry.descriptor.fallbackPolicy == RenderFeatureFallbackPolicy::Skip)
    {
        if (!entry.descriptor.producedResources.empty())
        {
            outDiagnostic = readiness.diagnostic.empty()
                ? "Optional feature cannot skip its declared outputs"
                : readiness.diagnostic;
            return RenderFeatureRegistryCode::OptionalFallbackUnavailable;
        }
        return RenderFeatureRegistryCode::Success;
    }
    if (entry.descriptor.fallbackPolicy !=
        RenderFeatureFallbackPolicy::ExplicitFeature)
    {
        outDiagnostic = "Optional feature has no declared fallback";
        return RenderFeatureRegistryCode::InvalidFallback;
    }

    Entry* fallback = FindEntry(entry.descriptor.fallbackFeatureId);
    if (fallback == nullptr)
    {
        outDiagnostic = "Declared fallback feature is missing";
        return RenderFeatureRegistryCode::MissingFallback;
    }
    return SelectFeature(*fallback,
                         RenderFeatureRequirement::Optional,
                         context,
                         true,
                         entry.descriptor.featureId,
                         outDiagnostic);
}
} // namespace RVX
