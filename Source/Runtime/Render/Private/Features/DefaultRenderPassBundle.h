#pragma once

#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Renderer/RenderPassRegistry.h"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>

namespace RVX
{
    class ClusteredLighting;
    class DefaultRenderComposition;
    class DeferredLightingPass;
    class IShaderProgramProvider;
    class IRHIDevice;
    class LightManager;
    class MaterialSystem;
    class PipelineCache;
    class RayTracingSceneManager;
    class RenderResourceRegistry;
    class RenderSubmissionTracker;
    class ResourceViewCache;
    struct RenderFeatureSnapshot;

    /**
     * @brief Construction-only dependencies for the built-in raster pass set.
     *
     * The bundle borrows renderer services whose lifetime is owned by the
     * enclosing SceneRenderer. Pass ownership itself never escapes the
     * composition-owned bundle.
     */
    struct DefaultRenderPassBundleDependencies
    {
        IRHIDevice* device = nullptr;
        PipelineCache* pipelineCache = nullptr;
        MaterialSystem* materialSystem = nullptr;
        LightManager* lightManager = nullptr;
        ClusteredLighting* clusteredLighting = nullptr;
        ResourceViewCache* resourceViewCache = nullptr;
        RenderResourceRegistry* resourceRegistry = nullptr;
        RayTracingSceneManager* rayTracingScene = nullptr;
        RenderSubmissionTracker* submissionTracker = nullptr;
        IShaderProgramProvider* shaderProgramProvider = nullptr;
        const RenderFeatureSnapshot* featureSnapshot = nullptr;
        ShadowPassConfig shadowConfig{};
        bool reverseZ = false;
    };

    /**
     * @brief Move-only owner and lookup boundary for one built-in pass set.
     *
     * Feature adapters borrow this stable bundle and resolve a pass only for
     * the duration of an operation. They never retain registry or pass
     * pointers, which makes pass replacement and teardown atomic at the
     * composition boundary.
     */
    class DefaultRenderPassBundle final
    {
    public:
        DefaultRenderPassBundle();
        ~DefaultRenderPassBundle();

        DefaultRenderPassBundle(const DefaultRenderPassBundle&) = delete;
        DefaultRenderPassBundle& operator=(const DefaultRenderPassBundle&) = delete;
        DefaultRenderPassBundle(DefaultRenderPassBundle&&) noexcept;
        DefaultRenderPassBundle& operator=(DefaultRenderPassBundle&&) noexcept;

        [[nodiscard]] RenderPassRegistry& GetRegistry() noexcept;
        [[nodiscard]] const RenderPassRegistry& GetRegistry() const noexcept;

        template <typename T>
        [[nodiscard]] T* FindPass() noexcept
        {
            if constexpr (std::is_same_v<T, DeferredLightingPass>)
            {
                return m_deferredLighting.get();
            }
            else
            {
                for (const std::unique_ptr<IRenderPass>& pass : GetPassStorage())
                {
                    if (T* const typed = dynamic_cast<T*>(pass.get()))
                    {
                        return typed;
                    }
                }
                return nullptr;
            }
        }

        template <typename T>
        [[nodiscard]] const T* FindPass() const noexcept
        {
            if constexpr (std::is_same_v<T, DeferredLightingPass>)
            {
                return m_deferredLighting.get();
            }
            else
            {
                for (const std::unique_ptr<IRenderPass>& pass : GetPassStorage())
                {
                    if (const T* const typed =
                            dynamic_cast<const T*>(pass.get()))
                    {
                        return typed;
                    }
                }
                return nullptr;
            }
        }

        void AddPass(std::unique_ptr<IRenderPass> pass, IRHIDevice* device);
        [[nodiscard]] bool RemovePass(const char* name);
        void Clear() noexcept;
        [[nodiscard]] size_t GetPassCount() const noexcept;

    private:
        friend class DefaultRenderComposition;
        friend std::unique_ptr<DefaultRenderPassBundle>
            CreateDefaultRenderPassBundle(
                const DefaultRenderPassBundleDependencies& dependencies);

        /**
         * Prepend already initialized externally registered passes without
         * replaying their OnAdd lifecycle. This is used exactly once when the
         * default composition is installed after pre-initialize AddPass calls.
         */
        void PrependOwnedPassesFrom(DefaultRenderPassBundle& source);

        [[nodiscard]] std::vector<std::unique_ptr<IRenderPass>>&
            GetPassStorage() noexcept;
        [[nodiscard]] const std::vector<std::unique_ptr<IRenderPass>>&
            GetPassStorage() const noexcept;

        std::unique_ptr<RenderPassRegistry> m_registry;
        std::unique_ptr<DeferredLightingPass> m_deferredLighting;
    };

    /** Construct the complete built-in pass set in its sole ownership site. */
    [[nodiscard]] std::unique_ptr<DefaultRenderPassBundle>
        CreateDefaultRenderPassBundle(
            const DefaultRenderPassBundleDependencies& dependencies);
} // namespace RVX
