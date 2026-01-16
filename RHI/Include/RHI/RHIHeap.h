#pragma once

#include "RHI/RHIResources.h"

namespace RVX
{
    // =============================================================================
    // Heap Types (for Memory Aliasing / Placed Resources)
    // =============================================================================
    enum class RHIHeapType
    {
        Default,    // GPU-only memory (VRAM)
        Upload,     // CPU -> GPU staging
        Readback,   // GPU -> CPU readback
    };

    enum class RHIHeapFlags : uint32
    {
        None = 0,
        AllowTextures = 1 << 0,
        AllowBuffers = 1 << 1,
        AllowRenderTargets = 1 << 2,
        AllowDepthStencil = 1 << 3,
        AllowUnorderedAccess = 1 << 4,
        AllowAll = AllowTextures | AllowBuffers | AllowRenderTargets | AllowDepthStencil | AllowUnorderedAccess,
    };

    inline RHIHeapFlags operator|(RHIHeapFlags a, RHIHeapFlags b)
    {
        return static_cast<RHIHeapFlags>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline RHIHeapFlags operator&(RHIHeapFlags a, RHIHeapFlags b)
    {
        return static_cast<RHIHeapFlags>(static_cast<uint32>(a) & static_cast<uint32>(b));
    }

    inline bool HasFlag(RHIHeapFlags flags, RHIHeapFlags flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    // =============================================================================
    // Heap Description
    // =============================================================================
    struct RHIHeapDesc
    {
        uint64 size = 0;                            // Total heap size in bytes
        RHIHeapType type = RHIHeapType::Default;    // Memory type
        RHIHeapFlags flags = RHIHeapFlags::AllowAll; // Allowed resource types
        uint64 alignment = 0;                       // 0 = use default alignment (64KB for textures, 256B for buffers)
        const char* debugName = nullptr;
    };

    // =============================================================================
    // RHI Heap - Base class for memory heaps used by placed resources
    // =============================================================================
    class RHIHeap : public RHIResource
    {
    public:
        virtual ~RHIHeap() = default;

        virtual uint64 GetSize() const = 0;
        virtual RHIHeapType GetType() const = 0;
        virtual RHIHeapFlags GetFlags() const = 0;
    };

    using RHIHeapRef = Ref<RHIHeap>;

} // namespace RVX
