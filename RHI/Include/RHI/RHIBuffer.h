#pragma once

#include "RHI/RHIResources.h"

namespace RVX
{
    // =============================================================================
    // Buffer Description
    // =============================================================================
    struct RHIBufferDesc
    {
        uint64 size = 0;
        RHIBufferUsage usage = RHIBufferUsage::None;
        RHIMemoryType memoryType = RHIMemoryType::Default;
        uint32 stride = 0;  // For structured buffers
        const char* debugName = nullptr;

        // Builder pattern helpers
        RHIBufferDesc& SetSize(uint64 s) { size = s; return *this; }
        RHIBufferDesc& SetUsage(RHIBufferUsage u) { usage = u; return *this; }
        RHIBufferDesc& SetMemoryType(RHIMemoryType m) { memoryType = m; return *this; }
        RHIBufferDesc& SetStride(uint32 s) { stride = s; return *this; }
        RHIBufferDesc& SetDebugName(const char* n) { debugName = n; return *this; }
    };

    // =============================================================================
    // Buffer Interface
    // =============================================================================
    class RHIBuffer : public RHIResource
    {
    public:
        virtual ~RHIBuffer() = default;

        // Getters
        virtual uint64 GetSize() const = 0;
        virtual RHIBufferUsage GetUsage() const = 0;
        virtual RHIMemoryType GetMemoryType() const = 0;
        virtual uint32 GetStride() const = 0;

        // Mapping (for Upload/Readback buffers)
        virtual void* Map() = 0;
        virtual void Unmap() = 0;

        // Helper: Map, write, unmap
        template<typename T>
        void Upload(const T* data, uint64 count, uint64 offset = 0)
        {
            void* mapped = Map();
            if (mapped)
            {
                std::memcpy(static_cast<uint8*>(mapped) + offset, data, count * sizeof(T));
                Unmap();
            }
        }
    };

} // namespace RVX
