#pragma once

// Prevent Windows macro conflicts
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Define VK_USE_PLATFORM before including Vulkan
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// Include RHI definitions after Vulkan to ensure proper ordering
#include "RHI/RHIDefinitions.h"
#include "Core/Log.h"
#include "Core/Assert.h"

// Vulkan Memory Allocator
#include <vk_mem_alloc.h>

#include <vector>
#include <string>
#include <optional>
#include <array>

namespace RVX
{
    // =============================================================================
    // Vulkan Error Handling
    // =============================================================================
    inline const char* VkResultToString(VkResult result)
    {
        switch (result)
        {
            case VK_SUCCESS: return "VK_SUCCESS";
            case VK_NOT_READY: return "VK_NOT_READY";
            case VK_TIMEOUT: return "VK_TIMEOUT";
            case VK_EVENT_SET: return "VK_EVENT_SET";
            case VK_EVENT_RESET: return "VK_EVENT_RESET";
            case VK_INCOMPLETE: return "VK_INCOMPLETE";
            case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
            case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
            case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
            case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
            case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
            case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
            default: return "UNKNOWN_VK_RESULT";
        }
    }

    inline void VkCheck(VkResult result, const char* message = "Vulkan Error")
    {
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("{}: {}", message, VkResultToString(result));
            RVX_ASSERT_MSG(false, "Vulkan Error");
        }
    }

    #define VK_CHECK(result) VkCheck(result, #result)

    // =============================================================================
    // Format Conversion
    // =============================================================================
    inline VkFormat ToVkFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::Unknown:        return VK_FORMAT_UNDEFINED;
            case RHIFormat::R8_UNORM:       return VK_FORMAT_R8_UNORM;
            case RHIFormat::R8_SNORM:       return VK_FORMAT_R8_SNORM;
            case RHIFormat::R8_UINT:        return VK_FORMAT_R8_UINT;
            case RHIFormat::R8_SINT:        return VK_FORMAT_R8_SINT;
            case RHIFormat::RG8_UNORM:      return VK_FORMAT_R8G8_UNORM;
            case RHIFormat::RG8_SNORM:      return VK_FORMAT_R8G8_SNORM;
            case RHIFormat::RG8_UINT:       return VK_FORMAT_R8G8_UINT;
            case RHIFormat::RG8_SINT:       return VK_FORMAT_R8G8_SINT;
            case RHIFormat::RGBA8_UNORM:    return VK_FORMAT_R8G8B8A8_UNORM;
            case RHIFormat::RGBA8_SNORM:    return VK_FORMAT_R8G8B8A8_SNORM;
            case RHIFormat::RGBA8_UINT:     return VK_FORMAT_R8G8B8A8_UINT;
            case RHIFormat::RGBA8_SINT:     return VK_FORMAT_R8G8B8A8_SINT;
            case RHIFormat::RGBA8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
            case RHIFormat::BGRA8_UNORM:    return VK_FORMAT_B8G8R8A8_UNORM;
            case RHIFormat::BGRA8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
            case RHIFormat::R16_UNORM:      return VK_FORMAT_R16_UNORM;
            case RHIFormat::R16_UINT:       return VK_FORMAT_R16_UINT;
            case RHIFormat::R16_SINT:       return VK_FORMAT_R16_SINT;
            case RHIFormat::R16_FLOAT:      return VK_FORMAT_R16_SFLOAT;
            case RHIFormat::RG16_UNORM:     return VK_FORMAT_R16G16_UNORM;
            case RHIFormat::RG16_UINT:      return VK_FORMAT_R16G16_UINT;
            case RHIFormat::RG16_SINT:      return VK_FORMAT_R16G16_SINT;
            case RHIFormat::RG16_FLOAT:     return VK_FORMAT_R16G16_SFLOAT;
            case RHIFormat::RGBA16_UNORM:   return VK_FORMAT_R16G16B16A16_UNORM;
            case RHIFormat::RGBA16_UINT:    return VK_FORMAT_R16G16B16A16_UINT;
            case RHIFormat::RGBA16_SINT:    return VK_FORMAT_R16G16B16A16_SINT;
            case RHIFormat::RGBA16_FLOAT:   return VK_FORMAT_R16G16B16A16_SFLOAT;
            case RHIFormat::R32_UINT:       return VK_FORMAT_R32_UINT;
            case RHIFormat::R32_SINT:       return VK_FORMAT_R32_SINT;
            case RHIFormat::R32_FLOAT:      return VK_FORMAT_R32_SFLOAT;
            case RHIFormat::RG32_UINT:      return VK_FORMAT_R32G32_UINT;
            case RHIFormat::RG32_SINT:      return VK_FORMAT_R32G32_SINT;
            case RHIFormat::RG32_FLOAT:     return VK_FORMAT_R32G32_SFLOAT;
            case RHIFormat::RGB32_FLOAT:    return VK_FORMAT_R32G32B32_SFLOAT;
            case RHIFormat::RGBA32_UINT:    return VK_FORMAT_R32G32B32A32_UINT;
            case RHIFormat::RGBA32_SINT:    return VK_FORMAT_R32G32B32A32_SINT;
            case RHIFormat::RGBA32_FLOAT:   return VK_FORMAT_R32G32B32A32_SFLOAT;
            case RHIFormat::D16_UNORM:      return VK_FORMAT_D16_UNORM;
            case RHIFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
            case RHIFormat::D32_FLOAT:      return VK_FORMAT_D32_SFLOAT;
            case RHIFormat::D32_FLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
            case RHIFormat::BC1_UNORM:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case RHIFormat::BC1_UNORM_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case RHIFormat::BC2_UNORM:      return VK_FORMAT_BC2_UNORM_BLOCK;
            case RHIFormat::BC2_UNORM_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
            case RHIFormat::BC3_UNORM:      return VK_FORMAT_BC3_UNORM_BLOCK;
            case RHIFormat::BC3_UNORM_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
            case RHIFormat::BC4_UNORM:      return VK_FORMAT_BC4_UNORM_BLOCK;
            case RHIFormat::BC4_SNORM:      return VK_FORMAT_BC4_SNORM_BLOCK;
            case RHIFormat::BC5_UNORM:      return VK_FORMAT_BC5_UNORM_BLOCK;
            case RHIFormat::BC5_SNORM:      return VK_FORMAT_BC5_SNORM_BLOCK;
            case RHIFormat::BC6H_UF16:      return VK_FORMAT_BC6H_UFLOAT_BLOCK;
            case RHIFormat::BC6H_SF16:      return VK_FORMAT_BC6H_SFLOAT_BLOCK;
            case RHIFormat::BC7_UNORM:      return VK_FORMAT_BC7_UNORM_BLOCK;
            case RHIFormat::BC7_UNORM_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    inline RHIFormat FromVkFormat(VkFormat format)
    {
        switch (format)
        {
            case VK_FORMAT_B8G8R8A8_UNORM:  return RHIFormat::BGRA8_UNORM;
            case VK_FORMAT_B8G8R8A8_SRGB:   return RHIFormat::BGRA8_UNORM_SRGB;
            case VK_FORMAT_R8G8B8A8_UNORM:  return RHIFormat::RGBA8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB:   return RHIFormat::RGBA8_UNORM_SRGB;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return RHIFormat::RGBA16_FLOAT;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return RHIFormat::RGB10A2_UNORM;
            default: return RHIFormat::Unknown;
        }
    }

    // =============================================================================
    // Resource State / Access Flags Conversion
    // =============================================================================
    inline VkAccessFlags ToVkAccessFlags(RHIResourceState state)
    {
        VkAccessFlags flags = 0;
        switch (state)
        {
            case RHIResourceState::Common:
                break;
            case RHIResourceState::VertexBuffer:
                flags = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
                break;
            case RHIResourceState::IndexBuffer:
                flags = VK_ACCESS_INDEX_READ_BIT;
                break;
            case RHIResourceState::ConstantBuffer:
                flags = VK_ACCESS_UNIFORM_READ_BIT;
                break;
            case RHIResourceState::ShaderResource:
                flags = VK_ACCESS_SHADER_READ_BIT;
                break;
            case RHIResourceState::UnorderedAccess:
                flags = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                break;
            case RHIResourceState::RenderTarget:
                flags = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                break;
            case RHIResourceState::DepthWrite:
                flags = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                break;
            case RHIResourceState::DepthRead:
                flags = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                break;
            case RHIResourceState::CopySource:
                flags = VK_ACCESS_TRANSFER_READ_BIT;
                break;
            case RHIResourceState::CopyDest:
                flags = VK_ACCESS_TRANSFER_WRITE_BIT;
                break;
            case RHIResourceState::Present:
                break;
            case RHIResourceState::IndirectArgument:
                flags = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
                break;
            default:
                break;
        }
        return flags;
    }

    inline VkImageLayout ToVkImageLayout(RHIResourceState state)
    {
        switch (state)
        {
            case RHIResourceState::Common:
                return VK_IMAGE_LAYOUT_GENERAL;
            case RHIResourceState::ShaderResource:
                return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            case RHIResourceState::UnorderedAccess:
                return VK_IMAGE_LAYOUT_GENERAL;
            case RHIResourceState::RenderTarget:
                return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case RHIResourceState::DepthWrite:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case RHIResourceState::DepthRead:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            case RHIResourceState::CopySource:
                return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            case RHIResourceState::CopyDest:
                return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            case RHIResourceState::Present:
                return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            default:
                return VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }

    inline VkPipelineStageFlags ToVkPipelineStageFlags(RHIResourceState state)
    {
        switch (state)
        {
            case RHIResourceState::Common:
                return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            case RHIResourceState::VertexBuffer:
            case RHIResourceState::IndexBuffer:
                return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            case RHIResourceState::ConstantBuffer:
            case RHIResourceState::ShaderResource:
                return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            case RHIResourceState::UnorderedAccess:
                return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            case RHIResourceState::RenderTarget:
                return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            case RHIResourceState::DepthWrite:
            case RHIResourceState::DepthRead:
                return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            case RHIResourceState::CopySource:
            case RHIResourceState::CopyDest:
                return VK_PIPELINE_STAGE_TRANSFER_BIT;
            case RHIResourceState::Present:
                return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            case RHIResourceState::IndirectArgument:
                return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
            default:
                return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
    }

    // =============================================================================
    // Other Conversions
    // =============================================================================
    inline VkPrimitiveTopology ToVkPrimitiveTopology(RHIPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHIPrimitiveTopology::PointList:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case RHIPrimitiveTopology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case RHIPrimitiveTopology::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case RHIPrimitiveTopology::TriangleList:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case RHIPrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }
    }

    inline VkCullModeFlags ToVkCullMode(RHICullMode mode)
    {
        switch (mode)
        {
            case RHICullMode::None:  return VK_CULL_MODE_NONE;
            case RHICullMode::Front: return VK_CULL_MODE_FRONT_BIT;
            case RHICullMode::Back:  return VK_CULL_MODE_BACK_BIT;
            default: return VK_CULL_MODE_BACK_BIT;
        }
    }

    inline VkPolygonMode ToVkPolygonMode(RHIFillMode mode)
    {
        switch (mode)
        {
            case RHIFillMode::Solid:     return VK_POLYGON_MODE_FILL;
            case RHIFillMode::Wireframe: return VK_POLYGON_MODE_LINE;
            default: return VK_POLYGON_MODE_FILL;
        }
    }

    inline VkCompareOp ToVkCompareOp(RHICompareOp op)
    {
        switch (op)
        {
            case RHICompareOp::Never:        return VK_COMPARE_OP_NEVER;
            case RHICompareOp::Less:         return VK_COMPARE_OP_LESS;
            case RHICompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
            case RHICompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
            case RHICompareOp::Greater:      return VK_COMPARE_OP_GREATER;
            case RHICompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
            case RHICompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case RHICompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
            default: return VK_COMPARE_OP_LESS;
        }
    }

    inline VkBlendFactor ToVkBlendFactor(RHIBlendFactor factor)
    {
        switch (factor)
        {
            case RHIBlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
            case RHIBlendFactor::One:              return VK_BLEND_FACTOR_ONE;
            case RHIBlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
            case RHIBlendFactor::InvSrcColor:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case RHIBlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
            case RHIBlendFactor::InvSrcAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case RHIBlendFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
            case RHIBlendFactor::InvDstColor:      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case RHIBlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
            case RHIBlendFactor::InvDstAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case RHIBlendFactor::SrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            case RHIBlendFactor::ConstantColor:    return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case RHIBlendFactor::InvConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            default: return VK_BLEND_FACTOR_ONE;
        }
    }

    inline VkBlendOp ToVkBlendOp(RHIBlendOp op)
    {
        switch (op)
        {
            case RHIBlendOp::Add:             return VK_BLEND_OP_ADD;
            case RHIBlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
            case RHIBlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case RHIBlendOp::Min:             return VK_BLEND_OP_MIN;
            case RHIBlendOp::Max:             return VK_BLEND_OP_MAX;
            default: return VK_BLEND_OP_ADD;
        }
    }

    inline VkStencilOp ToVkStencilOp(RHIStencilOp op)
    {
        switch (op)
        {
            case RHIStencilOp::Keep:           return VK_STENCIL_OP_KEEP;
            case RHIStencilOp::Zero:           return VK_STENCIL_OP_ZERO;
            case RHIStencilOp::Replace:        return VK_STENCIL_OP_REPLACE;
            case RHIStencilOp::IncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case RHIStencilOp::DecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case RHIStencilOp::Invert:         return VK_STENCIL_OP_INVERT;
            case RHIStencilOp::IncrementWrap:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case RHIStencilOp::DecrementWrap:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            default: return VK_STENCIL_OP_KEEP;
        }
    }

    inline VkFilter ToVkFilter(RHIFilterMode filter)
    {
        switch (filter)
        {
            case RHIFilterMode::Nearest: return VK_FILTER_NEAREST;
            case RHIFilterMode::Linear:  return VK_FILTER_LINEAR;
            default: return VK_FILTER_LINEAR;
        }
    }

    inline VkSamplerAddressMode ToVkSamplerAddressMode(RHIAddressMode mode)
    {
        switch (mode)
        {
            case RHIAddressMode::Repeat:        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case RHIAddressMode::MirrorRepeat:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case RHIAddressMode::ClampToEdge:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case RHIAddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }

    inline VkSamplerMipmapMode ToVkMipmapMode(RHIFilterMode filter)
    {
        switch (filter)
        {
            case RHIFilterMode::Nearest: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
            case RHIFilterMode::Linear:  return VK_SAMPLER_MIPMAP_MODE_LINEAR;
            default: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
    }

    inline VkDescriptorType ToVkDescriptorType(RHIBindingType type)
    {
        switch (type)
        {
            case RHIBindingType::UniformBuffer:        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case RHIBindingType::DynamicUniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            case RHIBindingType::StorageBuffer:        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case RHIBindingType::DynamicStorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            case RHIBindingType::SampledTexture:       return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            case RHIBindingType::StorageTexture:       return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case RHIBindingType::Sampler:              return VK_DESCRIPTOR_TYPE_SAMPLER;
            case RHIBindingType::CombinedTextureSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            default: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
    }

    inline VkShaderStageFlags ToVkShaderStageFlags(RHIShaderStage stage)
    {
        VkShaderStageFlags flags = 0;
        if (HasFlag(stage, RHIShaderStage::Vertex))   flags |= VK_SHADER_STAGE_VERTEX_BIT;
        if (HasFlag(stage, RHIShaderStage::Pixel))    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (HasFlag(stage, RHIShaderStage::Geometry)) flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
        if (HasFlag(stage, RHIShaderStage::Hull))     flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        if (HasFlag(stage, RHIShaderStage::Domain))   flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        if (HasFlag(stage, RHIShaderStage::Compute))  flags |= VK_SHADER_STAGE_COMPUTE_BIT;
        if (stage == RHIShaderStage::All)             flags = VK_SHADER_STAGE_ALL;
        return flags;
    }

    // =============================================================================
    // Queue Family Indices
    // =============================================================================
    struct QueueFamilyIndices
    {
        std::optional<uint32> graphicsFamily;
        std::optional<uint32> computeFamily;
        std::optional<uint32> transferFamily;
        std::optional<uint32> presentFamily;

        bool IsComplete() const
        {
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

} // namespace RVX
