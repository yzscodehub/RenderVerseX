#pragma once

/**
 * @file PostProcessStack.h
 * @brief Post-processing effect chain manager
 */

#include "Core/Types.h"
#include "Render/Graph/RenderGraph.h"
#include <vector>
#include <memory>
#include <functional>

namespace RVX
{
    class RenderGraph;
    class IRHIDevice;
    class RHICommandContext;

    /**
     * @brief Post-process settings accessible by all effects
     */
    struct PostProcessSettings
    {
        // Tone mapping
        bool enableToneMapping = true;
        float exposure = 1.0f;
        float gamma = 2.2f;

        // Bloom
        bool enableBloom = true;
        float bloomThreshold = 1.0f;
        float bloomIntensity = 1.0f;
        float bloomRadius = 0.5f;

        // Anti-aliasing
        bool enableFXAA = true;
        float fxaaQuality = 0.75f;

        // Color grading
        float contrast = 1.0f;
        float saturation = 1.0f;
        float brightness = 0.0f;

        // Vignette
        bool enableVignette = false;
        float vignetteIntensity = 0.3f;
        float vignetteRadius = 0.8f;
    };

    /**
     * @brief Base interface for post-process effects
     */
    class IPostProcessPass
    {
    public:
        virtual ~IPostProcessPass() = default;

        /**
         * @brief Get the effect name
         */
        virtual const char* GetName() const = 0;

        /**
         * @brief Get execution priority (lower runs first)
         */
        virtual int32 GetPriority() const { return 0; }

        /**
         * @brief Check if this effect is enabled
         */
        virtual bool IsEnabled() const { return m_enabled; }

        /**
         * @brief Enable/disable the effect
         */
        virtual void SetEnabled(bool enabled) { m_enabled = enabled; }

        /**
         * @brief Configure the effect based on settings
         */
        virtual void Configure(const PostProcessSettings& settings) = 0;

        /**
         * @brief Add the pass to the render graph
         * @param graph The render graph
         * @param input Input texture handle
         * @param output Output texture handle
         */
        virtual void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) = 0;

    protected:
        bool m_enabled = true;
    };

    /**
     * @brief Manages post-processing effect chain
     * 
     * PostProcessStack handles:
     * - Effect ordering by priority
     * - Ping-pong buffer management
     * - Integration with RenderGraph
     */
    class PostProcessStack
    {
    public:
        PostProcessStack() = default;
        ~PostProcessStack();

        // Non-copyable
        PostProcessStack(const PostProcessStack&) = delete;
        PostProcessStack& operator=(const PostProcessStack&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Effect Management
        // =========================================================================

        /**
         * @brief Add a post-process effect
         */
        template<typename T, typename... Args>
        T* AddEffect(Args&&... args)
        {
            auto effect = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = effect.get();
            m_effects.push_back(std::move(effect));
            SortEffects();
            return ptr;
        }

        /**
         * @brief Get an effect by type
         */
        template<typename T>
        T* GetEffect()
        {
            for (auto& effect : m_effects)
            {
                if (auto* typed = dynamic_cast<T*>(effect.get()))
                    return typed;
            }
            return nullptr;
        }

        /**
         * @brief Remove all effects
         */
        void ClearEffects();

        // =========================================================================
        // Execution
        // =========================================================================

        /**
         * @brief Apply settings to all effects
         */
        void ApplySettings(const PostProcessSettings& settings);

        /**
         * @brief Execute the post-processing chain
         * @param graph The render graph
         * @param sceneColor Input scene color
         * @param output Final output target
         */
        void Execute(RenderGraph& graph, RGTextureHandle sceneColor, RGTextureHandle output);

        /**
         * @brief Get current settings
         */
        PostProcessSettings& GetSettings() { return m_settings; }
        const PostProcessSettings& GetSettings() const { return m_settings; }

    private:
        void SortEffects();

        IRHIDevice* m_device = nullptr;
        PostProcessSettings m_settings;
        std::vector<std::unique_ptr<IPostProcessPass>> m_effects;
    };

} // namespace RVX
