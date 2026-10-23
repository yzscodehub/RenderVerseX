#pragma once

/**
 * @file MaterialInstanceResource.h
 * @brief Resource-managed, runtime mutable PBR material instance.
 */

#include "Resource/ResourceLoadOperation.h"
#include "Resource/Types/MaterialResource.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX::Resource
{
    class ResourceManager;

    /** @brief Result codes for a manager-owned material-instance mutation. */
    enum class MaterialInstanceMutationCode : uint8
    {
        Applied = 0,
        NoChange,
        ManagerNotInitialized,
        OwnerThreadRequired,
        InvalidParent,
        InvalidInstance,
        RuntimeKeyConflict,
        InvalidPatch,
        StaticPropertyImmutable,
        DependencyUnavailable,
        NeedsResidencyRefresh,
        TransactionFailed,
    };

    /** @brief Immutable receipt returned by CreateMaterialInstance and UpdateMaterialInstance. */
    struct MaterialInstanceMutationReceipt
    {
        MaterialInstanceMutationCode code = MaterialInstanceMutationCode::InvalidInstance;
        ResourceId resourceId = InvalidResourceId;
        uint64 revision = 0;
        std::string message;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == MaterialInstanceMutationCode::Applied ||
                   code == MaterialInstanceMutationCode::NoChange;
        }
    };

    /**
     * @brief PBR values and texture overrides legal for a runtime material instance.
     *
     * A texture map entry with std::nullopt clears that instance override and
     * restores the immutable parent binding. Values in the static fields are
     * deliberately represented so callers receive a structured immutable
     * rejection instead of silently changing shader/pipeline semantics.
     */
    struct MaterialInstancePatch
    {
        std::optional<Vec4> baseColor;
        std::optional<float> metallicFactor;
        std::optional<float> roughnessFactor;
        std::optional<float> normalScale;
        std::optional<float> occlusionStrength;
        std::optional<Vec3> emissiveColor;
        std::optional<float> emissiveStrength;
        std::optional<float> alphaCutoff;

        std::unordered_map<std::string,
                           std::optional<ResourceHandle<TextureResource>>>
            textureOverrides;

        // Static/pipeline-affecting fields: v1 always rejects their presence.
        std::optional<MaterialWorkflowMode> workflowMode;
        std::optional<MaterialAlphaMode> alphaMode;
        std::optional<bool> doubleSided;
        std::optional<ResourceHandle<ShaderResource>> shader;
    };

    /**
     * @brief A mutable PBR parameter layer over one immutable MaterialResource.
     *
     * The class deliberately exposes no public mutators. ResourceManager owns
     * validation, cache/registry/dependency publication, lifecycle events and
     * revision changes so Scene and Render observe one atomic material meaning.
     */
    class MaterialInstanceResource final : public MaterialResource
    {
    public:
        static constexpr ResourceType StaticResourceType = ResourceType::Material;

        MaterialInstanceResource(ResourceHandle<MaterialResource> parent,
                                 std::string runtimeKey);
        ~MaterialInstanceResource() override;

        const char* GetTypeName() const override { return "MaterialInstance"; }
        size_t GetMemoryUsage() const override;

        [[nodiscard]] const ResourceHandle<MaterialResource>& GetParent() const noexcept
        {
            return m_parent;
        }
        [[nodiscard]] const std::string& GetRuntimeKey() const noexcept
        {
            return m_runtimeKey;
        }
        [[nodiscard]] uint64 GetRevision() const noexcept { return m_revision; }

        std::vector<ResourceId> GetRequiredDependencies() const override;

    private:
        struct PreparedState
        {
            std::shared_ptr<Material> material;
            std::unordered_map<std::string, ResourceHandle<TextureResource>> textures;
            ResourceHandle<ShaderResource> shader;
            std::vector<ResourceId> dependencies;
        };

        [[nodiscard]] bool InitializeFromParent();
        [[nodiscard]] MaterialInstanceMutationCode BuildPatchedState(
            const MaterialInstancePatch& patch,
            PreparedState& outState,
            std::string& outMessage) const;
        [[nodiscard]] bool HasSameState(const PreparedState& state) const;
        void CommitPatchedState(PreparedState&& state) noexcept;

        ResourceHandle<MaterialResource> m_parent;
        std::string m_runtimeKey;
        uint64 m_revision = 0;

        friend class ResourceManager;
    };

    using MaterialInstanceHandle = ResourceHandle<MaterialInstanceResource>;

    /** @brief Result of creating a published runtime material instance. */
    struct MaterialInstanceCreateResult
    {
        MaterialInstanceHandle instance;
        AssetKey assetKey;
        MaterialInstanceMutationReceipt receipt;
    };
} // namespace RVX::Resource
