#pragma once

#include "Features/DefaultRenderPassBundle.h"
#include "Render/Features/RenderFeatureRegistry.h"

#include <memory>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class PipelineCache;
    class PostProcessStack;
    class ResourceViewCache;
    class OpaqueSceneColorHistory;

    /**
     * @brief Private owner for the default renderer's passes and features.
     *
     * SceneRenderer owns exactly one composition and interacts with it through
     * transient lookups. The registry-owned feature objects and bundle-owned
     * pass objects therefore share one atomic lifetime boundary.
     */
    class DefaultRenderComposition final
    {
    public:
        DefaultRenderComposition();
        ~DefaultRenderComposition();

        DefaultRenderComposition(const DefaultRenderComposition&) = delete;
        DefaultRenderComposition& operator=(const DefaultRenderComposition&) = delete;

        [[nodiscard]] DefaultRenderPassBundle& GetPassBundle() noexcept;
        [[nodiscard]] const DefaultRenderPassBundle&
            GetPassBundle() const noexcept;
        /** Install the built-in set while preserving pre-initialize AddPass work. */
        void InstallDefaultPassBundle(
            std::unique_ptr<DefaultRenderPassBundle> passBundle);
        [[nodiscard]] bool HasInstalledDefaultPassBundle() const noexcept
        {
            return m_defaultPassBundleInstalled;
        }

        template <typename T>
        [[nodiscard]] T* FindPass() noexcept
        {
            return m_passBundle != nullptr
                ? m_passBundle->FindPass<T>()
                : nullptr;
        }

        template <typename T>
        [[nodiscard]] const T* FindPass() const noexcept
        {
            return m_passBundle != nullptr
                ? m_passBundle->FindPass<T>()
                : nullptr;
        }

        void BeginFeatureRegistration();
        [[nodiscard]] RenderFeatureRegistryCode DeclareImplementedRecipe(
            RenderPipelineRecipe recipe);
        [[nodiscard]] RenderFeatureRegistryCode RegisterFeature(
            std::unique_ptr<IRenderFeature> feature);
        [[nodiscard]] RenderFeatureRegistryCode SealFeatures();
        void ResetFeatures() noexcept;
        [[nodiscard]] RenderFeatureRegistry* GetFeatureRegistry() noexcept;
        [[nodiscard]] const RenderFeatureRegistry*
            GetFeatureRegistry() const noexcept;
        [[nodiscard]] size_t GetFeatureCount() const noexcept;

        template <typename T>
        [[nodiscard]] T* FindFeature() noexcept
        {
            for (IRenderFeature* const feature : m_featureIndex)
            {
                if (T* const typed = dynamic_cast<T*>(feature))
                {
                    return typed;
                }
            }
            return nullptr;
        }

        template <typename T>
        [[nodiscard]] const T* FindFeature() const noexcept
        {
            for (const IRenderFeature* const feature : m_featureIndex)
            {
                if (const T* const typed = dynamic_cast<const T*>(feature))
                {
                    return typed;
                }
            }
            return nullptr;
        }

        [[nodiscard]] bool InitializePostProcess(
            IRHIDevice* device,
            PipelineCache* pipelineCache,
            ResourceViewCache* resourceViewCache);
        [[nodiscard]] PostProcessStack* GetPostProcessStack() noexcept;
        [[nodiscard]] const PostProcessStack*
            GetPostProcessStack() const noexcept;
        OpaqueSceneColorHistory* GetOpaqueSceneColorHistory() noexcept
        { return m_opaqueSceneColorHistory.get(); }
        void Shutdown() noexcept;

    private:
        std::unique_ptr<DefaultRenderPassBundle> m_passBundle;
        std::unique_ptr<RenderFeatureRegistry> m_featureRegistry;
        std::vector<IRenderFeature*> m_featureIndex;
        std::unique_ptr<PostProcessStack> m_postProcessStack;
        std::unique_ptr<OpaqueSceneColorHistory> m_opaqueSceneColorHistory;
        bool m_defaultPassBundleInstalled = false;
    };
} // namespace RVX
