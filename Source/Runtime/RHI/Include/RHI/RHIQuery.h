#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIBuffer.h"

#include <limits>

namespace RVX
{
    // =============================================================================
    // Query Types
    // =============================================================================
    enum class RHIQueryType : uint8
    {
        Timestamp,           // GPU timestamp for profiling
        Occlusion,           // Number of samples that passed depth/stencil tests
        BinaryOcclusion,     // Boolean: any samples passed?
        PipelineStatistics,  // Detailed pipeline statistics
    };

    // =============================================================================
    // Query Pool Description
    // =============================================================================
    struct RHIQueryPoolDesc
    {
        RHIQueryType type = RHIQueryType::Timestamp;
        /**
         * @brief Queue that owns query writes and resolution.
         *
         * The current timestamp contract is intentionally Graphics-only.  This
         * makes a timestamp frequency an unambiguous projection of the queue
         * that produced the ticks instead of a device-wide approximation.
         */
        RHICommandQueueType queueType = RHICommandQueueType::Graphics;
        uint32 count = 64;  // Number of queries in the pool
        const char* debugName = nullptr;
    };

    // =============================================================================
    // Query Validation Helpers
    // =============================================================================
    struct RHIQueryValidationResult
    {
        bool valid = true;
        const char* message = "";

        explicit operator bool() const { return valid; }
    };

    inline RHIQueryValidationResult RHIQueryValidationPass()
    {
        return {};
    }

    inline RHIQueryValidationResult RHIQueryValidationFail(const char* message)
    {
        return {false, message};
    }

    inline bool IsRHIQueryTypeValid(RHIQueryType type)
    {
        return type == RHIQueryType::Timestamp ||
               type == RHIQueryType::Occlusion ||
               type == RHIQueryType::BinaryOcclusion ||
               type == RHIQueryType::PipelineStatistics;
    }

    inline bool IsRHICommandQueueTypeValid(RHICommandQueueType type)
    {
        return type == RHICommandQueueType::Graphics ||
               type == RHICommandQueueType::Compute ||
               type == RHICommandQueueType::Copy;
    }

    inline RHIQueryValidationResult ValidateRHIQueryPoolDesc(const RHIQueryPoolDesc& desc)
    {
        if (!IsRHIQueryTypeValid(desc.type))
        {
            return RHIQueryValidationFail("query pool type is invalid");
        }

        if (!IsRHICommandQueueTypeValid(desc.queueType))
        {
            return RHIQueryValidationFail("query pool queue type is invalid");
        }

        if (desc.type == RHIQueryType::Timestamp &&
            desc.queueType != RHICommandQueueType::Graphics)
        {
            return RHIQueryValidationFail("timestamp query pools currently require the Graphics queue");
        }

        if (desc.count == 0)
        {
            return RHIQueryValidationFail("query pool count must be greater than zero");
        }

        return RHIQueryValidationPass();
    }

    // =============================================================================
    // Pipeline Statistics Result
    // =============================================================================
    struct RHIPipelineStatistics
    {
        uint64 inputAssemblerVertices = 0;
        uint64 inputAssemblerPrimitives = 0;
        uint64 vertexShaderInvocations = 0;
        uint64 geometryShaderInvocations = 0;
        uint64 geometryShaderPrimitives = 0;
        uint64 clippingInvocations = 0;
        uint64 clippingPrimitives = 0;
        uint64 pixelShaderInvocations = 0;
        uint64 hullShaderInvocations = 0;
        uint64 domainShaderInvocations = 0;
        uint64 computeShaderInvocations = 0;
    };

    inline uint64 GetRHIQueryResultSizeBytes(RHIQueryType type)
    {
        switch (type)
        {
            case RHIQueryType::Timestamp:
            case RHIQueryType::Occlusion:
            case RHIQueryType::BinaryOcclusion:
                return sizeof(uint64);
            case RHIQueryType::PipelineStatistics:
                return sizeof(RHIPipelineStatistics);
            default:
                return 0;
        }
    }

    // =============================================================================
    // Query Pool Interface
    // =============================================================================
    class RHIQueryPool : public RHIResource
    {
    public:
        RHIQueryPool(
            RHICommandQueueType queueType = RHICommandQueueType::Graphics,
            uint8 timestampValidBits = 0)
            : m_queueType(queueType)
            , m_timestampValidBits(timestampValidBits)
        {
        }

        virtual ~RHIQueryPool() = default;

        virtual RHIQueryType GetType() const = 0;
        virtual uint32 GetCount() const = 0;

        /** @brief Queue that owns every command issued against this pool. */
        RHICommandQueueType GetQueueType() const { return m_queueType; }

        /**
         * @brief Number of valid low-order bits in a timestamp result.
         *
         * Zero means that the pool does not represent timestamps.  A timestamp
         * pool must expose a value in [1, 64].
         */
        uint8 GetTimestampValidBits() const { return m_timestampValidBits; }
        
        /**
         * @brief Get the GPU timestamp frequency
         * @return Ticks per second, or 0 if not a timestamp query
         */
        virtual uint64 GetTimestampFrequency() const = 0;

    private:
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;
        uint8 m_timestampValidBits = 0;
    };

    inline RHIQueryValidationResult ValidateRHIQueryPoolMetadata(
        const RHIQueryPool& pool,
        RHIQueryType expectedType,
        RHICommandQueueType expectedQueueType)
    {
        if (pool.GetType() != expectedType)
        {
            return RHIQueryValidationFail("query pool type does not match the requested operation");
        }

        if (pool.GetQueueType() != expectedQueueType)
        {
            return RHIQueryValidationFail("query pool queue does not match the command context queue");
        }

        if (pool.GetType() == RHIQueryType::Timestamp)
        {
            const uint8 validBits = pool.GetTimestampValidBits();
            if (validBits == 0 || validBits > 64)
            {
                return RHIQueryValidationFail("timestamp query pool valid-bit count must be in [1, 64]");
            }

            if (pool.GetQueueType() != RHICommandQueueType::Graphics)
            {
                return RHIQueryValidationFail("timestamp query pools currently require the Graphics queue");
            }

            if (pool.GetTimestampFrequency() == 0)
            {
                return RHIQueryValidationFail("timestamp query pool frequency must be non-zero");
            }
        }

        return RHIQueryValidationPass();
    }

    inline RHIQueryValidationResult ValidateRHIQueryRange(
        const RHIQueryPool& pool,
        uint32 firstQuery,
        uint32 queryCount)
    {
        if (queryCount == 0)
        {
            return RHIQueryValidationFail("query range count must be greater than zero");
        }

        if (firstQuery >= pool.GetCount() || queryCount > pool.GetCount() - firstQuery)
        {
            return RHIQueryValidationFail("query range exceeds query pool bounds");
        }

        return RHIQueryValidationPass();
    }

    inline RHIQueryValidationResult ValidateRHIQueryResolveDestination(
        const RHIQueryPool& pool,
        uint32 firstQuery,
        uint32 queryCount,
        const RHIBuffer& destBuffer,
        uint64 destOffset)
    {
        const RHIQueryValidationResult rangeValidation =
            ValidateRHIQueryRange(pool, firstQuery, queryCount);
        if (!rangeValidation)
        {
            return rangeValidation;
        }

        const uint64 resultSize = GetRHIQueryResultSizeBytes(pool.GetType());
        if (resultSize == 0)
        {
            return RHIQueryValidationFail("query pool type has no defined resolve result size");
        }

        if ((destOffset % alignof(uint64)) != 0)
        {
            return RHIQueryValidationFail("query resolve destination offset must be 8-byte aligned");
        }

        if (queryCount > (std::numeric_limits<uint64>::max() - destOffset) / resultSize)
        {
            return RHIQueryValidationFail("query resolve destination range overflowed");
        }

        const uint64 requiredSize = static_cast<uint64>(queryCount) * resultSize;
        if (destOffset > destBuffer.GetSize() ||
            requiredSize > destBuffer.GetSize() - destOffset)
        {
            return RHIQueryValidationFail("query resolve destination range exceeds buffer bounds");
        }

        if (!HasFlag(destBuffer.GetUsage(), RHIBufferUsage::CopyDst))
        {
            return RHIQueryValidationFail("query resolve destination buffer requires CopyDst usage");
        }

        return RHIQueryValidationPass();
    }

    /**
     * @brief Computes end - begin in an N-bit timestamp domain.
     *
     * Timestamp counter rollover is intentional.  The 64-bit path relies on
     * C++ unsigned subtraction, which is exactly the required modulo-2^64
     * operation and avoids an invalid 1ULL << 64 shift.
     */
    inline uint64 CalculateRHITimestampElapsedDelta(
        uint64 beginTimestamp,
        uint64 endTimestamp,
        uint8 validBits)
    {
        if (validBits == 0 || validBits > 64)
        {
            return 0;
        }

        const uint64 delta = endTimestamp - beginTimestamp;
        if (validBits == 64)
        {
            return delta;
        }

        const uint64 mask = (uint64{1} << validBits) - 1;
        return delta & mask;
    }

    using RHIQueryPoolRef = Ref<RHIQueryPool>;

} // namespace RVX
