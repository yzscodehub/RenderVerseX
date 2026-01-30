#include "MetalQuery.h"
#include "Core/Log.h"

namespace RVX
{
    MetalQueryPool::MetalQueryPool(id<MTLDevice> device, const RHIQueryPoolDesc& desc)
        : m_device(device)
        , m_type(desc.type)
        , m_count(desc.count)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Check device capabilities
        switch (m_type)
        {
            case RHIQueryType::Timestamp:
            {
                // Timestamp queries require counter sampling support (macOS 10.15+ / iOS 14+)
                if (@available(macOS 10.15, iOS 14.0, *))
                {
                    // Check if the device supports timestamp counters
                    if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
                    {
                        RVX_RHI_WARN("Metal: Device does not support timestamp queries");
                        m_supported = false;
                        return;
                    }

                    // Get the timestamp counter set
                    id<MTLCounterSet> timestampCounterSet = nil;
                    for (id<MTLCounterSet> set in device.counterSets)
                    {
                        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
                        {
                            timestampCounterSet = set;
                            break;
                        }
                    }

                    if (!timestampCounterSet)
                    {
                        RVX_RHI_WARN("Metal: Timestamp counter set not available");
                        m_supported = false;
                        return;
                    }

                    // Create counter sample buffer descriptor
                    MTLCounterSampleBufferDescriptor* counterDesc = [[MTLCounterSampleBufferDescriptor alloc] init];
                    counterDesc.counterSet = timestampCounterSet;
                    counterDesc.sampleCount = m_count;
                    counterDesc.storageMode = MTLStorageModePrivate;
                    counterDesc.label = desc.debugName ? 
                        [NSString stringWithUTF8String:desc.debugName] : @"TimestampQueryPool";

                    NSError* error = nil;
                    m_counterSampleBuffer = [device newCounterSampleBufferWithDescriptor:counterDesc 
                                                                                   error:&error];
                    
                    if (error || !m_counterSampleBuffer)
                    {
                        RVX_RHI_ERROR("Metal: Failed to create counter sample buffer: {}", 
                                     error ? [[error localizedDescription] UTF8String] : "unknown");
                        m_supported = false;
                        return;
                    }

                    // Get timestamp frequency (GPU ticks per second)
                    // Metal timestamps are in nanoseconds, so frequency is 1 GHz
                    m_timestampFrequency = 1000000000;
                    
                    m_supported = true;
                    RVX_RHI_DEBUG("Metal: Created timestamp query pool with {} queries", m_count);
                }
                else
                {
                    RVX_RHI_WARN("Metal: Timestamp queries require macOS 10.15+ or iOS 14+");
                    m_supported = false;
                    return;
                }
                break;
            }

            case RHIQueryType::Occlusion:
            case RHIQueryType::BinaryOcclusion:
            {
                // Occlusion queries use visibility result buffer
                // Each query needs 8 bytes for the result
                NSUInteger bufferSize = m_count * sizeof(uint64);
                
                m_visibilityBuffer = [device newBufferWithLength:bufferSize 
                                                         options:MTLResourceStorageModeShared];
                
                if (!m_visibilityBuffer)
                {
                    RVX_RHI_ERROR("Metal: Failed to create visibility buffer");
                    m_supported = false;
                    return;
                }

                if (desc.debugName)
                {
                    m_visibilityBuffer.label = [NSString stringWithUTF8String:desc.debugName];
                }

                m_supported = true;
                RVX_RHI_DEBUG("Metal: Created occlusion query pool with {} queries", m_count);
                break;
            }

            case RHIQueryType::PipelineStatistics:
            {
                RVX_RHI_WARN("Metal: Pipeline statistics queries not supported");
                m_supported = false;
                return;
            }
        }

        // Create result buffer for CPU access
        if (m_supported)
        {
            NSUInteger resultSize = m_count * sizeof(uint64);
            m_resultBuffer = [device newBufferWithLength:resultSize 
                                                 options:MTLResourceStorageModeShared];
            
            if (m_resultBuffer && desc.debugName)
            {
                m_resultBuffer.label = [NSString stringWithFormat:@"%s_Results", desc.debugName];
            }
        }
    }

    MetalQueryPool::~MetalQueryPool()
    {
        m_counterSampleBuffer = nil;
        m_visibilityBuffer = nil;
        m_resultBuffer = nil;
    }

    NSUInteger MetalQueryPool::GetResultOffset(uint32 index) const
    {
        if (index >= m_count)
        {
            RVX_RHI_WARN("Metal: Query index {} out of range (max {})", index, m_count);
            return 0;
        }
        return index * sizeof(uint64);
    }

    void MetalQueryPool::ResolveResults(uint32 firstQuery, uint32 queryCount)
    {
        if (!m_supported || !m_resultBuffer)
        {
            return;
        }

        switch (m_type)
        {
            case RHIQueryType::Timestamp:
            {
                if (@available(macOS 10.15, iOS 14.0, *))
                {
                    if (m_counterSampleBuffer)
                    {
                        // Resolve counter sample buffer to result buffer
                        NSRange range = NSMakeRange(firstQuery, queryCount);
                        
                        NSError* error = nil;
                        NSData* data = [m_counterSampleBuffer resolveCounterRange:range];
                        
                        if (data && data.length > 0)
                        {
                            // Parse the counter data
                            // The format depends on the counter set, but for timestamps
                            // we expect MTLCounterResultTimestamp structures
                            const MTLCounterResultTimestamp* timestamps = 
                                static_cast<const MTLCounterResultTimestamp*>(data.bytes);
                            
                            uint64* results = static_cast<uint64*>(m_resultBuffer.contents);
                            
                            NSUInteger count = data.length / sizeof(MTLCounterResultTimestamp);
                            for (NSUInteger i = 0; i < count && i < queryCount; ++i)
                            {
                                results[firstQuery + i] = timestamps[i].timestamp;
                            }
                        }
                    }
                }
                break;
            }

            case RHIQueryType::Occlusion:
            case RHIQueryType::BinaryOcclusion:
            {
                if (m_visibilityBuffer)
                {
                    // Copy visibility results to result buffer
                    const uint64* visibility = static_cast<const uint64*>(m_visibilityBuffer.contents);
                    uint64* results = static_cast<uint64*>(m_resultBuffer.contents);
                    
                    for (uint32 i = 0; i < queryCount; ++i)
                    {
                        if (m_type == RHIQueryType::BinaryOcclusion)
                        {
                            // Convert to binary (0 or 1)
                            results[firstQuery + i] = visibility[firstQuery + i] > 0 ? 1 : 0;
                        }
                        else
                        {
                            results[firstQuery + i] = visibility[firstQuery + i];
                        }
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    uint64 MetalQueryPool::GetResult(uint32 index) const
    {
        if (!m_supported || !m_resultBuffer || index >= m_count)
        {
            return 0;
        }

        const uint64* results = static_cast<const uint64*>(m_resultBuffer.contents);
        return results[index];
    }

} // namespace RVX
