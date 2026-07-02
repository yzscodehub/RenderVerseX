#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIBuffer.h"

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

    inline RHIQueryValidationResult ValidateRHIQueryPoolDesc(const RHIQueryPoolDesc& desc)
    {
        if (!IsRHIQueryTypeValid(desc.type))
        {
            return RHIQueryValidationFail("query pool type is invalid");
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

    // =============================================================================
    // Query Pool Interface
    // =============================================================================
    class RHIQueryPool : public RHIResource
    {
    public:
        virtual ~RHIQueryPool() = default;

        virtual RHIQueryType GetType() const = 0;
        virtual uint32 GetCount() const = 0;
        
        /**
         * @brief Get the GPU timestamp frequency
         * @return Ticks per second, or 0 if not a timestamp query
         */
        virtual uint64 GetTimestampFrequency() const = 0;
    };

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

    using RHIQueryPoolRef = Ref<RHIQueryPool>;

} // namespace RVX
