#include "MetalResources.h"
#include "MetalConversions.h"

namespace RVX
{
    // =============================================================================
    // MetalBuffer
    // =============================================================================
    MetalBuffer::MetalBuffer(id<MTLDevice> device, const RHIBufferDesc& desc)
        : m_size(desc.size)
        , m_usage(desc.usage)
        , m_memoryType(desc.memoryType)
        , m_stride(desc.stride)
    {
        MTLResourceOptions options = 0;

        // Check if running on Apple Silicon (Unified Memory Architecture)
        // On Apple Silicon, Shared mode can be more efficient for certain workloads
        // as it avoids unnecessary GPU-side copies
        bool hasUnifiedMemory = false;
        if (@available(macOS 10.15, iOS 13.0, *))
        {
            hasUnifiedMemory = [device hasUnifiedMemory];
        }

        switch (desc.memoryType)
        {
            case RHIMemoryType::Default:
                if (hasUnifiedMemory)
                {
                    // Apple Silicon: Use Shared for frequently updated buffers
                    // Use Private only for GPU-only resources
                    bool isGPUOnly = !HasFlag(desc.usage, RHIBufferUsage::Constant);
                    if (isGPUOnly && !HasFlag(desc.usage, RHIBufferUsage::Vertex) && 
                        !HasFlag(desc.usage, RHIBufferUsage::Index))
                    {
                        options = MTLResourceStorageModePrivate;
                    }
                    else
                    {
                        // For vertex/index/constant buffers, Shared is often better on Apple Silicon
                        options = MTLResourceStorageModeShared;
                    }
                }
                else
                {
                    // Intel Mac: Private mode for GPU-only resources
                    options = MTLResourceStorageModePrivate;
                }
                break;

            case RHIMemoryType::Upload:
                // Write-combined for optimal CPU write performance
                options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
                break;

            case RHIMemoryType::Readback:
                // Default cache mode for CPU read performance
                options = MTLResourceStorageModeShared;
                break;
        }

        // For static vertex/index buffers that won't be updated, disable hazard tracking
        // This can improve performance by reducing Metal's internal synchronization
        if (@available(macOS 10.15, iOS 13.0, *))
        {
            if (HasFlag(desc.usage, RHIBufferUsage::Vertex) || HasFlag(desc.usage, RHIBufferUsage::Index))
            {
                if (desc.memoryType == RHIMemoryType::Default)
                {
                    options |= MTLResourceHazardTrackingModeUntracked;
                }
            }
        }

        m_buffer = [device newBufferWithLength:desc.size options:options];

        if (desc.debugName)
        {
            [m_buffer setLabel:[NSString stringWithUTF8String:desc.debugName]];
            SetDebugName(desc.debugName);
        }
    }

    MetalBuffer::MetalBuffer(id<MTLHeap> heap, uint64 offset, const RHIBufferDesc& desc)
        : m_size(desc.size)
        , m_usage(desc.usage)
        , m_memoryType(desc.memoryType)
        , m_stride(desc.stride)
    {
        // Create buffer from heap at specified offset
        m_buffer = [heap newBufferWithLength:desc.size options:MTLResourceStorageModePrivate offset:offset];

        if (desc.debugName)
        {
            [m_buffer setLabel:[NSString stringWithUTF8String:desc.debugName]];
            SetDebugName(desc.debugName);
        }
    }

    MetalBuffer::MetalBuffer(id<MTLBuffer> existingBuffer, const RHIBufferDesc& desc)
        : m_buffer(existingBuffer)
        , m_size(desc.size)
        , m_usage(desc.usage)
        , m_memoryType(desc.memoryType)
        , m_stride(desc.stride)
    {
        // Wrapper constructor - takes ownership of existing buffer
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    MetalBuffer::~MetalBuffer()
    {
        m_buffer = nil;
    }

    void* MetalBuffer::Map()
    {
        if (m_memoryType == RHIMemoryType::Default)
        {
            RVX_RHI_ERROR("Cannot map GPU-only buffer");
            return nullptr;
        }
        return [m_buffer contents];
    }

    void MetalBuffer::Unmap()
    {
        // Metal shared buffers don't need explicit unmap
        // For managed buffers, we would call didModifyRange:
    }

    // =============================================================================
    // MetalTexture
    // =============================================================================
    MetalTexture::MetalTexture(id<MTLDevice> device, const RHITextureDesc& desc)
        : m_width(desc.width)
        , m_height(desc.height)
        , m_depth(desc.depth)
        , m_mipLevels(desc.mipLevels)
        , m_arrayLayers(desc.arraySize)
        , m_format(desc.format)
        , m_dimension(desc.dimension)
        , m_usage(desc.usage)
        , m_sampleCount(desc.sampleCount)
        , m_ownsTexture(true)
    {
        MTLTextureDescriptor* texDesc = [[MTLTextureDescriptor alloc] init];

        // Set texture type
        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                texDesc.textureType = MTLTextureType1D;
                break;
            case RHITextureDimension::Texture2D:
                if (desc.arraySize > 1)
                    texDesc.textureType = MTLTextureType2DArray;
                else if (static_cast<uint32>(desc.sampleCount) > 1)
                    texDesc.textureType = MTLTextureType2DMultisample;
                else
                    texDesc.textureType = MTLTextureType2D;
                break;
            case RHITextureDimension::Texture3D:
                texDesc.textureType = MTLTextureType3D;
                break;
            case RHITextureDimension::TextureCube:
                texDesc.textureType = MTLTextureTypeCube;
                break;
        }

        texDesc.pixelFormat = ToMTLPixelFormat(desc.format);
        texDesc.width = desc.width;
        texDesc.height = desc.height;
        texDesc.depth = desc.depth;
        texDesc.mipmapLevelCount = desc.mipLevels;
        texDesc.arrayLength = desc.arraySize;
        texDesc.sampleCount = static_cast<NSUInteger>(desc.sampleCount);

        // Set usage flags
        MTLTextureUsage usage = MTLTextureUsageUnknown;
        if (HasFlag(desc.usage, RHITextureUsage::ShaderResource))
            usage |= MTLTextureUsageShaderRead;
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
            usage |= MTLTextureUsageShaderWrite;
        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget) ||
            HasFlag(desc.usage, RHITextureUsage::DepthStencil))
            usage |= MTLTextureUsageRenderTarget;

        texDesc.usage = usage;

        // Storage mode - optimize based on usage patterns
        if (HasFlag(desc.usage, RHITextureUsage::Transient))
        {
            // Transient/memoryless textures don't allocate backing memory
            // Content is only valid during a single render pass
            // Ideal for temporary render targets like depth buffers or MSAA resolve targets
#if TARGET_OS_IPHONE || (TARGET_OS_OSX && __MAC_OS_X_VERSION_MIN_REQUIRED >= 110000)
            texDesc.storageMode = MTLStorageModeMemoryless;
#else
            // Fallback for older macOS versions
            texDesc.storageMode = MTLStorageModePrivate;
#endif
        }
        else if (HasFlag(desc.usage, RHITextureUsage::RenderTarget) ||
                 HasFlag(desc.usage, RHITextureUsage::DepthStencil))
        {
            texDesc.storageMode = MTLStorageModePrivate;
        }
        else
        {
            texDesc.storageMode = MTLStorageModePrivate;
        }

        m_texture = [device newTextureWithDescriptor:texDesc];

        if (desc.debugName)
        {
            [m_texture setLabel:[NSString stringWithUTF8String:desc.debugName]];
            SetDebugName(desc.debugName);
        }
    }

    MetalTexture::MetalTexture(id<MTLHeap> heap, uint64 offset, const RHITextureDesc& desc)
        : m_width(desc.width)
        , m_height(desc.height)
        , m_depth(desc.depth)
        , m_mipLevels(desc.mipLevels)
        , m_arrayLayers(desc.arraySize)
        , m_format(desc.format)
        , m_dimension(desc.dimension)
        , m_usage(desc.usage)
        , m_sampleCount(desc.sampleCount)
        , m_ownsTexture(true)
    {
        MTLTextureDescriptor* texDesc = [[MTLTextureDescriptor alloc] init];

        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                texDesc.textureType = MTLTextureType1D;
                break;
            case RHITextureDimension::Texture2D:
                if (desc.arraySize > 1)
                    texDesc.textureType = MTLTextureType2DArray;
                else if (static_cast<uint32>(desc.sampleCount) > 1)
                    texDesc.textureType = MTLTextureType2DMultisample;
                else
                    texDesc.textureType = MTLTextureType2D;
                break;
            case RHITextureDimension::Texture3D:
                texDesc.textureType = MTLTextureType3D;
                break;
            case RHITextureDimension::TextureCube:
                texDesc.textureType = MTLTextureTypeCube;
                break;
        }

        texDesc.pixelFormat = ToMTLPixelFormat(desc.format);
        texDesc.width = desc.width;
        texDesc.height = desc.height;
        texDesc.depth = desc.depth;
        texDesc.mipmapLevelCount = desc.mipLevels;
        texDesc.arrayLength = desc.arraySize;
        texDesc.sampleCount = static_cast<NSUInteger>(desc.sampleCount);

        MTLTextureUsage usage = MTLTextureUsageUnknown;
        if (HasFlag(desc.usage, RHITextureUsage::ShaderResource))
            usage |= MTLTextureUsageShaderRead;
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
            usage |= MTLTextureUsageShaderWrite;
        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget) ||
            HasFlag(desc.usage, RHITextureUsage::DepthStencil))
            usage |= MTLTextureUsageRenderTarget;

        texDesc.usage = usage;
        texDesc.storageMode = heap.storageMode;

        // Create texture from heap at specified offset
        m_texture = [heap newTextureWithDescriptor:texDesc offset:offset];

        if (desc.debugName)
        {
            [m_texture setLabel:[NSString stringWithUTF8String:desc.debugName]];
            SetDebugName(desc.debugName);
        }
    }

    MetalTexture::MetalTexture(id<MTLTexture> texture, const RHITextureDesc& desc)
        : m_texture(texture)
        , m_width(desc.width)
        , m_height(desc.height)
        , m_depth(desc.depth)
        , m_mipLevels(desc.mipLevels)
        , m_arrayLayers(desc.arraySize)
        , m_format(desc.format)
        , m_dimension(desc.dimension)
        , m_usage(desc.usage)
        , m_sampleCount(desc.sampleCount)
        , m_ownsTexture(false)
    {
    }

    MetalTexture::~MetalTexture()
    {
        if (m_ownsTexture)
        {
            m_texture = nil;
        }
    }

    // =============================================================================
    // MetalTextureView
    // =============================================================================
    MetalTextureView::MetalTextureView(MetalTexture* texture, const RHITextureViewDesc& desc)
        : m_sourceTexture(texture)
        , m_format(desc.format != RHIFormat::Unknown ? desc.format : texture->GetFormat())
        , m_subresourceRange(desc.subresourceRange)
    {
        id<MTLTexture> srcTexture = texture->GetMTLTexture();

        // Create a texture view with the specified format
        MTLPixelFormat pixelFormat = ToMTLPixelFormat(m_format);
        m_textureView = [srcTexture newTextureViewWithPixelFormat:pixelFormat];

        if (desc.debugName)
        {
            [m_textureView setLabel:[NSString stringWithUTF8String:desc.debugName]];
            SetDebugName(desc.debugName);
        }
    }

    MetalTextureView::~MetalTextureView()
    {
        m_textureView = nil;
    }

    // =============================================================================
    // MetalSampler
    // =============================================================================
    MetalSampler::MetalSampler(id<MTLDevice> device, const RHISamplerDesc& desc)
    {
        MTLSamplerDescriptor* samplerDesc = [[MTLSamplerDescriptor alloc] init];

        samplerDesc.minFilter = ToMTLSamplerFilter(desc.minFilter);
        samplerDesc.magFilter = ToMTLSamplerFilter(desc.magFilter);
        samplerDesc.mipFilter = desc.mipFilter == RHIFilterMode::Linear ?
            MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;

        samplerDesc.sAddressMode = ToMTLSamplerAddressMode(desc.addressU);
        samplerDesc.tAddressMode = ToMTLSamplerAddressMode(desc.addressV);
        samplerDesc.rAddressMode = ToMTLSamplerAddressMode(desc.addressW);

        samplerDesc.maxAnisotropy = desc.maxAnisotropy;
        samplerDesc.compareFunction = ToMTLCompareFunction(desc.compareOp);
        samplerDesc.lodMinClamp = desc.minLod;
        samplerDesc.lodMaxClamp = desc.maxLod;

        m_sampler = [device newSamplerStateWithDescriptor:samplerDesc];

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    MetalSampler::~MetalSampler()
    {
        m_sampler = nil;
    }

    // =============================================================================
    // MetalShader
    // =============================================================================
    MetalShader::MetalShader(id<MTLDevice> device, const RHIShaderDesc& desc)
        : m_stage(desc.stage)
        , m_entryPoint(desc.entryPoint ? desc.entryPoint : "main")
    {
        // The shader bytecode should be MSL source code (as string)
        if (!desc.bytecode || desc.bytecodeSize == 0)
        {
            RVX_RHI_ERROR("MetalShader: No shader bytecode provided");
            return;
        }

        // Treat bytecode as MSL source string
        NSString* mslSource = [[NSString alloc]
            initWithBytes:desc.bytecode
            length:desc.bytecodeSize
            encoding:NSUTF8StringEncoding];

        if (!mslSource)
        {
            RVX_RHI_ERROR("MetalShader: Failed to parse MSL source");
            return;
        }

        // Compile MSL to library
        NSError* error = nil;
        m_library = [device newLibraryWithSource:mslSource options:nil error:&error];

        if (error || !m_library)
        {
            RVX_RHI_ERROR("MetalShader: Failed to compile MSL: {}",
                error ? [[error localizedDescription] UTF8String] : "Unknown error");
            return;
        }

        // Get the function
        NSString* entryPointNS = [NSString stringWithUTF8String:m_entryPoint.c_str()];
        m_function = [m_library newFunctionWithName:entryPointNS];

        if (!m_function)
        {
            RVX_RHI_ERROR("MetalShader: Entry point '{}' not found in library", m_entryPoint);
            return;
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    MetalShader::~MetalShader()
    {
        m_function = nil;
        m_library = nil;
    }

    // =============================================================================
    // MetalHeap
    // =============================================================================
    MetalHeap::MetalHeap(id<MTLDevice> device, const RHIHeapDesc& desc)
        : m_size(desc.size)
        , m_type(desc.type)
        , m_flags(desc.flags)
    {
        MTLHeapDescriptor* heapDesc = [[MTLHeapDescriptor alloc] init];
        heapDesc.size = desc.size;
        heapDesc.storageMode = MTLStorageModePrivate;
        heapDesc.cpuCacheMode = MTLCPUCacheModeDefaultCache;

        m_heap = [device newHeapWithDescriptor:heapDesc];

        if (desc.debugName)
        {
            [m_heap setLabel:[NSString stringWithUTF8String:desc.debugName]];
            SetDebugName(desc.debugName);
        }
    }

    MetalHeap::~MetalHeap()
    {
        m_heap = nil;
    }

    // =============================================================================
    // MetalDescriptorSetLayout
    // =============================================================================
    MetalDescriptorSetLayout::MetalDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
        : m_desc(desc)
    {
        // Metal doesn't have explicit descriptor set layouts
        // This is just metadata for binding
    }

    // =============================================================================
    // MetalDescriptorSet
    // =============================================================================
    MetalDescriptorSet::MetalDescriptorSet(const RHIDescriptorSetDesc& desc)
        : m_desc(desc)
    {
        // Pre-allocate binding slots based on layout
        if (desc.layout)
        {
            auto* layout = static_cast<MetalDescriptorSetLayout*>(desc.layout);
            m_bindings.resize(layout->GetDesc().entries.size());
        }

        // Apply initial bindings from desc
        if (!desc.bindings.empty())
        {
            Update(desc.bindings);
        }
    }

    void MetalDescriptorSet::Update(const std::vector<RHIDescriptorBinding>& bindings)
    {
        for (const auto& updateDesc : bindings)
        {
            if (updateDesc.binding >= m_bindings.size())
            {
                m_bindings.resize(updateDesc.binding + 1);
            }

            BindingData& binding = m_bindings[updateDesc.binding];

            if (updateDesc.buffer)
            {
                binding.buffer = static_cast<MetalBuffer*>(updateDesc.buffer)->GetMTLBuffer();
                binding.offset = updateDesc.offset;
            }
            if (updateDesc.textureView)
            {
                binding.texture = static_cast<MetalTextureView*>(updateDesc.textureView)->GetMTLTexture();
            }
            if (updateDesc.sampler)
            {
                binding.sampler = static_cast<MetalSampler*>(updateDesc.sampler)->GetMTLSampler();
            }
        }
    }

} // namespace RVX
