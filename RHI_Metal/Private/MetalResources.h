#pragma once

#include "MetalCommon.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIShader.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHIHeap.h"

namespace RVX
{
    // =============================================================================
    // MetalBuffer
    // =============================================================================
    class MetalBuffer : public RHIBuffer
    {
    public:
        MetalBuffer(id<MTLDevice> device, const RHIBufferDesc& desc);
        MetalBuffer(id<MTLHeap> heap, uint64 offset, const RHIBufferDesc& desc); // Placed buffer
        ~MetalBuffer() override;

        uint64 GetSize() const override { return m_size; }
        RHIBufferUsage GetUsage() const override { return m_usage; }
        RHIMemoryType GetMemoryType() const override { return m_memoryType; }
        uint32 GetStride() const override { return m_stride; }

        void* Map() override;
        void Unmap() override;

        id<MTLBuffer> GetMTLBuffer() const { return m_buffer; }

    private:
        id<MTLBuffer> m_buffer = nil;
        uint64 m_size = 0;
        RHIBufferUsage m_usage = RHIBufferUsage::None;
        RHIMemoryType m_memoryType = RHIMemoryType::Default;
        uint32 m_stride = 0;
    };

    // =============================================================================
    // MetalTexture
    // =============================================================================
    class MetalTexture : public RHITexture
    {
    public:
        MetalTexture(id<MTLDevice> device, const RHITextureDesc& desc);
        MetalTexture(id<MTLHeap> heap, uint64 offset, const RHITextureDesc& desc); // Placed texture
        MetalTexture(id<MTLTexture> texture, const RHITextureDesc& desc); // For swap chain
        ~MetalTexture() override;

        uint32 GetWidth() const override { return m_width; }
        uint32 GetHeight() const override { return m_height; }
        uint32 GetDepth() const override { return m_depth; }
        uint32 GetMipLevels() const override { return m_mipLevels; }
        uint32 GetArraySize() const override { return m_arrayLayers; }
        RHIFormat GetFormat() const override { return m_format; }
        RHITextureDimension GetDimension() const override { return m_dimension; }
        RHITextureUsage GetUsage() const override { return m_usage; }
        RHISampleCount GetSampleCount() const override { return m_sampleCount; }

        id<MTLTexture> GetMTLTexture() const { return m_texture; }

    private:
        id<MTLTexture> m_texture = nil;
        uint32 m_width = 0;
        uint32 m_height = 0;
        uint32 m_depth = 0;
        uint32 m_mipLevels = 1;
        uint32 m_arrayLayers = 1;
        RHIFormat m_format = RHIFormat::Unknown;
        RHITextureDimension m_dimension = RHITextureDimension::Texture2D;
        RHITextureUsage m_usage = RHITextureUsage::None;
        RHISampleCount m_sampleCount = RHISampleCount::Count1;
        bool m_ownsTexture = true;
    };

    // =============================================================================
    // MetalTextureView
    // =============================================================================
    class MetalTextureView : public RHITextureView
    {
    public:
        MetalTextureView(MetalTexture* texture, const RHITextureViewDesc& desc);
        ~MetalTextureView() override;

        RHITexture* GetTexture() const override { return m_sourceTexture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_subresourceRange; }

        id<MTLTexture> GetMTLTexture() const { return m_textureView; }

    private:
        MetalTexture* m_sourceTexture = nullptr;
        id<MTLTexture> m_textureView = nil;
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_subresourceRange;
    };

    // =============================================================================
    // MetalSampler
    // =============================================================================
    class MetalSampler : public RHISampler
    {
    public:
        MetalSampler(id<MTLDevice> device, const RHISamplerDesc& desc);
        ~MetalSampler() override;

        id<MTLSamplerState> GetMTLSampler() const { return m_sampler; }

    private:
        id<MTLSamplerState> m_sampler = nil;
    };

    // =============================================================================
    // MetalShader
    // =============================================================================
    class MetalShader : public RHIShader
    {
    public:
        MetalShader(id<MTLDevice> device, const RHIShaderDesc& desc);
        ~MetalShader() override;

        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }
        const char* GetEntryPoint() const { return m_entryPoint.c_str(); }

        id<MTLLibrary> GetMTLLibrary() const { return m_library; }
        id<MTLFunction> GetMTLFunction() const { return m_function; }

    private:
        id<MTLLibrary> m_library = nil;
        id<MTLFunction> m_function = nil;
        RHIShaderStage m_stage = RHIShaderStage::None;
        std::string m_entryPoint;
        std::vector<uint8> m_bytecode;
    };

    // =============================================================================
    // MetalHeap
    // =============================================================================
    class MetalHeap : public RHIHeap
    {
    public:
        MetalHeap(id<MTLDevice> device, const RHIHeapDesc& desc);
        ~MetalHeap() override;

        uint64 GetSize() const override { return m_size; }
        RHIHeapType GetType() const override { return m_type; }
        RHIHeapFlags GetFlags() const override { return m_flags; }

        id<MTLHeap> GetMTLHeap() const { return m_heap; }

    private:
        id<MTLHeap> m_heap = nil;
        uint64 m_size = 0;
        RHIHeapType m_type = RHIHeapType::Default;
        RHIHeapFlags m_flags = RHIHeapFlags::AllowAll;
    };

    // =============================================================================
    // MetalDescriptorSetLayout
    // =============================================================================
    class MetalDescriptorSetLayout : public RHIDescriptorSetLayout
    {
    public:
        MetalDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc);
        ~MetalDescriptorSetLayout() override = default;

        const RHIDescriptorSetLayoutDesc& GetDesc() const { return m_desc; }

    private:
        RHIDescriptorSetLayoutDesc m_desc;
    };

    // =============================================================================
    // MetalDescriptorSet
    // =============================================================================
    class MetalDescriptorSet : public RHIDescriptorSet
    {
    public:
        MetalDescriptorSet(const RHIDescriptorSetDesc& desc);
        ~MetalDescriptorSet() override = default;

        void Update(const std::vector<RHIDescriptorBinding>& bindings) override;

        const RHIDescriptorSetDesc& GetDesc() const { return m_desc; }

        // Binding data for direct binding approach
        struct BindingData
        {
            id<MTLBuffer> buffer = nil;
            uint64 offset = 0;
            id<MTLTexture> texture = nil;
            id<MTLSamplerState> sampler = nil;
        };

        const std::vector<BindingData>& GetBindings() const { return m_bindings; }

    private:
        RHIDescriptorSetDesc m_desc;
        std::vector<BindingData> m_bindings;
    };

} // namespace RVX
