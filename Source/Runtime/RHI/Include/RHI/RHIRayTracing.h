#pragma once

/**
 * @file RHIRayTracing.h
 * @brief Cross-backend ray tracing resource and dispatch contracts
 */

#include "RHI/RHIResources.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIShader.h"

#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

namespace RVX
{
    // =============================================================================
    // Acceleration Structures
    // =============================================================================
    enum class RHIAccelerationStructureType : uint8
    {
        BottomLevel,
        TopLevel
    };

    enum class RHIRayTracingGeometryType : uint8
    {
        Triangles,
        AABBs
    };

    enum class RHIRayTracingGeometryFlags : uint32
    {
        None = 0,
        Opaque = 1 << 0,
        NoDuplicateAnyHitInvocation = 1 << 1,
    };

    inline RHIRayTracingGeometryFlags operator|(RHIRayTracingGeometryFlags a, RHIRayTracingGeometryFlags b)
    {
        return static_cast<RHIRayTracingGeometryFlags>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline bool HasFlag(RHIRayTracingGeometryFlags flags, RHIRayTracingGeometryFlags flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    inline constexpr uint32 RVX_RAY_TRACING_GEOMETRY_FLAGS_MASK =
        static_cast<uint32>(RHIRayTracingGeometryFlags::Opaque) |
        static_cast<uint32>(RHIRayTracingGeometryFlags::NoDuplicateAnyHitInvocation);

    inline bool AreRHIRayTracingGeometryFlagsValid(RHIRayTracingGeometryFlags flags)
    {
        return (static_cast<uint32>(flags) & ~RVX_RAY_TRACING_GEOMETRY_FLAGS_MASK) == 0;
    }

    enum class RHIAccelerationStructureBuildFlags : uint32
    {
        None = 0,
        AllowUpdate = 1 << 0,
        AllowCompaction = 1 << 1,
        PreferFastTrace = 1 << 2,
        PreferFastBuild = 1 << 3,
        MinimizeMemory = 1 << 4,
    };

    inline RHIAccelerationStructureBuildFlags operator|(RHIAccelerationStructureBuildFlags a,
                                                       RHIAccelerationStructureBuildFlags b)
    {
        return static_cast<RHIAccelerationStructureBuildFlags>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline bool HasFlag(RHIAccelerationStructureBuildFlags flags, RHIAccelerationStructureBuildFlags flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    inline constexpr uint32 RVX_RAY_TRACING_BUILD_FLAGS_MASK =
        static_cast<uint32>(RHIAccelerationStructureBuildFlags::AllowUpdate) |
        static_cast<uint32>(RHIAccelerationStructureBuildFlags::AllowCompaction) |
        static_cast<uint32>(RHIAccelerationStructureBuildFlags::PreferFastTrace) |
        static_cast<uint32>(RHIAccelerationStructureBuildFlags::PreferFastBuild) |
        static_cast<uint32>(RHIAccelerationStructureBuildFlags::MinimizeMemory);

    inline bool AreRHIAccelerationStructureBuildFlagsValid(RHIAccelerationStructureBuildFlags flags)
    {
        return (static_cast<uint32>(flags) & ~RVX_RAY_TRACING_BUILD_FLAGS_MASK) == 0;
    }

    enum class RHIRayTracingInstanceFlags : uint32
    {
        None = 0,
        TriangleCullDisable = 1 << 0,
        TriangleFrontCounterClockwise = 1 << 1,
        ForceOpaque = 1 << 2,
        ForceNoOpaque = 1 << 3,
    };

    inline RHIRayTracingInstanceFlags operator|(RHIRayTracingInstanceFlags a, RHIRayTracingInstanceFlags b)
    {
        return static_cast<RHIRayTracingInstanceFlags>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline bool HasFlag(RHIRayTracingInstanceFlags flags, RHIRayTracingInstanceFlags flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    inline constexpr uint32 RVX_RAY_TRACING_INSTANCE_FLAGS_MASK =
        static_cast<uint32>(RHIRayTracingInstanceFlags::TriangleCullDisable) |
        static_cast<uint32>(RHIRayTracingInstanceFlags::TriangleFrontCounterClockwise) |
        static_cast<uint32>(RHIRayTracingInstanceFlags::ForceOpaque) |
        static_cast<uint32>(RHIRayTracingInstanceFlags::ForceNoOpaque);

    inline bool AreRHIRayTracingInstanceFlagsValid(RHIRayTracingInstanceFlags flags)
    {
        return (static_cast<uint32>(flags) & ~RVX_RAY_TRACING_INSTANCE_FLAGS_MASK) == 0;
    }

    struct RHIRayTracingTrianglesDesc
    {
        RHIBuffer* vertexBuffer = nullptr;
        uint64 vertexOffset = 0;
        uint32 vertexStride = 0;
        RHIFormat vertexFormat = RHIFormat::RGB32_FLOAT;
        uint32 vertexCount = 0;

        RHIBuffer* indexBuffer = nullptr;
        uint64 indexOffset = 0;
        RHIFormat indexFormat = RHIFormat::R32_UINT;
        uint32 indexCount = 0;

        RHIBuffer* transformBuffer = nullptr; // Optional row-major 3x4 transform buffer.
        uint64 transformOffset = 0;
    };

    struct RHIRayTracingAABBDesc
    {
        RHIBuffer* aabbBuffer = nullptr;
        uint64 offset = 0;
        uint32 stride = 0;
        uint32 count = 0;
    };

    struct RHIRayTracingGeometryDesc
    {
        RHIRayTracingGeometryType type = RHIRayTracingGeometryType::Triangles;
        RHIRayTracingGeometryFlags flags = RHIRayTracingGeometryFlags::Opaque;
        RHIRayTracingTrianglesDesc triangles;
        RHIRayTracingAABBDesc aabbs;
    };

    struct RHIBottomLevelASDesc
    {
        std::vector<RHIRayTracingGeometryDesc> geometries;
        RHIAccelerationStructureBuildFlags buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
        const char* debugName = nullptr;
    };

    inline constexpr uint32 RVX_RAY_TRACING_INSTANCE_METADATA_MASK = 0x00FF'FFFFu;
    inline constexpr uint32 RVX_RAY_TRACING_INSTANCE_MASK_MASK = 0xFFu;
    inline constexpr uint32 RVX_RAY_TRACING_MAX_ATTRIBUTE_SIZE = 32;
    inline constexpr uint64 RVX_RAY_TRACING_MAX_RAY_COUNT = ~static_cast<uint64>(0);
    inline constexpr uint64 RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT = 256;
    inline constexpr uint64 RVX_RAY_TRACING_INSTANCE_DESC_ALIGNMENT = 16;

    inline bool IsRHIRayTracingAccelerationStructureSizeAligned(uint64 size)
    {
        return (size % RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT) == 0;
    }

    struct RHIRayTracingInstanceDesc
    {
        // Row-major 3x4 object-to-world transform, matching DXR/Vulkan instance memory layout.
        float transform[12] =
        {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f
        };
        RHIAccelerationStructure* bottomLevel = nullptr;
        uint32 instanceId = 0;
        uint32 instanceMask = 0xFF;
        uint32 instanceContributionToHitGroupIndex = 0;
        RHIRayTracingInstanceFlags flags = RHIRayTracingInstanceFlags::None;
    };

    /**
     * @brief GPU instance record consumed by TLAS builds.
     *
     * The layout intentionally matches both D3D12_RAYTRACING_INSTANCE_DESC and
     * VkAccelerationStructureInstanceKHR: 3x4 row-major transform, two packed
     * 24/8-bit metadata words, then a 64-bit BLAS GPU address/reference.
     */
    struct RHIRayTracingInstanceRecord
    {
        float transform[12] = {};
        uint32 instanceIdAndMask = 0;
        uint32 instanceContributionAndFlags = 0;
        uint64 accelerationStructureAddress = 0;
    };

    static_assert(sizeof(RHIRayTracingInstanceRecord) == 64,
                  "RHIRayTracingInstanceRecord must match native TLAS instance layout");

    inline RHIRayTracingInstanceRecord PackRHIRayTracingInstanceRecord(
        const RHIRayTracingInstanceDesc& desc,
        uint64 accelerationStructureAddress)
    {
        RHIRayTracingInstanceRecord record;
        for (uint32 i = 0; i < 12; ++i)
        {
            record.transform[i] = desc.transform[i];
        }

        record.instanceIdAndMask =
            (desc.instanceId & RVX_RAY_TRACING_INSTANCE_METADATA_MASK) |
            ((desc.instanceMask & RVX_RAY_TRACING_INSTANCE_MASK_MASK) << 24);
        record.instanceContributionAndFlags =
            (desc.instanceContributionToHitGroupIndex & RVX_RAY_TRACING_INSTANCE_METADATA_MASK) |
            ((static_cast<uint32>(desc.flags) & RVX_RAY_TRACING_INSTANCE_FLAGS_MASK) << 24);
        record.accelerationStructureAddress = accelerationStructureAddress;
        return record;
    }

    struct RHITopLevelASDesc
    {
        // Preferred backend-ready path: GPU buffer containing native instance records
        // (D3D12_RAYTRACING_INSTANCE_DESC / VkAccelerationStructureInstanceKHR-compatible data).
        RHIBuffer* instanceBuffer = nullptr;
        uint64 instanceOffset = 0;
        uint32 instanceCount = 0;

        // High-level CPU description for render-side scene builders. Backends should
        // consume instanceBuffer for command recording after the builder uploads it.
        std::vector<RHIRayTracingInstanceDesc> instances;
        RHIAccelerationStructureBuildFlags buildFlags = RHIAccelerationStructureBuildFlags::PreferFastTrace;
        const char* debugName = nullptr;

        uint32 GetInstanceCount() const
        {
            return instanceBuffer ? instanceCount : static_cast<uint32>(instances.size());
        }
    };

    struct RHIAccelerationStructureBuildSizes
    {
        uint64 accelerationStructureSize = 0;
        uint64 buildScratchSize = 0;
        uint64 updateScratchSize = 0;

        bool IsValid() const
        {
            return accelerationStructureSize > 0 &&
                   buildScratchSize > 0 &&
                   IsRHIRayTracingAccelerationStructureSizeAligned(accelerationStructureSize);
        }

        bool HasValidUpdateScratchSize() const
        {
            return IsValid() && updateScratchSize > 0;
        }
    };

    struct RHIAccelerationStructureDesc
    {
        RHIAccelerationStructureType type = RHIAccelerationStructureType::BottomLevel;
        uint64 size = 0;
        const char* debugName = nullptr;
    };

    class RHIAccelerationStructure : public RHIResource
    {
    public:
        virtual ~RHIAccelerationStructure() = default;

        virtual RHIAccelerationStructureType GetType() const = 0;
        virtual uint64 GetSize() const = 0;
        virtual uint64 GetGPUVirtualAddress() const = 0;
    };

    // =============================================================================
    // Ray Tracing Pipelines and Shader Tables
    // =============================================================================
    enum class RHIRayTracingShaderGroupType : uint8
    {
        General,
        TrianglesHitGroup,
        ProceduralHitGroup
    };

    struct RHIRayTracingShaderGroupDesc
    {
        RHIRayTracingShaderGroupType type = RHIRayTracingShaderGroupType::General;
        const char* exportName = nullptr;

        RHIShader* generalShader = nullptr;      // Ray generation, miss, or callable.
        RHIShader* closestHitShader = nullptr;
        RHIShader* anyHitShader = nullptr;
        RHIShader* intersectionShader = nullptr;
    };

    struct RHIRayTracingPipelineDesc
    {
        std::vector<RHIRayTracingShaderGroupDesc> shaderGroups;
        RHIPipelineLayout* pipelineLayout = nullptr;
        uint32 maxRecursionDepth = 1;
        uint32 maxPayloadSize = 32;
        uint32 maxAttributeSize = 8;
        const char* debugName = nullptr;
    };

    struct RHIShaderTableRecord
    {
        uint32 shaderGroupIndex = 0;
        const void* localRootData = nullptr;
        uint32 localRootDataSize = 0;
    };

    struct RHIShaderTableDesc
    {
        RHIPipelineRef rayTracingPipelineOwner;
        RHIPipeline* rayTracingPipeline = nullptr;
        std::vector<RHIShaderTableRecord> rayGenerationRecords;
        std::vector<RHIShaderTableRecord> missRecords;
        std::vector<RHIShaderTableRecord> hitGroupRecords;
        std::vector<RHIShaderTableRecord> callableRecords;
        const char* debugName = nullptr;

        RHIPipeline* GetRayTracingPipeline() const
        {
            return rayTracingPipelineOwner ? rayTracingPipelineOwner.Get() : rayTracingPipeline;
        }
    };

    class RHIShaderTable : public RHIResource
    {
    public:
        virtual ~RHIShaderTable() = default;

        virtual uint32 GetRayGenerationRecordCount() const = 0;
        virtual uint32 GetMissRecordCount() const = 0;
        virtual uint32 GetHitGroupRecordCount() const = 0;
        virtual uint32 GetCallableRecordCount() const = 0;
        virtual RHIPipeline* GetRayTracingPipeline() const = 0;
    };

    struct RHIDispatchRaysDesc
    {
        RHIShaderTable* shaderTable = nullptr;
        uint32 width = 0;
        uint32 height = 0;
        uint32 depth = 1;
    };

    // =============================================================================
    // Validation Helpers
    // =============================================================================
    struct RHIRayTracingValidationResult
    {
        bool valid = true;
        const char* message = "";

        explicit operator bool() const { return valid; }
    };

    inline RHIRayTracingValidationResult RHIRayTracingValidationPass()
    {
        return {};
    }

    inline RHIRayTracingValidationResult RHIRayTracingValidationFail(const char* message)
    {
        return {false, message};
    }

    inline RHIRayTracingValidationResult ValidateRHIAccelerationStructureDesc(
        const RHIAccelerationStructureDesc& desc)
    {
        if (desc.type != RHIAccelerationStructureType::BottomLevel &&
            desc.type != RHIAccelerationStructureType::TopLevel)
        {
            return RHIRayTracingValidationFail("acceleration structure type is invalid");
        }

        if (desc.size == 0)
        {
            return RHIRayTracingValidationFail("acceleration structure size must be non-zero");
        }

        if (!IsRHIRayTracingAccelerationStructureSizeAligned(desc.size))
        {
            return RHIRayTracingValidationFail("acceleration structure size must be 256-byte aligned");
        }

        return RHIRayTracingValidationPass();
    }

    inline bool IsRHIRayTracingShaderStage(RHIShaderStage stage)
    {
        const uint32 bits = static_cast<uint32>(stage);
        const uint32 rayTracingBits = static_cast<uint32>(RHIShaderStage::AllRayTracing);
        return bits != 0 && (bits & (bits - 1)) == 0 && (bits & rayTracingBits) != 0;
    }

    inline bool IsRHIRayTracingGeneralShaderStage(RHIShaderStage stage)
    {
        return stage == RHIShaderStage::RayGeneration ||
               stage == RHIShaderStage::Miss ||
               stage == RHIShaderStage::Callable;
    }

    inline const char* GetRHIRayTracingStageExportName(RHIShaderStage stage)
    {
        switch (stage)
        {
            case RHIShaderStage::RayGeneration: return "RayGen";
            case RHIShaderStage::Miss: return "Miss";
            case RHIShaderStage::ClosestHit: return "ClosestHit";
            case RHIShaderStage::AnyHit: return "AnyHit";
            case RHIShaderStage::Intersection: return "Intersection";
            case RHIShaderStage::Callable: return "Callable";
            default: return "Shader";
        }
    }

    inline std::string MakeRHIRayTracingDefaultGeneralExportName(RHIShaderStage stage, uint32 index)
    {
        return std::string(GetRHIRayTracingStageExportName(stage)) + "_" + std::to_string(index);
    }

    inline std::string MakeRHIRayTracingDefaultHitGroupExportName(uint32 index)
    {
        return "HitGroup_" + std::to_string(index);
    }

    inline RHIRayTracingValidationResult AddRHIRayTracingStateObjectExport(
        std::unordered_set<std::string>& exportNames,
        const std::string& exportName)
    {
        if (exportName.empty())
        {
            return RHIRayTracingValidationFail("ray tracing pipeline shader group export name cannot be empty");
        }

        if (!exportNames.insert(exportName).second)
        {
            return RHIRayTracingValidationFail("ray tracing pipeline shader exports must be unique");
        }

        return RHIRayTracingValidationPass();
    }

    inline bool IsRHIIndexFormatSupportedForRayTracing(RHIFormat format)
    {
        return format == RHIFormat::R16_UINT || format == RHIFormat::R32_UINT;
    }

    inline bool HasRHIRayTracingASInputUsage(const RHIBuffer& buffer)
    {
        const RHIBufferUsage usage = buffer.GetUsage();
        return HasFlag(usage, RHIBufferUsage::AccelerationStructureInput) &&
               HasFlag(usage, RHIBufferUsage::DeviceAddress);
    }

    inline bool IsRHIRayTracingBufferRangeValid(const RHIBuffer& buffer,
                                                uint64 offset,
                                                uint64 requiredBytes)
    {
        const uint64 bufferSize = buffer.GetSize();
        return offset <= bufferSize && requiredBytes <= bufferSize - offset;
    }

    inline bool TryGetRHIRayTracingStridedRangeSize(uint32 count,
                                                     uint32 stride,
                                                     uint32 recordSize,
                                                     uint64& rangeSize)
    {
        if (count == 0 || stride == 0 || recordSize == 0)
        {
            rangeSize = 0;
            return false;
        }

        const uint64 lastRecordOffset = static_cast<uint64>(count - 1) * static_cast<uint64>(stride);
        if (lastRecordOffset > RVX_RAY_TRACING_MAX_RAY_COUNT - static_cast<uint64>(recordSize))
        {
            return false;
        }

        rangeSize = lastRecordOffset + static_cast<uint64>(recordSize);
        return true;
    }

    inline bool TryMultiplyRHIRayTracingCount(uint64 lhs, uint64 rhs, uint64& result)
    {
        if (lhs == 0 || rhs == 0)
        {
            result = 0;
            return true;
        }

        if (lhs > RVX_RAY_TRACING_MAX_RAY_COUNT / rhs)
        {
            return false;
        }

        result = lhs * rhs;
        return true;
    }

    inline bool TryGetRHIRayTracingDispatchRayCount(uint32 width,
                                                     uint32 height,
                                                     uint32 depth,
                                                     uint64& rayCount)
    {
        uint64 widthHeight = 0;
        if (!TryMultiplyRHIRayTracingCount(static_cast<uint64>(width), static_cast<uint64>(height), widthHeight))
        {
            return false;
        }

        return TryMultiplyRHIRayTracingCount(widthHeight, static_cast<uint64>(depth), rayCount);
    }

    inline bool TryGetRHIRayTracingDispatchRayCount(const RHIDispatchRaysDesc& desc, uint64& rayCount)
    {
        return TryGetRHIRayTracingDispatchRayCount(desc.width, desc.height, desc.depth, rayCount);
    }

    inline bool IsRHIRayTracingInstanceTransformFinite(const RHIRayTracingInstanceDesc& desc)
    {
        for (float value : desc.transform)
        {
            if (!std::isfinite(value))
            {
                return false;
            }
        }

        return true;
    }

    inline RHIRayTracingValidationResult ValidateRHIRayTracingGeometryDesc(
        const RHIRayTracingGeometryDesc& desc)
    {
        if (!AreRHIRayTracingGeometryFlagsValid(desc.flags))
        {
            return RHIRayTracingValidationFail("ray tracing geometry flags contain unknown bits");
        }

        if (desc.type == RHIRayTracingGeometryType::Triangles)
        {
            if (!desc.triangles.vertexBuffer)
            {
                return RHIRayTracingValidationFail("triangle geometry requires a vertex buffer");
            }

            if (desc.triangles.vertexCount == 0 || desc.triangles.vertexStride == 0)
            {
                return RHIRayTracingValidationFail("triangle geometry requires vertices and a non-zero stride");
            }

            if (desc.triangles.vertexFormat == RHIFormat::Unknown)
            {
                return RHIRayTracingValidationFail("triangle geometry requires a known vertex format");
            }

            if (!HasRHIRayTracingASInputUsage(*desc.triangles.vertexBuffer))
            {
                return RHIRayTracingValidationFail(
                    "triangle geometry vertex buffer requires AccelerationStructureInput and DeviceAddress usage");
            }

            const uint32 vertexFormatSize = GetFormatBytesPerPixel(desc.triangles.vertexFormat);
            if (vertexFormatSize == 0)
            {
                return RHIRayTracingValidationFail("triangle geometry vertex format has no byte size");
            }

            if (desc.triangles.vertexStride < vertexFormatSize)
            {
                return RHIRayTracingValidationFail("triangle geometry vertex stride is smaller than the vertex format size");
            }

            uint64 vertexRangeBytes = 0;
            if (!TryGetRHIRayTracingStridedRangeSize(desc.triangles.vertexCount,
                                                     desc.triangles.vertexStride,
                                                     vertexFormatSize,
                                                     vertexRangeBytes) ||
                !IsRHIRayTracingBufferRangeValid(*desc.triangles.vertexBuffer,
                                                 desc.triangles.vertexOffset,
                                                 vertexRangeBytes))
            {
                return RHIRayTracingValidationFail("triangle geometry vertex range exceeds vertex buffer size");
            }

            if (desc.triangles.indexBuffer)
            {
                if (desc.triangles.indexCount == 0)
                {
                    return RHIRayTracingValidationFail("indexed triangle geometry requires indices");
                }

                if (!IsRHIIndexFormatSupportedForRayTracing(desc.triangles.indexFormat))
                {
                    return RHIRayTracingValidationFail("indexed triangle geometry uses an unsupported index format");
                }

                if (!HasRHIRayTracingASInputUsage(*desc.triangles.indexBuffer))
                {
                    return RHIRayTracingValidationFail(
                        "indexed triangle geometry index buffer requires AccelerationStructureInput and DeviceAddress usage");
                }

                const uint32 indexElementSize = desc.triangles.indexFormat == RHIFormat::R16_UINT ? 2u : 4u;
                const uint64 indexRangeBytes = static_cast<uint64>(desc.triangles.indexCount) * indexElementSize;
                if (!IsRHIRayTracingBufferRangeValid(*desc.triangles.indexBuffer,
                                                     desc.triangles.indexOffset,
                                                     indexRangeBytes))
                {
                    return RHIRayTracingValidationFail("indexed triangle geometry range exceeds index buffer size");
                }
            }

            if (desc.triangles.transformBuffer)
            {
                if (!HasRHIRayTracingASInputUsage(*desc.triangles.transformBuffer))
                {
                    return RHIRayTracingValidationFail(
                        "triangle geometry transform buffer requires AccelerationStructureInput and DeviceAddress usage");
                }

                if (!IsRHIRayTracingBufferRangeValid(*desc.triangles.transformBuffer,
                                                     desc.triangles.transformOffset,
                                                     sizeof(float) * 12))
                {
                    return RHIRayTracingValidationFail("triangle geometry transform range exceeds transform buffer size");
                }
            }

            return RHIRayTracingValidationPass();
        }

        if (desc.type != RHIRayTracingGeometryType::AABBs)
        {
            return RHIRayTracingValidationFail("ray tracing geometry type is invalid");
        }

        if (!desc.aabbs.aabbBuffer)
        {
            return RHIRayTracingValidationFail("AABB geometry requires an AABB buffer");
        }

        if (desc.aabbs.count == 0 || desc.aabbs.stride < 24)
        {
            return RHIRayTracingValidationFail("AABB geometry requires records with at least six floats");
        }

        if (!HasRHIRayTracingASInputUsage(*desc.aabbs.aabbBuffer))
        {
            return RHIRayTracingValidationFail(
                "AABB geometry buffer requires AccelerationStructureInput and DeviceAddress usage");
        }

        uint64 aabbRangeBytes = 0;
        if (!TryGetRHIRayTracingStridedRangeSize(desc.aabbs.count, desc.aabbs.stride, 24, aabbRangeBytes) ||
            !IsRHIRayTracingBufferRangeValid(*desc.aabbs.aabbBuffer, desc.aabbs.offset, aabbRangeBytes))
        {
            return RHIRayTracingValidationFail("AABB geometry range exceeds AABB buffer size");
        }

        return RHIRayTracingValidationPass();
    }

    inline RHIRayTracingValidationResult ValidateRHIBottomLevelASDesc(const RHIBottomLevelASDesc& desc)
    {
        if (!AreRHIAccelerationStructureBuildFlagsValid(desc.buildFlags))
        {
            return RHIRayTracingValidationFail("BLAS build flags contain unknown bits");
        }

        if (desc.geometries.empty())
        {
            return RHIRayTracingValidationFail("BLAS requires at least one geometry");
        }

        for (const RHIRayTracingGeometryDesc& geometry : desc.geometries)
        {
            RHIRayTracingValidationResult result = ValidateRHIRayTracingGeometryDesc(geometry);
            if (!result)
            {
                return result;
            }
        }

        return RHIRayTracingValidationPass();
    }

    inline RHIRayTracingValidationResult ValidateRHITopLevelASDesc(const RHITopLevelASDesc& desc)
    {
        if (!AreRHIAccelerationStructureBuildFlagsValid(desc.buildFlags))
        {
            return RHIRayTracingValidationFail("TLAS build flags contain unknown bits");
        }

        if (desc.instanceBuffer && !desc.instances.empty())
        {
            return RHIRayTracingValidationFail("TLAS build desc must use either an instance buffer or CPU instances, not both");
        }

        if (desc.instanceBuffer)
        {
            if (desc.instanceCount == 0)
            {
                return RHIRayTracingValidationFail("TLAS instance buffer requires a non-zero instance count");
            }

            if ((desc.instanceOffset % RVX_RAY_TRACING_INSTANCE_DESC_ALIGNMENT) != 0)
            {
                return RHIRayTracingValidationFail("TLAS instance buffer offset must be 16-byte aligned");
            }

            if (!HasRHIRayTracingASInputUsage(*desc.instanceBuffer))
            {
                return RHIRayTracingValidationFail(
                    "TLAS instance buffer requires AccelerationStructureInput and DeviceAddress usage");
            }

            const uint64 requiredInstanceBytes =
                static_cast<uint64>(desc.instanceCount) * sizeof(RHIRayTracingInstanceRecord);
            const uint64 instanceBufferSize = desc.instanceBuffer->GetSize();
            if (desc.instanceOffset > instanceBufferSize ||
                requiredInstanceBytes > instanceBufferSize - desc.instanceOffset)
            {
                return RHIRayTracingValidationFail("TLAS instance buffer range exceeds buffer size");
            }

            return RHIRayTracingValidationPass();
        }

        if (desc.instances.empty())
        {
            return RHIRayTracingValidationFail("TLAS requires at least one instance");
        }

        for (const RHIRayTracingInstanceDesc& instance : desc.instances)
        {
            if (!instance.bottomLevel)
            {
                return RHIRayTracingValidationFail("TLAS instance requires a BLAS");
            }

            if (instance.bottomLevel->GetType() != RHIAccelerationStructureType::BottomLevel)
            {
                return RHIRayTracingValidationFail("TLAS instance requires a bottom-level acceleration structure");
            }

            if (instance.bottomLevel->GetGPUVirtualAddress() == 0)
            {
                return RHIRayTracingValidationFail("TLAS instance requires a non-zero BLAS GPU address");
            }

            if (!IsRHIRayTracingInstanceTransformFinite(instance))
            {
                return RHIRayTracingValidationFail("TLAS instance transform must contain only finite values");
            }

            if (instance.instanceId > RVX_RAY_TRACING_INSTANCE_METADATA_MASK)
            {
                return RHIRayTracingValidationFail("TLAS instance ID exceeds the native 24-bit field");
            }

            if (instance.instanceMask > RVX_RAY_TRACING_INSTANCE_MASK_MASK)
            {
                return RHIRayTracingValidationFail("TLAS instance mask exceeds the native 8-bit field");
            }

            if (instance.instanceContributionToHitGroupIndex > RVX_RAY_TRACING_INSTANCE_METADATA_MASK)
            {
                return RHIRayTracingValidationFail("TLAS instance hit-group contribution exceeds the native 24-bit field");
            }

            if (!AreRHIRayTracingInstanceFlagsValid(instance.flags))
            {
                return RHIRayTracingValidationFail("TLAS instance flags contain unknown bits");
            }
        }

        return RHIRayTracingValidationPass();
    }

    inline RHIRayTracingValidationResult ValidateRHIRayTracingPipelineDesc(
        const RHIRayTracingPipelineDesc& desc)
    {
        if (!desc.pipelineLayout)
        {
            return RHIRayTracingValidationFail("ray tracing pipeline requires a pipeline layout");
        }

        if (desc.shaderGroups.empty())
        {
            return RHIRayTracingValidationFail("ray tracing pipeline requires shader groups");
        }

        if (desc.maxRecursionDepth == 0)
        {
            return RHIRayTracingValidationFail("ray tracing pipeline requires a non-zero recursion depth");
        }

        if (desc.maxAttributeSize > RVX_RAY_TRACING_MAX_ATTRIBUTE_SIZE)
        {
            return RHIRayTracingValidationFail("ray tracing pipeline max attribute size exceeds the native 32-byte limit");
        }

        bool hasRayGenerationGroup = false;
        std::unordered_set<std::string> stateObjectExports;
        for (uint32 groupIndex = 0; groupIndex < static_cast<uint32>(desc.shaderGroups.size()); ++groupIndex)
        {
            const RHIRayTracingShaderGroupDesc& group = desc.shaderGroups[groupIndex];
            if (group.exportName && group.exportName[0] == '\0')
            {
                return RHIRayTracingValidationFail("ray tracing pipeline shader group export name cannot be empty");
            }

            if (group.type == RHIRayTracingShaderGroupType::General)
            {
                if (!group.generalShader)
                {
                    return RHIRayTracingValidationFail("general shader group requires a shader");
                }

                if (group.closestHitShader || group.anyHitShader || group.intersectionShader)
                {
                    return RHIRayTracingValidationFail("general shader group cannot include hit shaders");
                }

                const RHIShaderStage generalStage = group.generalShader->GetStage();
                if (!IsRHIRayTracingGeneralShaderStage(generalStage))
                {
                    return RHIRayTracingValidationFail("general shader group requires a ray-generation, miss, or callable shader");
                }

                hasRayGenerationGroup = hasRayGenerationGroup || generalStage == RHIShaderStage::RayGeneration;
            }
            else if (group.type == RHIRayTracingShaderGroupType::TrianglesHitGroup)
            {
                if (group.generalShader || group.intersectionShader)
                {
                    return RHIRayTracingValidationFail("triangle hit group cannot include general or intersection shaders");
                }

                if (!group.closestHitShader && !group.anyHitShader)
                {
                    return RHIRayTracingValidationFail("triangle hit group requires a closest-hit or any-hit shader");
                }

                if (group.closestHitShader && group.closestHitShader->GetStage() != RHIShaderStage::ClosestHit)
                {
                    return RHIRayTracingValidationFail("triangle hit group closest-hit shader has the wrong stage");
                }

                if (group.anyHitShader && group.anyHitShader->GetStage() != RHIShaderStage::AnyHit)
                {
                    return RHIRayTracingValidationFail("triangle hit group any-hit shader has the wrong stage");
                }
            }
            else if (group.type == RHIRayTracingShaderGroupType::ProceduralHitGroup)
            {
                if (group.generalShader)
                {
                    return RHIRayTracingValidationFail("procedural hit group cannot include a general shader");
                }

                if (!group.intersectionShader)
                {
                    return RHIRayTracingValidationFail("procedural hit group requires an intersection shader");
                }

                if (group.closestHitShader && group.closestHitShader->GetStage() != RHIShaderStage::ClosestHit)
                {
                    return RHIRayTracingValidationFail("procedural hit group closest-hit shader has the wrong stage");
                }

                if (group.anyHitShader && group.anyHitShader->GetStage() != RHIShaderStage::AnyHit)
                {
                    return RHIRayTracingValidationFail("procedural hit group any-hit shader has the wrong stage");
                }

                if (group.intersectionShader->GetStage() != RHIShaderStage::Intersection)
                {
                    return RHIRayTracingValidationFail("procedural hit group intersection shader has the wrong stage");
                }
            }
            else
            {
                return RHIRayTracingValidationFail("ray tracing pipeline shader group has an invalid type");
            }

            const std::string groupExportName = group.exportName
                ? std::string(group.exportName)
                : (group.type == RHIRayTracingShaderGroupType::General
                    ? MakeRHIRayTracingDefaultGeneralExportName(group.generalShader->GetStage(), groupIndex)
                    : MakeRHIRayTracingDefaultHitGroupExportName(groupIndex));
            if (auto validation = AddRHIRayTracingStateObjectExport(stateObjectExports, groupExportName); !validation)
            {
                return validation;
            }

            if (group.type != RHIRayTracingShaderGroupType::General)
            {
                if (group.closestHitShader)
                {
                    if (auto validation = AddRHIRayTracingStateObjectExport(
                            stateObjectExports,
                            groupExportName + "_ClosestHit");
                        !validation)
                    {
                        return validation;
                    }
                }

                if (group.anyHitShader)
                {
                    if (auto validation = AddRHIRayTracingStateObjectExport(
                            stateObjectExports,
                            groupExportName + "_AnyHit");
                        !validation)
                    {
                        return validation;
                    }
                }

                if (group.intersectionShader)
                {
                    if (auto validation = AddRHIRayTracingStateObjectExport(
                            stateObjectExports,
                            groupExportName + "_Intersection");
                        !validation)
                    {
                        return validation;
                    }
                }
            }
        }

        if (!hasRayGenerationGroup)
        {
            return RHIRayTracingValidationFail("ray tracing pipeline requires a ray-generation shader group");
        }

        return RHIRayTracingValidationPass();
    }

    inline RHIRayTracingValidationResult ValidateRHIShaderTableDesc(const RHIShaderTableDesc& desc)
    {
        RHIPipeline* rayTracingPipeline = desc.GetRayTracingPipeline();
        if (!rayTracingPipeline)
        {
            return RHIRayTracingValidationFail("shader table requires a ray tracing pipeline");
        }

        if (desc.rayTracingPipelineOwner && desc.rayTracingPipeline &&
            desc.rayTracingPipelineOwner.Get() != desc.rayTracingPipeline)
        {
            return RHIRayTracingValidationFail("shader table pipeline owner must match the raw pipeline pointer");
        }

        if (!rayTracingPipeline->IsRayTracing())
        {
            return RHIRayTracingValidationFail("shader table requires a ray tracing pipeline object");
        }

        if (desc.rayGenerationRecords.size() != 1)
        {
            return RHIRayTracingValidationFail("shader table requires exactly one ray generation record");
        }

        const uint32 shaderGroupCount = rayTracingPipeline->GetRayTracingShaderGroupCount();
        if (shaderGroupCount == 0)
        {
            return RHIRayTracingValidationFail("shader table requires ray tracing shader group metadata");
        }

        auto validateRecords = [&](const std::vector<RHIShaderTableRecord>& records,
                                   RHIShaderStage expectedStage,
                                   bool expectHitGroup,
                                   const char* mismatchMessage) -> RHIRayTracingValidationResult
        {
            for (const RHIShaderTableRecord& record : records)
            {
                if (record.localRootData || record.localRootDataSize > 0)
                {
                    return RHIRayTracingValidationFail(
                        "shader table local root data is unsupported until local root signatures are exposed");
                }

                if (record.shaderGroupIndex >= shaderGroupCount)
                {
                    return RHIRayTracingValidationFail("shader table record shader group index is out of range");
                }

                const bool isHitGroup = rayTracingPipeline->IsRayTracingHitGroup(record.shaderGroupIndex);
                if (expectHitGroup)
                {
                    if (!isHitGroup)
                    {
                        return RHIRayTracingValidationFail(mismatchMessage);
                    }
                }
                else
                {
                    if (isHitGroup ||
                        rayTracingPipeline->GetRayTracingShaderGroupStage(record.shaderGroupIndex) != expectedStage)
                    {
                        return RHIRayTracingValidationFail(mismatchMessage);
                    }
                }
            }

            return RHIRayTracingValidationPass();
        };

        if (auto validation = validateRecords(desc.rayGenerationRecords,
                                              RHIShaderStage::RayGeneration,
                                              false,
                                              "shader table ray generation record requires a ray-generation shader group");
            !validation)
            return validation;
        if (auto validation = validateRecords(desc.missRecords,
                                              RHIShaderStage::Miss,
                                              false,
                                              "shader table miss record requires a miss shader group");
            !validation)
            return validation;
        if (auto validation = validateRecords(desc.hitGroupRecords,
                                              RHIShaderStage::None,
                                              true,
                                              "shader table hit-group record requires a hit group shader group");
            !validation)
            return validation;
        if (auto validation = validateRecords(desc.callableRecords,
                                              RHIShaderStage::Callable,
                                              false,
                                              "shader table callable record requires a callable shader group");
            !validation)
            return validation;

        return RHIRayTracingValidationPass();
    }

    inline RHIRayTracingValidationResult ValidateRHIDispatchRaysDesc(
        const RHIDispatchRaysDesc& desc,
        const RHIPipeline* boundRayTracingPipeline = nullptr)
    {
        if (!desc.shaderTable)
        {
            return RHIRayTracingValidationFail("DispatchRays requires a shader table");
        }

        RHIPipeline* shaderTablePipeline = desc.shaderTable->GetRayTracingPipeline();
        if (!shaderTablePipeline)
        {
            return RHIRayTracingValidationFail("DispatchRays requires a shader table with a ray tracing pipeline");
        }

        if (!shaderTablePipeline->IsRayTracing())
        {
            return RHIRayTracingValidationFail("DispatchRays requires a shader table owned by a ray tracing pipeline");
        }

        if (boundRayTracingPipeline)
        {
            if (!boundRayTracingPipeline->IsRayTracing())
            {
                return RHIRayTracingValidationFail("DispatchRays requires a bound ray tracing pipeline");
            }

            if (shaderTablePipeline != boundRayTracingPipeline)
            {
                return RHIRayTracingValidationFail(
                    "DispatchRays shader table must match the bound ray tracing pipeline");
            }
        }

        if (desc.shaderTable->GetRayGenerationRecordCount() != 1)
        {
            return RHIRayTracingValidationFail("DispatchRays requires exactly one ray generation shader table record");
        }

        if (desc.width == 0 || desc.height == 0 || desc.depth == 0)
        {
            return RHIRayTracingValidationFail("DispatchRays dimensions must be non-zero");
        }

        uint64 rayCount = 0;
        if (!TryGetRHIRayTracingDispatchRayCount(desc, rayCount))
        {
            return RHIRayTracingValidationFail("DispatchRays ray count exceeds 64-bit range");
        }

        return RHIRayTracingValidationPass();
    }

} // namespace RVX
