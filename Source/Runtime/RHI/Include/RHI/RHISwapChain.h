#pragma once

#include "RHI/RHIPresentation.h"
#include "RHI/RHIResources.h"

namespace RVX
{
    // =============================================================================
    // SwapChain Description
    // =============================================================================
    struct RHISwapChainDesc
    {
        HAL::NativeSurfaceDesc surface;
        RHISwapChainConfig config;
        uint32 bufferCount = 3;
        const char* debugName = nullptr;
    };

    // =============================================================================
    // SwapChain Interface
    // =============================================================================
    class RHISwapChain : public RHIResource
    {
    public:
        virtual ~RHISwapChain() = default;

        // Get current back buffer
        virtual RHITexture* GetCurrentBackBuffer() = 0;
        virtual RHITextureView* GetCurrentBackBufferView() = 0;
        virtual uint32 GetCurrentBackBufferIndex() const = 0;

        // Present and acquire next image
        virtual void Present() = 0;

        /**
         * @brief Whether this swap chain currently owns an acquired image that
         * must be retired even when the engine frame is abandoned.
         *
         * Backends without an explicit acquire lifetime keep the default false
         * result. This is presentation ownership, not evidence that an engine
         * frame rendered or presented successfully.
         */
        [[nodiscard]] virtual bool HasAcquiredPresentationImage() const noexcept
        {
            return false;
        }

        /**
         * @brief Record backend-specific discard work before retiring an
         * acquired image from an abandoned frame.
         *
         * The caller submits @p context on Graphics and then calls Present to
         * release the acquisition. Returning false is a terminal ownership
         * failure; callers must not silently clear backend acquire state.
         */
        virtual bool RecordAbortPresentationImageRetirement(
            RHICommandContext& context) noexcept
        {
            static_cast<void>(context);
            return false;
        }

        // Resize
        virtual void Resize(uint32 width, uint32 height) = 0;

        // Properties
        virtual uint32 GetWidth() const = 0;
        virtual uint32 GetHeight() const = 0;
        virtual RHIFormat GetFormat() const = 0;
        virtual uint32 GetBufferCount() const = 0;
    };

} // namespace RVX
