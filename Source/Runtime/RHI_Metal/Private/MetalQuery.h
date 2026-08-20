#pragma once

#include "MetalCommon.h"
#include "RHI/RHIQuery.h"
#include <vector>

namespace RVX
{
    class MetalDevice;

    // =============================================================================
    // Metal Query Pool
    // Uses MTLCounterSampleBuffer for timestamp and visibility queries
    // =============================================================================
    class MetalQueryPool : public RHIQueryPool
    {
    public:
        MetalQueryPool(id<MTLDevice> device, const RHIQueryPoolDesc& desc);
        ~MetalQueryPool() override;

        // RHIQueryPool interface
        RHIQueryType GetType() const override { return m_type; }
        uint32 GetCount() const override { return m_count; }
        uint64 GetTimestampFrequency() const override { return m_timestampFrequency; }

        // Metal specific accessors
        id<MTLBuffer> GetResultBuffer() const { return m_resultBuffer; }
        
        // Get the visibility result buffer for occlusion queries
        id<MTLBuffer> GetVisibilityBuffer() const { return m_visibilityBuffer; }
        NSUInteger GetResultOffset(uint32 index) const;

        // For timestamp queries (counter sample buffer)
        id<MTLCounterSampleBuffer> GetCounterSampleBuffer() const { return m_counterSampleBuffer; }

        // Check if this device supports the query type
        bool IsSupported() const { return m_supported; }

        // Resolve query results to CPU-accessible buffer
        void ResolveResults(uint32 firstQuery, uint32 queryCount);
        
        // Get query result (after resolve)
        uint64 GetResult(uint32 index) const;

    private:
        id<MTLDevice> m_device = nil;
        RHIQueryType m_type = RHIQueryType::Timestamp;
        uint32 m_count = 0;
        uint64 m_timestampFrequency = 0;
        bool m_supported = false;

        // Buffer to store query results
        id<MTLBuffer> m_resultBuffer = nil;
        
        // For occlusion queries: visibility result buffer
        id<MTLBuffer> m_visibilityBuffer = nil;
        
        // For timestamp queries: counter sample buffer (macOS 10.15+ / iOS 14+)
        id<MTLCounterSampleBuffer> m_counterSampleBuffer = nil;
    };

} // namespace RVX
