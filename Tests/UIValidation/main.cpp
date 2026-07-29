#include "RHI/RHI.h"
#include "UI/UI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

using namespace RVX;
using namespace RVX::UI;

namespace
{
    TEST(UIValidation, ClipboardUsesMemoryFallbackAndExternalProvider)
    {
        UIClipboard::ResetProvider();
        EXPECT_FALSE(UIClipboard::HasExternalProvider());
        EXPECT_TRUE(UIClipboard::SetText("Fallback"));
        std::string text;
        EXPECT_TRUE(UIClipboard::GetText(text));
        EXPECT_EQ("Fallback", text);
        EXPECT_TRUE(UIClipboard::HasText());

        std::string externalText = "External";
        uint32 getCount = 0;
        uint32 setCount = 0;
        UIClipboardProvider provider;
        provider.getText = [&externalText, &getCount](std::string& output) {
            ++getCount;
            output = externalText;
            return true;
        };
        provider.setText = [&externalText, &setCount](const std::string& input) {
            ++setCount;
            externalText = input;
            return true;
        };
        UIClipboard::SetProvider(std::move(provider));
        EXPECT_TRUE(UIClipboard::HasExternalProvider());
        EXPECT_TRUE(UIClipboard::SetText("Native"));
        EXPECT_EQ(1u, setCount);
        EXPECT_EQ("Native", externalText);
        EXPECT_TRUE(UIClipboard::GetText(text));
        EXPECT_EQ("Native", text);
        EXPECT_EQ(1u, getCount);

        UIClipboard::ResetProvider();
        EXPECT_FALSE(UIClipboard::HasExternalProvider());
        EXPECT_TRUE(UIClipboard::GetText(text));
        EXPECT_EQ("Native", text);
    }

    Rect GlyphRectForExpectation(const Rect& rect)
    {
        const float left = std::round(rect.Left());
        const float top = std::round(rect.Top());
        float right = std::round(rect.Right());
        float bottom = std::round(rect.Bottom());
        if (right <= left && rect.width > 0.0f)
        {
            right = left + 1.0f;
        }
        if (bottom <= top && rect.height > 0.0f)
        {
            bottom = top + 1.0f;
        }
        return Rect(left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top));
    }

    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size), 0u)
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override
        {
            ++mapCount;
            return m_storage.data();
        }

        void Unmap() override { ++unmapCount; }

        const std::vector<uint8>& GetStorage() const { return m_storage; }

        uint32 mapCount = 0;
        uint32 unmapCount = 0;

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
    };

    class FakeStagingBuffer final : public RHIStagingBuffer
    {
    public:
        explicit FakeStagingBuffer(const RHIStagingBufferDesc& desc)
            : m_desc(desc)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = desc.size;
            bufferDesc.usage = RHIBufferUsage::CopySrc;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = desc.debugName;
            m_buffer = MakeRef<FakeBuffer>(bufferDesc);
        }

        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override
        {
            (void)size;
            auto* bytes = static_cast<uint8*>(m_buffer->Map());
            return bytes ? bytes + offset : nullptr;
        }

        void Unmap() override { m_buffer->Unmap(); }
        uint64 GetSize() const override { return m_desc.size; }
        RHIBuffer* GetBuffer() const override { return m_buffer.Get(); }

    private:
        RHIStagingBufferDesc m_desc;
        Ref<FakeBuffer> m_buffer;
    };

    class FakeTexture final : public RHITexture
    {
    public:
        explicit FakeTexture(const RHITextureDesc& desc)
            : m_desc(desc)
        {
        }

        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

    private:
        RHITextureDesc m_desc;
    };

    class FakeTextureView final : public RHITextureView
    {
    public:
        FakeTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
            : m_texture(texture)
            , m_format(desc.format == RHIFormat::Unknown && texture ? texture->GetFormat() : desc.format)
            , m_range(desc.subresourceRange)
        {
        }

        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_range; }

    private:
        RHITexture* m_texture = nullptr;
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_range;
    };

    class FakeSampler final : public RHISampler
    {
    };

    class FakeDescriptorSetLayout final : public RHIDescriptorSetLayout
    {
    public:
        explicit FakeDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
            : m_entries(desc.entries)
        {
        }

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }

    private:
        std::vector<RHIBindingLayoutEntry> m_entries;
    };

    class FakeDescriptorSet final : public RHIDescriptorSet
    {
    public:
        explicit FakeDescriptorSet(const RHIDescriptorSetDesc& desc)
            : bindings(desc.bindings)
        {
        }

        bool Update(const std::vector<RHIDescriptorBinding>& updatedBindings) override
        {
            bindings = updatedBindings;
            return true;
        }

        std::vector<RHIDescriptorBinding> bindings;
    };

    class FakePipeline final : public RHIPipeline
    {
    public:
        bool IsCompute() const override { return false; }
    };

    struct FakeDrawIndexedCall
    {
        uint32 indexCount = 0;
        uint32 instanceCount = 0;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
        uint32 firstInstance = 0;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        void Begin() override { calls.push_back("Begin"); }
        void End() override { calls.push_back("End"); }
        void Reset() override { calls.push_back("Reset"); }
        void BeginEvent(const char*, uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, uint32 = 0) override {}
        void BufferBarrier(const RHIBufferBarrier& barrier) override
        {
            calls.push_back("BufferBarrier");
            bufferBarriers.push_back(barrier);
        }
        void TextureBarrier(const RHITextureBarrier& barrier) override
        {
            calls.push_back("TextureBarrier");
            textureBarriers.push_back(barrier);
        }
        void Barriers(std::span<const RHIBufferBarrier> buffers,
                      std::span<const RHITextureBarrier> textures) override
        {
            calls.push_back("Barriers");
            bufferBarriers.insert(bufferBarriers.end(), buffers.begin(), buffers.end());
            textureBarriers.insert(textureBarriers.end(), textures.begin(), textures.end());
        }
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc& desc) override
        {
            calls.push_back("BeginRenderPass");
            renderPassDesc = desc;
        }
        void EndRenderPass() override { calls.push_back("EndRenderPass"); }
        void SetPipeline(RHIPipeline* pipeline) override
        {
            calls.push_back("SetPipeline");
            boundPipeline = pipeline;
        }
        void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset = 0) override
        {
            calls.push_back("SetVertexBuffer");
            vertexBufferSlot = slot;
            boundVertexBuffer = buffer;
            vertexBufferOffset = offset;
        }
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset = 0) override
        {
            calls.push_back("SetIndexBuffer");
            boundIndexBuffer = buffer;
            indexFormat = format;
            indexBufferOffset = offset;
        }
        void SetDescriptorSet(uint32 slot,
                              RHIDescriptorSet* set,
                              std::span<const uint32> = {}) override
        {
            calls.push_back("SetDescriptorSet");
            descriptorSetSlots.push_back(slot);
            boundDescriptorSets.push_back(set);
        }
        void SetPushConstants(const void*, uint32 size, uint32 offset = 0) override
        {
            calls.push_back("SetPushConstants");
            pushConstantSize = size;
            pushConstantOffset = offset;
        }
        void SetViewport(const RHIViewport& viewport) override
        {
            calls.push_back("SetViewport");
            boundViewport = viewport;
        }
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect& rect) override
        {
            calls.push_back("SetScissor");
            boundScissor = rect;
            scissors.push_back(rect);
        }
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
        void DrawIndexed(uint32 indexCount,
                         uint32 instanceCount = 1,
                         uint32 firstIndex = 0,
                         int32 vertexOffset = 0,
                         uint32 firstInstance = 0) override
        {
            calls.push_back("DrawIndexed");
            drawIndexedCalls.push_back({indexCount, instanceCount, firstIndex, vertexOffset, firstInstance});
        }
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override
        {
            calls.push_back("CopyBuffer");
        }
        void CopyTexture(RHITexture*, RHITexture*, const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RHIBuffer*, RHITexture*, const RHIBufferTextureCopyDesc& desc) override
        {
            calls.push_back("CopyBufferToTexture");
            textureCopies.push_back(desc);
        }
        void CopyTextureToBuffer(RHITexture*, RHIBuffer*, const RHIBufferTextureCopyDesc&) override {}
        void BeginQuery(RHIQueryPool*, uint32) override {}
        void EndQuery(RHIQueryPool*, uint32) override {}
        void WriteTimestamp(RHIQueryPool*, uint32) override {}
        void ResolveQueries(RHIQueryPool*, uint32, uint32, RHIBuffer*, uint64) override {}
        void ResetQueries(RHIQueryPool*, uint32, uint32) override {}
        void SetStencilReference(uint32) override {}
        void SetBlendConstants(const float[4]) override {}
        void SetDepthBias(float, float, float = 0.0f) override {}
        void SetDepthBounds(float, float) override {}
        void SetStencilReferenceSeparate(uint32, uint32) override {}
        void SetLineWidth(float) override {}
        void SignalFence(RHIFence*, uint64) override {}
        void WaitFence(RHIFence*, uint64) override {}

        std::vector<std::string> calls;
        std::vector<RHIBufferBarrier> bufferBarriers;
        std::vector<RHITextureBarrier> textureBarriers;
        std::vector<RHIBufferTextureCopyDesc> textureCopies;
        std::vector<FakeDrawIndexedCall> drawIndexedCalls;
        std::vector<RHIRect> scissors;
        std::vector<uint32> descriptorSetSlots;
        std::vector<RHIDescriptorSet*> boundDescriptorSets;
        RHIRenderPassDesc renderPassDesc;
        RHIViewport boundViewport;
        RHIRect boundScissor;
        RHIPipeline* boundPipeline = nullptr;
        RHIBuffer* boundVertexBuffer = nullptr;
        RHIBuffer* boundIndexBuffer = nullptr;
        uint32 vertexBufferSlot = RVX_INVALID_INDEX;
        uint64 vertexBufferOffset = 0;
        RHIFormat indexFormat = RHIFormat::Unknown;
        uint64 indexBufferOffset = 0;
        uint32 pushConstantSize = 0;
        uint32 pushConstantOffset = 0;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            createdBufferDescs.push_back(desc);
            return MakeRef<FakeBuffer>(desc);
        }
        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            createdTextureDescs.push_back(desc);
            return MakeRef<FakeTexture>(desc);
        }
        RHITextureViewRef CreateTextureView(RHITexture* texture,
                                            const RHITextureViewDesc& desc = {}) override
        {
            createdTextureViewDescs.push_back(desc);
            return MakeRef<FakeTextureView>(texture, desc);
        }
        RHISamplerRef CreateSampler(const RHISamplerDesc& desc) override
        {
            createdSamplerDescs.push_back(desc);
            return MakeRef<FakeSampler>();
        }
        RHIShaderRef CreateShader(const RHIShaderDesc&) override { return nullptr; }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return nullptr; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override
        {
            return nullptr;
        }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override
        {
            return nullptr;
        }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override { return {}; }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(
            const RHIDescriptorSetLayoutDesc& desc) override
        {
            createdDescriptorSetLayoutDescs.push_back(desc);
            return MakeRef<FakeDescriptorSetLayout>(desc);
        }
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override
        {
            return nullptr;
        }
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override
        {
            createdGraphicsPipelineDescs.push_back(desc);
            return MakeRef<FakePipeline>();
        }
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return nullptr; }
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override
        {
            createdDescriptorSetDescs.push_back(desc);
            return MakeRef<FakeDescriptorSet>(desc);
        }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return nullptr; }
        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* = nullptr) override { return 0; }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>,
                                     RHIFence* = nullptr) override
        {
            return 0;
        }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }
        RHIFenceRef CreateFence(uint64 = 0) override { return nullptr; }
        void WaitForFence(RHIFence*, uint64) override {}
        void WaitIdle() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        uint32 GetCurrentFrameIndex() const override { return 0; }
        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc& desc) override
        {
            createdStagingBufferDescs.push_back(desc);
            return MakeRef<FakeStagingBuffer>(desc);
        }
        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return nullptr; }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::None; }

        std::vector<RHIBufferDesc> createdBufferDescs;
        std::vector<RHITextureDesc> createdTextureDescs;
        std::vector<RHITextureViewDesc> createdTextureViewDescs;
        std::vector<RHISamplerDesc> createdSamplerDescs;
        std::vector<RHIDescriptorSetLayoutDesc> createdDescriptorSetLayoutDescs;
        std::vector<RHIGraphicsPipelineDesc> createdGraphicsPipelineDescs;
        std::vector<RHIDescriptorSetDesc> createdDescriptorSetDescs;
        std::vector<RHIStagingBufferDesc> createdStagingBufferDescs;

    private:
        RHICapabilities m_capabilities;
    };

    RHITextureViewRef CreateView(RHITextureViewType type)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.type = type;
        viewDesc.format = RHIFormat::RGBA8_UNORM;
        return MakeRef<FakeTextureView>(nullptr, viewDesc);
    }

    std::filesystem::path FindSystemFontPath()
    {
#if defined(_WIN32)
        constexpr std::array<const char*, 3> candidates = {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/calibri.ttf",
        };
#elif defined(__APPLE__)
        constexpr std::array<const char*, 3> candidates = {
            "/System/Library/Fonts/SFNS.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
        };
#else
        constexpr std::array<const char*, 4> candidates = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        };
#endif

        for (const char* candidate : candidates)
        {
            std::error_code error;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
        }
        return {};
    }

    std::filesystem::path FindFontPathWithCodepoint(uint32 codepoint)
    {
#if defined(_WIN32)
        constexpr std::array<const char*, 9> candidates = {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/calibri.ttf",
            "C:/Windows/Fonts/NotoSansSC-VF.ttf",
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/simhei.ttf",
            "C:/Windows/Fonts/simsun.ttc",
            "C:/Windows/Fonts/Deng.ttf",
            "C:/Windows/Fonts/SimsunExtG.ttf",
        };
#elif defined(__APPLE__)
        constexpr std::array<const char*, 5> candidates = {
            "/System/Library/Fonts/SFNS.ttf",
            "/System/Library/Fonts/PingFang.ttc",
            "/System/Library/Fonts/STHeiti Light.ttc",
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
            "/Library/Fonts/Arial.ttf",
        };
#else
        constexpr std::array<const char*, 7> candidates = {
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.otf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        };
#endif

        for (const char* candidate : candidates)
        {
            std::error_code error;
            if (!std::filesystem::exists(candidate, error))
            {
                continue;
            }

            UIFontMetrics font;
            if (font.LoadTrueTypeFile(candidate) && font.HasCodepoint(codepoint))
            {
                return candidate;
            }
        }
        return {};
    }

    std::filesystem::path FindFontPathWithoutCodepoint(uint32 codepoint)
    {
#if defined(_WIN32)
        constexpr std::array<const char*, 3> candidates = {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/calibri.ttf",
        };
#elif defined(__APPLE__)
        constexpr std::array<const char*, 3> candidates = {
            "/System/Library/Fonts/SFNS.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
        };
#else
        constexpr std::array<const char*, 4> candidates = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        };
#endif

        for (const char* candidate : candidates)
        {
            std::error_code error;
            if (!std::filesystem::exists(candidate, error))
            {
                continue;
            }

            UIFontMetrics font;
            if (font.LoadTrueTypeFile(candidate) && !font.HasCodepoint(codepoint))
            {
                return candidate;
            }
        }
        return {};
    }

    struct KerningFontSample
    {
        std::filesystem::path path;
        uint32 previousCodepoint = 0u;
        uint32 codepoint = 0u;
        float kerning = 0.0f;
    };

    KerningFontSample FindKerningFontSample(float fontSize)
    {
#if defined(_WIN32)
        constexpr std::array<const char*, 3> candidates = {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/calibri.ttf",
        };
#elif defined(__APPLE__)
        constexpr std::array<const char*, 3> candidates = {
            "/System/Library/Fonts/SFNS.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
        };
#else
        constexpr std::array<const char*, 4> candidates = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        };
#endif
        constexpr std::array<std::pair<uint32, uint32>, 5> pairs = {
            std::pair<uint32, uint32>{'A', 'V'},
            {'T', 'o'},
            {'Y', 'a'},
            {'W', 'a'},
            {'L', 'T'}};

        for (const char* candidate : candidates)
        {
            std::error_code error;
            if (!std::filesystem::exists(candidate, error))
            {
                continue;
            }

            UIFontMetrics font;
            if (!font.LoadTrueTypeFile(candidate))
            {
                continue;
            }

            for (const auto& pair : pairs)
            {
                const float kerning =
                    font.GetKerningAdvance(pair.first, pair.second, fontSize);
                if (std::abs(kerning) > 0.001f)
                {
                    return {candidate, pair.first, pair.second, kerning};
                }
            }
        }

        return {};
    }
}

TEST(UIValidation, RendererInitializesRuntimeRHIAndRecordsPrimitives)
{
    FakeDevice device;
    auto targetView = CreateView(RHITextureViewType::RenderTarget);
    auto imageView = CreateView(RHITextureViewType::ShaderResource);

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    EXPECT_EQ(UIRenderBackend::RuntimeRHI, renderer.GetBackend());

    UIRenderTargetDesc target;
    target.width = 128;
    target.height = 64;
    target.format = RHIFormat::RGBA8_UNORM;
    target.colorTarget = targetView.Get();

    renderer.BeginFrame(target);
    ASSERT_TRUE(renderer.IsRecording());
    renderer.DrawRect(Rect(1.0f, 2.0f, 16.0f, 8.0f), UIColor::Red());
    renderer.DrawText("Wi\nHi", Rect(0.0f, 0.0f, 80.0f, 64.0f), 20.0f, UIColor::White());
    renderer.DrawImage(imageView.Get(), Rect(4.0f, 6.0f, 24.0f, 24.0f));
    renderer.EndFrame();

    const UIRenderStats& stats = renderer.GetStats();
    EXPECT_EQ(3u, stats.commandCount);
    EXPECT_EQ(1u, stats.rectCount);
    EXPECT_EQ(1u, stats.textCount);
    EXPECT_EQ(1u, stats.imageCount);
    EXPECT_EQ(4u, stats.glyphCount);
    EXPECT_EQ(24u, stats.vertexCount);
    EXPECT_EQ(36u, stats.indexCount);
    ASSERT_EQ(3u, renderer.GetCommands().size());
    EXPECT_EQ(UIDrawCommandType::Image, renderer.GetCommands()[2].type);
    EXPECT_EQ(imageView.Get(), renderer.GetCommands()[2].textureView);
}

TEST(UIValidation, RendererDrawBorderEmitsFourClampedEdges)
{
    FakeDevice device;

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.BeginRecordOnlyFrame(128, 96);
    renderer.DrawBorder(Rect(10.0f, 20.0f, 30.0f, 16.0f),
                        UIColor::Yellow(),
                        2.0f);
    renderer.EndFrame();

    const UIRenderStats& stats = renderer.GetStats();
    EXPECT_EQ(4u, stats.commandCount);
    EXPECT_EQ(4u, stats.rectCount);
    ASSERT_EQ(4u, renderer.GetCommands().size());

    const UIDrawCommand& top = renderer.GetCommands()[0];
    EXPECT_EQ(UIDrawCommandType::Rect, top.type);
    EXPECT_FLOAT_EQ(10.0f, top.bounds.x);
    EXPECT_FLOAT_EQ(20.0f, top.bounds.y);
    EXPECT_FLOAT_EQ(30.0f, top.bounds.width);
    EXPECT_FLOAT_EQ(2.0f, top.bounds.height);

    const UIDrawCommand& bottom = renderer.GetCommands()[1];
    EXPECT_FLOAT_EQ(10.0f, bottom.bounds.x);
    EXPECT_FLOAT_EQ(34.0f, bottom.bounds.y);
    EXPECT_FLOAT_EQ(30.0f, bottom.bounds.width);
    EXPECT_FLOAT_EQ(2.0f, bottom.bounds.height);

    const UIDrawCommand& left = renderer.GetCommands()[2];
    EXPECT_FLOAT_EQ(10.0f, left.bounds.x);
    EXPECT_FLOAT_EQ(22.0f, left.bounds.y);
    EXPECT_FLOAT_EQ(2.0f, left.bounds.width);
    EXPECT_FLOAT_EQ(12.0f, left.bounds.height);

    const UIDrawCommand& right = renderer.GetCommands()[3];
    EXPECT_FLOAT_EQ(38.0f, right.bounds.x);
    EXPECT_FLOAT_EQ(22.0f, right.bounds.y);
    EXPECT_FLOAT_EQ(2.0f, right.bounds.width);
    EXPECT_FLOAT_EQ(12.0f, right.bounds.height);
}

TEST(UIValidation, RendererDrawLineEmitsOrientedQuadGeometry)
{
    FakeDevice device;

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.BeginRecordOnlyFrame(128, 96);
    renderer.DrawLine(Vec2(10.0f, 20.0f),
                      Vec2(30.0f, 40.0f),
                      4.0f,
                      UIColor::White());
    renderer.EndFrame();

    const UIRenderStats& stats = renderer.GetStats();
    EXPECT_EQ(1u, stats.commandCount);
    EXPECT_EQ(1u, stats.rectCount);
    EXPECT_EQ(4u, stats.vertexCount);
    EXPECT_EQ(6u, stats.indexCount);
    ASSERT_EQ(1u, renderer.GetCommands().size());
    ASSERT_EQ(4u, renderer.GetVertices().size());

    const UIDrawCommand& line = renderer.GetCommands()[0];
    EXPECT_EQ(UIDrawCommandType::Rect, line.type);
    EXPECT_NEAR(8.5858f, line.bounds.x, 0.001f);
    EXPECT_NEAR(18.5858f, line.bounds.y, 0.001f);
    EXPECT_NEAR(22.8284f, line.bounds.width, 0.001f);
    EXPECT_NEAR(22.8284f, line.bounds.height, 0.001f);

    EXPECT_NEAR(8.5858f, renderer.GetVertices()[0].position.x, 0.001f);
    EXPECT_NEAR(21.4142f, renderer.GetVertices()[0].position.y, 0.001f);
    EXPECT_NEAR(28.5858f, renderer.GetVertices()[1].position.x, 0.001f);
    EXPECT_NEAR(41.4142f, renderer.GetVertices()[1].position.y, 0.001f);
    EXPECT_NEAR(31.4142f, renderer.GetVertices()[2].position.x, 0.001f);
    EXPECT_NEAR(38.5858f, renderer.GetVertices()[2].position.y, 0.001f);
    EXPECT_NEAR(11.4142f, renderer.GetVertices()[3].position.x, 0.001f);
    EXPECT_NEAR(18.5858f, renderer.GetVertices()[3].position.y, 0.001f);
}

TEST(UIValidation, WidgetsRenderThroughUIRendererCommands)
{
    FakeDevice device;
    auto targetView = CreateView(RHITextureViewType::RenderTarget);
    auto imageView = CreateView(RHITextureViewType::ShaderResource);

    UICanvas canvas;
    canvas.Initialize(320.0f, 180.0f);

    auto panel = Panel::Create();
    panel->SetPosition(8.0f, 10.0f);
    panel->SetSize(160.0f, 96.0f);
    panel->SetBackgroundColor(UIColor(0.08f, 0.1f, 0.12f, 1.0f));

    auto button = Button::Create("Run");
    button->SetPosition(12.0f, 14.0f);
    button->SetSize(72.0f, 28.0f);

    auto label = Label::Create("Status");
    label->SetPosition(12.0f, 48.0f);
    label->SetSize(96.0f, 24.0f);

    auto image = Image::Create();
    image->SetPosition(116.0f, 14.0f);
    image->SetSize(32.0f, 32.0f);
    image->SetTextureView(imageView.Get());

    panel->AddChild(button);
    panel->AddChild(label);
    panel->AddChild(image);
    canvas.AddWidget(panel);

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.BeginFrame({320, 180, RHIFormat::RGBA8_UNORM, targetView.Get()});
    canvas.Render(renderer);
    renderer.EndFrame();

    const UIRenderStats& stats = renderer.GetStats();
    EXPECT_EQ(5u, stats.commandCount);
    EXPECT_EQ(2u, stats.rectCount);
    EXPECT_EQ(2u, stats.textCount);
    EXPECT_EQ(1u, stats.imageCount);

    ASSERT_EQ(5u, renderer.GetCommands().size());
    EXPECT_EQ("Run", renderer.GetCommands()[2].text);
    EXPECT_EQ("Status", renderer.GetCommands()[3].text);
    EXPECT_FLOAT_EQ(20.0f, renderer.GetCommands()[2].clipRect.x);
    EXPECT_FLOAT_EQ(24.0f, renderer.GetCommands()[2].clipRect.y);
    EXPECT_FLOAT_EQ(72.0f, renderer.GetCommands()[2].clipRect.width);
    EXPECT_FLOAT_EQ(28.0f, renderer.GetCommands()[2].clipRect.height);
    EXPECT_FLOAT_EQ(20.0f, renderer.GetCommands()[3].clipRect.x);
    EXPECT_FLOAT_EQ(58.0f, renderer.GetCommands()[3].clipRect.y);
    EXPECT_FLOAT_EQ(96.0f, renderer.GetCommands()[3].clipRect.width);
    EXPECT_FLOAT_EQ(24.0f, renderer.GetCommands()[3].clipRect.height);
    EXPECT_EQ(imageView.Get(), renderer.GetCommands()[4].textureView);
}

TEST(UIValidation, FocusedButtonRendersVisibleFocusOutline)
{
    FakeDevice device;
    auto targetView = CreateView(RHITextureViewType::RenderTarget);

    UICanvas canvas;
    canvas.Initialize(160.0f, 80.0f);

    auto button = Button::Create("Apply");
    button->SetPosition(20.0f, 24.0f);
    button->SetSize(96.0f, 28.0f);
    canvas.AddWidget(button);
    canvas.SetFocusedWidget(button.get());

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.BeginFrame({160, 80, RHIFormat::RGBA8_UNORM, targetView.Get()});
    canvas.Render(renderer);
    renderer.EndFrame();

    const UIRenderStats& stats = renderer.GetStats();
    EXPECT_EQ(10u, stats.commandCount);
    EXPECT_EQ(9u, stats.rectCount);
    EXPECT_EQ(1u, stats.textCount);
    ASSERT_EQ(10u, renderer.GetCommands().size());

    EXPECT_EQ(UIDrawCommandType::Rect, renderer.GetCommands()[1].type);
    EXPECT_FLOAT_EQ(19.0f, renderer.GetCommands()[1].bounds.x);
    EXPECT_FLOAT_EQ(23.0f, renderer.GetCommands()[1].bounds.y);
    EXPECT_FLOAT_EQ(98.0f, renderer.GetCommands()[1].bounds.width);
    EXPECT_FLOAT_EQ(1.0f, renderer.GetCommands()[1].bounds.height);

    EXPECT_EQ(UIDrawCommandType::Rect, renderer.GetCommands()[5].type);
    EXPECT_FLOAT_EQ(20.0f, renderer.GetCommands()[5].bounds.x);
    EXPECT_FLOAT_EQ(24.0f, renderer.GetCommands()[5].bounds.y);
    EXPECT_FLOAT_EQ(96.0f, renderer.GetCommands()[5].bounds.width);
    EXPECT_FLOAT_EQ(2.0f, renderer.GetCommands()[5].bounds.height);

    EXPECT_EQ(UIDrawCommandType::Text, renderer.GetCommands().back().type);
    EXPECT_EQ("Apply", renderer.GetCommands().back().text);

    canvas.Shutdown();
}

TEST(UIValidation, RendererRecordOnlyFrameBuildsCommandsWithoutSubmitTarget)
{
    FakeDevice device;

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));

    renderer.BeginRecordOnlyFrame(256, 128);
    ASSERT_TRUE(renderer.IsRecording());
    EXPECT_EQ(256u, renderer.GetCurrentTarget().width);
    EXPECT_EQ(128u, renderer.GetCurrentTarget().height);
    EXPECT_EQ(RHIFormat::RGBA8_UNORM, renderer.GetCurrentTarget().format);
    EXPECT_EQ(nullptr, renderer.GetCurrentTarget().colorTarget);

    renderer.DrawRect(Rect(4.0f, 6.0f, 48.0f, 20.0f), UIColor::Red());
    renderer.DrawText("Native", Rect(8.0f, 32.0f, 96.0f, 28.0f), 16.0f, UIColor::White());
    renderer.EndFrame();

    EXPECT_FALSE(renderer.IsRecording());
    const UIRenderStats& stats = renderer.GetStats();
    EXPECT_EQ(2u, stats.commandCount);
    EXPECT_EQ(1u, stats.rectCount);
    EXPECT_EQ(1u, stats.textCount);
    EXPECT_GT(stats.vertexCount, 0u);
    EXPECT_GT(stats.indexCount, 0u);

    UIRenderSubmitDesc submitDesc;
    EXPECT_FALSE(renderer.Submit(submitDesc));
    EXPECT_FALSE(renderer.GetSubmitStats().submitted);
}

TEST(UIValidation, RendererBindsRecordedFrameToMatchingSwapChainTarget)
{
    FakeDevice device;

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));

    renderer.BeginRecordOnlyFrame(640, 360, RHIFormat::RGBA8_UNORM);
    renderer.DrawRect(Rect(0.0f, 0.0f, 640.0f, 32.0f), UIColor::White());
    renderer.EndFrame();

    RHITextureDesc textureDesc;
    textureDesc.width = 640;
    textureDesc.height = 360;
    textureDesc.format = RHIFormat::BGRA8_UNORM;
    textureDesc.usage = RHITextureUsage::RenderTarget;
    auto targetTexture = MakeRef<FakeTexture>(textureDesc);

    RHITextureViewDesc viewDesc;
    viewDesc.type = RHITextureViewType::RenderTarget;
    viewDesc.format = RHIFormat::BGRA8_UNORM;
    auto targetView = MakeRef<FakeTextureView>(targetTexture.Get(), viewDesc);

    UIRenderTargetDesc target;
    target.width = 640;
    target.height = 360;
    target.format = RHIFormat::BGRA8_UNORM;
    target.colorTarget = targetView.Get();

    EXPECT_TRUE(renderer.BindRecordedFrameTarget(target));
    EXPECT_EQ(targetView.Get(), renderer.GetCurrentTarget().colorTarget);
    EXPECT_EQ(RHIFormat::BGRA8_UNORM, renderer.GetCurrentTarget().format);

    target.width = 320;
    EXPECT_FALSE(renderer.BindRecordedFrameTarget(target));
    EXPECT_EQ(targetView.Get(), renderer.GetCurrentTarget().colorTarget);
}

TEST(UIValidation, RendererSubmitsRecordOnlyFrameToDefaultFramebufferWhenAllowed)
{
    FakeDevice device;
    FakeCommandContext context;
    auto pipeline = MakeRef<FakePipeline>();

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));

    renderer.BeginRecordOnlyFrame(300, 120, RHIFormat::BGRA8_UNORM);
    renderer.DrawRect(Rect(0.0f, 0.0f, 300.0f, 24.0f), UIColor::White());
    renderer.DrawText("Native", Rect(8.0f, 4.0f, 96.0f, 20.0f), 14.0f, UIColor::Black());
    renderer.EndFrame();

    UIRenderSubmitDesc submitDesc;
    submitDesc.commandContext = &context;
    submitDesc.pipeline = pipeline.Get();
    submitDesc.allowDefaultFramebufferTarget = true;
    ASSERT_TRUE(renderer.Submit(submitDesc));

    const UIRenderSubmitStats& submitStats = renderer.GetSubmitStats();
    EXPECT_TRUE(submitStats.submitted);
    EXPECT_TRUE(submitStats.defaultFramebufferTarget);
    EXPECT_EQ(2u, submitStats.drawCallCount);
    EXPECT_GT(submitStats.vertexBytesUploaded, 0u);
    EXPECT_GT(submitStats.indexBytesUploaded, 0u);

    EXPECT_EQ(1u, std::count(context.calls.begin(), context.calls.end(), "BeginRenderPass"));
    const RHIRenderPassDesc& renderPass = context.renderPassDesc;
    EXPECT_EQ(0u, renderPass.colorAttachmentCount);
    EXPECT_FALSE(renderPass.hasDepthStencil);
    EXPECT_EQ(300u, renderPass.renderArea.width);
    EXPECT_EQ(120u, renderPass.renderArea.height);
    EXPECT_EQ(300.0f, context.boundViewport.width);
    EXPECT_EQ(120.0f, context.boundViewport.height);
    EXPECT_EQ(300u, context.boundScissor.width);
    EXPECT_EQ(120u, context.boundScissor.height);
    EXPECT_EQ(pipeline.Get(), context.boundPipeline);
    EXPECT_EQ(renderer.GetVertexBuffer(), context.boundVertexBuffer);
    EXPECT_EQ(renderer.GetIndexBuffer(), context.boundIndexBuffer);
}

TEST(UIValidation, RendererAppliesClipRectScissorsPerDrawCommand)
{
    FakeDevice device;
    FakeCommandContext context;
    auto pipeline = MakeRef<FakePipeline>();

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));

    renderer.BeginRecordOnlyFrame(200, 100, RHIFormat::RGBA8_UNORM);
    renderer.DrawRect(Rect(0.0f, 0.0f, 24.0f, 24.0f), UIColor::White());
    renderer.PushClipRect(Rect(10.0f, 12.0f, 40.0f, 30.0f));
    renderer.DrawRect(Rect(0.0f, 0.0f, 80.0f, 80.0f), UIColor::Red());
    renderer.PopClipRect();
    renderer.DrawRect(Rect(60.0f, 0.0f, 24.0f, 24.0f), UIColor::Blue());
    renderer.EndFrame();

    ASSERT_EQ(3u, renderer.GetCommands().size());
    EXPECT_FLOAT_EQ(200.0f, renderer.GetCommands()[0].clipRect.width);
    EXPECT_FLOAT_EQ(40.0f, renderer.GetCommands()[1].clipRect.width);
    EXPECT_FLOAT_EQ(30.0f, renderer.GetCommands()[1].clipRect.height);
    EXPECT_FLOAT_EQ(200.0f, renderer.GetCommands()[2].clipRect.width);

    UIRenderSubmitDesc submitDesc;
    submitDesc.commandContext = &context;
    submitDesc.pipeline = pipeline.Get();
    submitDesc.allowDefaultFramebufferTarget = true;
    ASSERT_TRUE(renderer.Submit(submitDesc));
    EXPECT_EQ(3u, renderer.GetSubmitStats().drawCallCount);
    ASSERT_GE(context.scissors.size(), static_cast<size_t>(3));
    EXPECT_EQ(200u, context.scissors[0].width);
    EXPECT_EQ(100u, context.scissors[0].height);
    EXPECT_EQ(10, context.scissors[1].x);
    EXPECT_EQ(12, context.scissors[1].y);
    EXPECT_EQ(40u, context.scissors[1].width);
    EXPECT_EQ(30u, context.scissors[1].height);
    EXPECT_EQ(200u, context.scissors.back().width);
    EXPECT_EQ(100u, context.scissors.back().height);
}

TEST(UIValidation, PanelClipChildrenAppliesClipRectToDescendantCommands)
{
    FakeDevice device;
    UICanvas canvas;
    canvas.Initialize(240.0f, 120.0f);

    auto panel = Panel::Create();
    panel->SetName("ClipPanel");
    panel->SetPosition(20.0f, 10.0f);
    panel->SetSize(80.0f, 30.0f);
    panel->SetBackgroundColor(UIColor::Transparent());
    panel->SetClipChildren(true);

    auto label = Label::Create("This label is wider than its parent");
    label->SetName("ClippedLabel");
    label->SetPosition(0.0f, 0.0f);
    label->SetSize(220.0f, 30.0f);
    panel->AddChild(label);
    canvas.AddWidget(panel);

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.BeginRecordOnlyFrame(240, 120);
    canvas.Render(renderer);
    renderer.EndFrame();

    ASSERT_GE(renderer.GetCommands().size(), static_cast<size_t>(1));
    const UIDrawCommand& textCommand = renderer.GetCommands().back();
    EXPECT_EQ(UIDrawCommandType::Text, textCommand.type);
    EXPECT_FLOAT_EQ(20.0f, textCommand.clipRect.x);
    EXPECT_FLOAT_EQ(10.0f, textCommand.clipRect.y);
    EXPECT_FLOAT_EQ(80.0f, textCommand.clipRect.width);
    EXPECT_FLOAT_EQ(30.0f, textCommand.clipRect.height);
}

TEST(UIValidation, ScrollViewScrollsContentAndClipsRenderedChildren)
{
    FakeDevice device;
    UICanvas canvas;
    canvas.Initialize(180.0f, 120.0f);

    auto scrollView = ScrollView::Create();
    scrollView->SetName("Scroll");
    scrollView->SetPosition(10.0f, 10.0f);
    scrollView->SetSize(100.0f, 40.0f);
    scrollView->SetContentSize(100.0f, 120.0f);
    scrollView->SetShowScrollbars(false);
    scrollView->SetWheelStep(20.0f);

    auto label = Label::Create("Deep");
    label->SetName("Scroll.DeepLabel");
    label->SetPosition(4.0f, 48.0f);
    label->SetSize(80.0f, 20.0f);
    scrollView->AddContentChild(label);
    canvas.AddWidget(scrollView);

    UIEvent scroll;
    scroll.type = UIEventType::Scroll;
    scroll.position = Vec2(24.0f, 24.0f);
    scroll.delta = Vec2(0.0f, -1.0f);
    EXPECT_TRUE(canvas.HandleEvent(scroll));
    EXPECT_FLOAT_EQ(20.0f, scrollView->GetScrollOffset().y);

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.BeginRecordOnlyFrame(180, 120);
    canvas.Render(renderer);
    renderer.EndFrame();

    const UIDrawCommand* textCommand = nullptr;
    for (const UIDrawCommand& command : renderer.GetCommands())
    {
        if (command.type == UIDrawCommandType::Text && command.text == "Deep")
        {
            textCommand = &command;
            break;
        }
    }

    ASSERT_NE(nullptr, textCommand);
    EXPECT_FLOAT_EQ(14.0f, textCommand->clipRect.x);
    EXPECT_FLOAT_EQ(38.0f, textCommand->clipRect.y);
    EXPECT_FLOAT_EQ(80.0f, textCommand->clipRect.width);
    EXPECT_FLOAT_EQ(12.0f, textCommand->clipRect.height);
    EXPECT_FLOAT_EQ(38.0f, textCommand->bounds.y);
}

TEST(UIValidation, ScrollViewSynchronizesContentPositionWhenOffsetChanges)
{
    UICanvas canvas;
    canvas.Initialize(180.0f, 120.0f);

    auto scrollView = ScrollView::Create();
    scrollView->SetName("Scroll");
    scrollView->SetPosition(10.0f, 20.0f);
    scrollView->SetSize(100.0f, 40.0f);
    scrollView->SetShowScrollbars(false);
    scrollView->SetContentSize(100.0f, 140.0f);

    auto label = Label::Create("Deep");
    label->SetName("Scroll.DeepLabel");
    label->SetPosition(4.0f, 60.0f);
    label->SetSize(80.0f, 20.0f);
    scrollView->AddContentChild(label);
    canvas.AddWidget(scrollView);

    uint32 scrollChangedCount = 0;
    scrollView->SetOnScrollChanged([&scrollChangedCount](const Vec2& offset) {
        EXPECT_GE(offset.y, 0.0f);
        ++scrollChangedCount;
    });

    scrollView->SetScrollOffset(0.0f, 30.0f);
    EXPECT_EQ(1u, scrollChangedCount);
    EXPECT_FLOAT_EQ(30.0f, scrollView->GetScrollOffset().y);
    EXPECT_FLOAT_EQ(14.0f, label->GetGlobalRect().x);
    EXPECT_FLOAT_EQ(50.0f, label->GetGlobalRect().y);

    scrollView->SetScrollOffset(0.0f, 30.0f);
    EXPECT_EQ(1u, scrollChangedCount);
    EXPECT_FLOAT_EQ(50.0f, label->GetGlobalRect().y);

    scrollView->SetContentSize(100.0f, 40.0f);
    EXPECT_EQ(2u, scrollChangedCount);
    EXPECT_FLOAT_EQ(0.0f, scrollView->GetScrollOffset().y);
    EXPECT_FLOAT_EQ(80.0f, label->GetGlobalRect().y);
}

TEST(UIValidation, ScrollViewRendersScrollbarWidgetsAndClampsOffset)
{
    FakeDevice device;
    UICanvas canvas;
    canvas.Initialize(180.0f, 120.0f);

    auto scrollView = ScrollView::Create();
    scrollView->SetName("Scroll");
    scrollView->SetPosition(5.0f, 7.0f);
    scrollView->SetSize(100.0f, 40.0f);
    scrollView->SetScrollbarNamePrefix("Scroll.Scrollbar");
    scrollView->SetContentSize(92.0f, 120.0f);
    scrollView->SetScrollOffset(0.0f, 500.0f);
    canvas.AddWidget(scrollView);

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.BeginRecordOnlyFrame(180, 120);
    canvas.Render(renderer);
    renderer.EndFrame();

    EXPECT_FLOAT_EQ(80.0f, scrollView->GetScrollOffset().y);
    EXPECT_TRUE(scrollView->IsVerticalScrollbarVisible());
    EXPECT_FALSE(scrollView->IsHorizontalScrollbarVisible());

    Widget::Ptr thumb = canvas.FindWidget("Scroll.Scrollbar.Thumb");
    ASSERT_NE(nullptr, thumb);
    EXPECT_TRUE(thumb->IsVisible());

    const Rect thumbRect = thumb->GetGlobalRect();
    EXPECT_FLOAT_EQ(97.0f, thumbRect.x);
    EXPECT_FLOAT_EQ(29.0f, thumbRect.y);
    EXPECT_FLOAT_EQ(8.0f, thumbRect.width);
    EXPECT_FLOAT_EQ(18.0f, thumbRect.height);
}

TEST(UIValidation, LabelMeasureUsesFontMetricsInsteadOfFlatLengthMultiplier)
{
    const UIFontFallbackChain& font = UIFontFallbackChain::Default();
    const UITextMetrics wide = font.MeasureText("W", 20.0f);
    const UITextMetrics narrow = font.MeasureText("i", 20.0f);
    ASSERT_GT(wide.width, narrow.width);

    Label label("Wi");
    label.SetFontSize(20.0f);
    const Vec2 measured = label.MeasureContent();
    const UITextMetrics expected = font.MeasureText("Wi", 20.0f);

    EXPECT_FLOAT_EQ(expected.width, measured.x);
    EXPECT_FLOAT_EQ(expected.height, measured.y);
    EXPECT_NE(2.0f * 20.0f * 0.6f, measured.x);

    label.SetText("W\ni");
    const Vec2 multiline = label.MeasureContent();
    EXPECT_GT(multiline.y, measured.y);
}

TEST(UIValidation, LabelMeasureUsesFallbackChainForLocalizedText)
{
    constexpr uint32 RVX_TEST_CJK_CODEPOINT = 0x4E2Du;
    const UIFontFallbackChain& font = UIFontFallbackChain::Default();
    if (!font.HasCodepoint(RVX_TEST_CJK_CODEPOINT))
    {
        GTEST_SKIP() << "Default font fallback chain does not expose the tested CJK glyph";
    }

    const std::string text = std::string("A\xE4\xB8\xAD");
    Label label(text);
    label.SetFontSize(22.0f);

    const UITextMetrics expected = font.MeasureText(text, 22.0f);
    const Vec2 measured = label.MeasureContent();

    EXPECT_FLOAT_EQ(expected.width, measured.x);
    EXPECT_FLOAT_EQ(expected.height, measured.y);
    EXPECT_GT(measured.x, font.MeasureText("A", 22.0f).width);
}

TEST(UIValidation, TrueTypeFontMetricsLoadFromFontFile)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    UIFontMetrics font;
    ASSERT_FALSE(font.HasTrueTypeData());
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));
    ASSERT_TRUE(font.HasTrueTypeData());

    const UITextMetrics text = font.MeasureText("Runtime UI", 24.0f);
    EXPECT_GT(text.width, 0.0f);
    EXPECT_GT(text.height, 0.0f);
    EXPECT_EQ(10u, text.glyphCount);
    EXPECT_EQ(1u, text.lineCount);

    const UIGlyphMetrics wide = font.GetGlyphMetrics('W', 24.0f);
    const UIGlyphMetrics narrow = font.GetGlyphMetrics('i', 24.0f);
    EXPECT_GT(wide.advance, narrow.advance);
    EXPECT_GT(font.GetAscent(24.0f), 0.0f);
    EXPECT_GT(font.GetDescent(24.0f), 0.0f);
    EXPECT_FLOAT_EQ(font.GetAscent(24.0f) + font.GetDescent(24.0f) + font.GetLineGap(24.0f),
                    font.GetLineHeight(24.0f));
}

TEST(UIValidation, TrueTypeFontMetricsAppliesKerningToMeasuredText)
{
    constexpr float RVX_TEST_FONT_SIZE = 48.0f;
    const KerningFontSample sample = FindKerningFontSample(RVX_TEST_FONT_SIZE);
    if (sample.path.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate exposes a tested kern pair";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(sample.path.string()));

    std::string text;
    text.push_back(static_cast<char>(sample.previousCodepoint));
    text.push_back(static_cast<char>(sample.codepoint));

    const float unkernedWidth =
        font.GetCodepointMetrics(sample.previousCodepoint, RVX_TEST_FONT_SIZE).advance +
        font.GetCodepointMetrics(sample.codepoint, RVX_TEST_FONT_SIZE).advance;
    const UITextMetrics measured = font.MeasureText(text, RVX_TEST_FONT_SIZE);

    EXPECT_NEAR(unkernedWidth + sample.kerning, measured.width, 0.001f);
    EXPECT_NE(unkernedWidth, measured.width);
}

TEST(UIValidation, DefaultFontMetricsUsesSystemFontWhenAvailable)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    EXPECT_TRUE(UIFontMetrics::Default().HasTrueTypeData());
    EXPECT_GT(UIFontMetrics::Default().MeasureText("Default", 18.0f).width, 0.0f);
}

TEST(UIValidation, FontAtlasBuildsGlyphPixelsAndTextUsesInsetAtlasUVs)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 24.0f;
    atlasDesc.width = 512;
    atlasDesc.height = 512;
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));
    ASSERT_TRUE(atlas.IsBuilt());
    ASSERT_EQ(static_cast<size_t>(atlasDesc.width) * atlasDesc.height, atlas.GetPixels().size());
    EXPECT_TRUE(std::any_of(atlas.GetPixels().begin(),
                            atlas.GetPixels().end(),
                            [](uint8 value) { return value > 0; }));

    const UIFontAtlasGlyph* wideGlyph = atlas.FindGlyph('W');
    ASSERT_NE(nullptr, wideGlyph);
    EXPECT_GT(wideGlyph->uvRect.width, 0.0f);
    EXPECT_GT(wideGlyph->uvRect.height, 0.0f);
    EXPECT_LT(wideGlyph->uvRect.Right(), 1.0f);
    EXPECT_LT(wideGlyph->uvRect.Bottom(), 1.0f);
    EXPECT_GE(atlas.GetDesc().padding, 2u);

    const uint32 glyphLeft =
        static_cast<uint32>(std::round(wideGlyph->uvRect.Left() * atlas.GetWidth()));
    const uint32 glyphTop =
        static_cast<uint32>(std::round(wideGlyph->uvRect.Top() * atlas.GetHeight()));
    const uint32 glyphBottom = static_cast<uint32>(
        std::round(wideGlyph->uvRect.Bottom() * atlas.GetHeight()));
    bool leftPaddingTransparent = glyphLeft == 0u;
    if (glyphLeft > 0u)
    {
        leftPaddingTransparent = true;
        for (uint32 y = glyphTop; y < glyphBottom; ++y)
        {
            if (atlas.GetPixels()[static_cast<size_t>(y) * atlas.GetWidth() +
                                  glyphLeft - 1u] != 0u)
            {
                leftPaddingTransparent = false;
                break;
            }
        }
    }
    EXPECT_TRUE(leftPaddingTransparent);

    FakeDevice device;
    auto targetView = CreateView(RHITextureViewType::RenderTarget);

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);
    renderer.BeginFrame({256, 128, RHIFormat::RGBA8_UNORM, targetView.Get()});
    renderer.DrawText("Wi", Rect(0.0f, 0.0f, 128.0f, 64.0f), 24.0f, UIColor::White());
    renderer.EndFrame();

    ASSERT_GE(renderer.GetVertices().size(), 4u);
    const float halfTexelU = 0.5f / static_cast<float>(atlas.GetWidth());
    const float halfTexelV = 0.5f / static_cast<float>(atlas.GetHeight());
    EXPECT_FLOAT_EQ(wideGlyph->uvRect.Left() + halfTexelU,
                    renderer.GetVertices()[0].uv.x);
    EXPECT_FLOAT_EQ(wideGlyph->uvRect.Top() + halfTexelV,
                    renderer.GetVertices()[0].uv.y);
    EXPECT_FLOAT_EQ(wideGlyph->uvRect.Right() - halfTexelU,
                    renderer.GetVertices()[1].uv.x);
    EXPECT_FLOAT_EQ(wideGlyph->uvRect.Bottom() - halfTexelV,
                    renderer.GetVertices()[2].uv.y);
}

TEST(UIValidation, FontAtlasBuildsExplicitNonContiguousCodepointSet)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 24.0f;
    atlasDesc.width = 512;
    atlasDesc.height = 512;
    atlasDesc.firstCodepoint = 0;
    atlasDesc.codepointCount = 0;
    atlasDesc.codepoints = {'A', 'Z', '?', 'A'};
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));

    EXPECT_TRUE(atlas.IsBuilt());
    EXPECT_EQ(3u, atlas.GetDesc().codepointCount);
    ASSERT_EQ(3u, atlas.GetDesc().codepoints.size());
    EXPECT_EQ(static_cast<uint32>('A'), atlas.GetDesc().codepoints[0]);
    EXPECT_EQ(static_cast<uint32>('Z'), atlas.GetDesc().codepoints[1]);
    EXPECT_EQ(static_cast<uint32>('?'), atlas.GetDesc().codepoints[2]);
    EXPECT_EQ(3u, atlas.GetGlyphCount());
    EXPECT_NE(nullptr, atlas.FindGlyph('A'));
    EXPECT_NE(nullptr, atlas.FindGlyph('Z'));
    EXPECT_NE(nullptr, atlas.FindGlyph('?'));
    EXPECT_EQ(nullptr, atlas.FindGlyph('B'));
}

TEST(UIValidation, FontAtlasBuildsLocalizedGlyphWhenFontProvidesCodepoint)
{
    constexpr uint32 RVX_TEST_CJK_CODEPOINT = 0x4E2Du;
    const std::filesystem::path fontPath =
        FindFontPathWithCodepoint(RVX_TEST_CJK_CODEPOINT);
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system font candidate exposes the tested CJK glyph";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));
    ASSERT_TRUE(font.HasCodepoint(RVX_TEST_CJK_CODEPOINT));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 36.0f;
    atlasDesc.width = 1024;
    atlasDesc.height = 1024;
    atlasDesc.padding = 4;
    atlasDesc.oversampleX = 3;
    atlasDesc.oversampleY = 3;
    atlasDesc.codepoints = {'R', 'V', 'X', RVX_TEST_CJK_CODEPOINT};
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));

    const UIFontAtlasGlyph* glyph =
        atlas.FindGlyph(RVX_TEST_CJK_CODEPOINT);
    ASSERT_NE(nullptr, glyph);
    EXPECT_GT(glyph->uvRect.width, 0.0f);
    EXPECT_GT(glyph->uvRect.height, 0.0f);
    EXPECT_GT(glyph->size.x, 0.0f);
    EXPECT_GT(glyph->size.y, 0.0f);

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);
    renderer.BeginRecordOnlyFrame(256, 128);
    renderer.DrawText(std::string("\xE4\xB8\xAD"),
                      Rect(8.0f, 8.0f, 180.0f, 64.0f),
                      36.0f,
                      UIColor::White());
    renderer.EndFrame();

    EXPECT_EQ(1u, renderer.GetStats().textCount);
    EXPECT_EQ(1u, renderer.GetStats().glyphCount);
    ASSERT_FALSE(renderer.GetCommands().empty());
    EXPECT_EQ(UIDrawCommandType::Text, renderer.GetCommands().back().type);
    EXPECT_GE(renderer.GetCommands().back().vertexCount, 4u);
}

TEST(UIValidation, FontFallbackChainSelectsFirstFontThatProvidesCodepoint)
{
    constexpr uint32 RVX_TEST_CJK_CODEPOINT = 0x4E2Du;
    const std::filesystem::path primaryPath =
        FindFontPathWithoutCodepoint(RVX_TEST_CJK_CODEPOINT);
    const std::filesystem::path fallbackPath =
        FindFontPathWithCodepoint(RVX_TEST_CJK_CODEPOINT);
    if (primaryPath.empty() || fallbackPath.empty())
    {
        GTEST_SKIP() << "No primary/fallback font pair is available";
    }

    UIFontMetrics primaryFont;
    UIFontMetrics fallbackFont;
    ASSERT_TRUE(primaryFont.LoadTrueTypeFile(primaryPath.string()));
    ASSERT_TRUE(fallbackFont.LoadTrueTypeFile(fallbackPath.string()));
    ASSERT_FALSE(primaryFont.HasCodepoint(RVX_TEST_CJK_CODEPOINT));
    ASSERT_TRUE(fallbackFont.HasCodepoint(RVX_TEST_CJK_CODEPOINT));

    UIFontFallbackChain chain;
    ASSERT_TRUE(chain.AddFont(std::move(primaryFont)));
    ASSERT_TRUE(chain.AddFont(std::move(fallbackFont)));

    ASSERT_EQ(2u, chain.GetFontCount());
    ASSERT_NE(nullptr, chain.GetPrimaryFont());
    EXPECT_EQ(chain.GetPrimaryFont(), chain.FindFontForCodepoint('A'));

    const UIFontMetrics* localizedFont =
        chain.FindFontForCodepoint(RVX_TEST_CJK_CODEPOINT);
    ASSERT_NE(nullptr, localizedFont);
    EXPECT_NE(chain.GetPrimaryFont(), localizedFont);
    EXPECT_TRUE(chain.HasCodepoint('A'));
    EXPECT_TRUE(chain.HasCodepoint(RVX_TEST_CJK_CODEPOINT));

    const UIGlyphMetrics localizedMetrics =
        chain.GetCodepointMetrics(RVX_TEST_CJK_CODEPOINT, 32.0f);
    EXPECT_GT(localizedMetrics.advance, 0.0f);

    const UITextMetrics textMetrics =
        chain.MeasureText(std::string("A\xE4\xB8\xAD"), 32.0f);
    EXPECT_EQ(2u, textMetrics.glyphCount);
    EXPECT_GT(textMetrics.width, localizedMetrics.advance);
}

TEST(UIValidation, FontFallbackChainBuildsAtlasAcrossPrimaryAndFallbackFonts)
{
    constexpr uint32 RVX_TEST_CJK_CODEPOINT = 0x4E2Du;
    const std::filesystem::path primaryPath =
        FindFontPathWithoutCodepoint(RVX_TEST_CJK_CODEPOINT);
    const std::filesystem::path fallbackPath =
        FindFontPathWithCodepoint(RVX_TEST_CJK_CODEPOINT);
    if (primaryPath.empty() || fallbackPath.empty())
    {
        GTEST_SKIP() << "No primary/fallback font pair is available";
    }

    UIFontMetrics primaryFont;
    UIFontMetrics fallbackFont;
    ASSERT_TRUE(primaryFont.LoadTrueTypeFile(primaryPath.string()));
    ASSERT_TRUE(fallbackFont.LoadTrueTypeFile(fallbackPath.string()));

    UIFontFallbackChain chain;
    ASSERT_TRUE(chain.AddFont(std::move(primaryFont)));
    ASSERT_TRUE(chain.AddFont(std::move(fallbackFont)));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 40.0f;
    atlasDesc.width = 1024;
    atlasDesc.height = 1024;
    atlasDesc.padding = 4;
    atlasDesc.oversampleX = 3;
    atlasDesc.oversampleY = 3;
    atlasDesc.codepoints = {'A', RVX_TEST_CJK_CODEPOINT};
    ASSERT_TRUE(chain.BuildAtlas(atlas, atlasDesc));

    const UIFontAtlasGlyph* latinGlyph = atlas.FindGlyph('A');
    const UIFontAtlasGlyph* localizedGlyph =
        atlas.FindGlyph(RVX_TEST_CJK_CODEPOINT);
    ASSERT_NE(nullptr, latinGlyph);
    ASSERT_NE(nullptr, localizedGlyph);
    EXPECT_EQ(0u, latinGlyph->fontIndex);
    EXPECT_EQ(1u, localizedGlyph->fontIndex);
    EXPECT_EQ(2u, atlas.GetGlyphCount());
    EXPECT_TRUE(std::any_of(atlas.GetPixels().begin(),
                            atlas.GetPixels().end(),
                            [](uint8 value) { return value > 0u; }));

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);
    renderer.BeginRecordOnlyFrame(320, 160);
    renderer.DrawText(std::string("A\xE4\xB8\xAD"),
                      Rect(8.0f, 8.0f, 240.0f, 80.0f),
                      40.0f,
                      UIColor::White());
    renderer.EndFrame();

    EXPECT_EQ(1u, renderer.GetStats().textCount);
    EXPECT_EQ(2u, renderer.GetStats().glyphCount);
    EXPECT_GE(renderer.GetStats().vertexCount, 8u);
}

TEST(UIValidation, DefaultFontFallbackChainLoadsSystemAndLocalizedFonts)
{
    constexpr uint32 RVX_TEST_CJK_CODEPOINT = 0x4E2Du;
    const UIFontFallbackChain& chain = UIFontFallbackChain::Default();

    ASSERT_NE(nullptr, chain.GetPrimaryFont());
    EXPECT_GE(chain.GetFontCount(), 1u);
    EXPECT_TRUE(chain.HasCodepoint('A'));

    if (!FindFontPathWithCodepoint(RVX_TEST_CJK_CODEPOINT).empty())
    {
        EXPECT_TRUE(chain.HasCodepoint(RVX_TEST_CJK_CODEPOINT));
    }
}

TEST(UIValidation, DefaultFontFallbackChainCanBeConfiguredFromExplicitFontPaths)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    const uint64 previousGeneration = UIFontFallbackChain::GetDefaultGeneration();

    UIFontFallbackChainDesc desc;
    desc.appendDefaultSystemFonts = false;
    desc.fontPaths.push_back(fontPath.string());
    std::string error;
    ASSERT_TRUE(UIFontFallbackChain::ConfigureDefault(desc, &error));
    EXPECT_TRUE(error.empty());
    EXPECT_GT(UIFontFallbackChain::GetDefaultGeneration(), previousGeneration);

    const UIFontFallbackChain& chain = UIFontFallbackChain::Default();
    EXPECT_EQ(1u, chain.GetFontCount());
    EXPECT_TRUE(chain.HasCodepoint('A'));

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    const UIFontAtlas* atlas = renderer.GetFontAtlas();
    ASSERT_NE(nullptr, atlas);
    EXPECT_TRUE(atlas->IsBuilt());
    EXPECT_NE(nullptr, atlas->FindGlyph('A'));

    std::string resetError;
    EXPECT_TRUE(UIFontFallbackChain::ResetDefault(&resetError));
    EXPECT_TRUE(resetError.empty());
}

TEST(UIValidation, DefaultRendererFontAtlasUsesFallbackSmokeGlyphs)
{
    constexpr uint32 RVX_TEST_CJK_CODEPOINT = 0x4E2Du;

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));

    const UIFontAtlas* atlas = renderer.GetFontAtlas();
    ASSERT_NE(nullptr, atlas);
    ASSERT_TRUE(atlas->IsBuilt());
    ASSERT_NE(nullptr, atlas->FindGlyph('A'));

    const auto containsCodepoint =
        [atlas](uint32 codepoint) {
            return std::find(atlas->GetDesc().codepoints.begin(),
                             atlas->GetDesc().codepoints.end(),
                             codepoint) != atlas->GetDesc().codepoints.end();
        };

    EXPECT_TRUE(containsCodepoint(0x2026u));
    EXPECT_TRUE(containsCodepoint(RVX_TEST_CJK_CODEPOINT));

    const UIFontFallbackChain& chain = UIFontFallbackChain::Default();
    if (chain.HasCodepoint(0x2026u))
    {
        EXPECT_NE(nullptr, atlas->FindGlyph(0x2026u));
    }

    if (!chain.HasCodepoint(RVX_TEST_CJK_CODEPOINT))
    {
        GTEST_SKIP() << "Default font fallback chain has no tested CJK glyph";
    }

    ASSERT_NE(nullptr, atlas->FindGlyph(RVX_TEST_CJK_CODEPOINT));

    renderer.BeginRecordOnlyFrame(320, 160);
    renderer.DrawText(std::string("A\xE4\xB8\xAD"),
                      Rect(8.0f, 8.0f, 240.0f, 80.0f),
                      32.0f,
                      UIColor::White());
    renderer.EndFrame();

    EXPECT_EQ(1u, renderer.GetStats().textCount);
    EXPECT_EQ(2u, renderer.GetStats().glyphCount);
    EXPECT_GE(renderer.GetStats().vertexCount, 8u);
}

TEST(UIValidation, RendererExpandsDefaultFontAtlasAtFrameBoundaryForMissingGlyphs)
{
    constexpr uint32 RVX_TEST_DYNAMIC_CJK_CODEPOINT = 0x6587u;
    if (!UIFontFallbackChain::Default().HasCodepoint(RVX_TEST_DYNAMIC_CJK_CODEPOINT))
    {
        GTEST_SKIP() << "Default font fallback chain has no tested dynamic CJK glyph";
    }

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));

    const UIFontAtlas* initialAtlas = renderer.GetFontAtlas();
    ASSERT_NE(nullptr, initialAtlas);
    ASSERT_EQ(nullptr, initialAtlas->FindGlyph(RVX_TEST_DYNAMIC_CJK_CODEPOINT));

    renderer.BeginRecordOnlyFrame(320, 160);
    renderer.DrawText(std::string("\xE6\x96\x87"),
                      Rect(8.0f, 8.0f, 240.0f, 80.0f),
                      32.0f,
                      UIColor::White());
    EXPECT_TRUE(renderer.GetFontAtlasRuntimeStats().rebuildPending);
    EXPECT_EQ(1u, renderer.GetFontAtlasRuntimeStats().pendingCodepointCount);
    renderer.EndFrame();

    const UIFontAtlasRuntimeStats& stats = renderer.GetFontAtlasRuntimeStats();
    EXPECT_TRUE(stats.usesRuntimeAtlas);
    EXPECT_FALSE(stats.rebuildPending);
    EXPECT_EQ(0u, stats.pendingCodepointCount);
    EXPECT_EQ(1u, stats.rebuildCount);

    const UIFontAtlas* expandedAtlas = renderer.GetFontAtlas();
    ASSERT_NE(nullptr, expandedAtlas);
    EXPECT_NE(initialAtlas, expandedAtlas);
    EXPECT_NE(nullptr, expandedAtlas->FindGlyph(RVX_TEST_DYNAMIC_CJK_CODEPOINT));

    renderer.BeginRecordOnlyFrame(320, 160);
    renderer.DrawText(std::string("\xE6\x96\x87"),
                      Rect(8.0f, 8.0f, 240.0f, 80.0f),
                      32.0f,
                      UIColor::White());
    renderer.EndFrame();
    EXPECT_EQ(1u, renderer.GetStats().textCount);
    EXPECT_EQ(1u, renderer.GetStats().glyphCount);
    EXPECT_GE(renderer.GetStats().vertexCount, 4u);
    EXPECT_EQ(1u, renderer.GetFontAtlasRuntimeStats().rebuildCount);
}

TEST(UIValidation, RendererDoesNotMutateExplicitFontAtlasForMissingGlyphs)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 24.0f;
    atlasDesc.width = 256;
    atlasDesc.height = 256;
    atlasDesc.codepoints = {'A'};
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);

    renderer.BeginRecordOnlyFrame(320, 160);
    renderer.DrawText("B", Rect(8.0f, 8.0f, 240.0f, 80.0f), 24.0f, UIColor::White());
    renderer.EndFrame();

    EXPECT_EQ(&atlas, renderer.GetFontAtlas());
    EXPECT_FALSE(renderer.GetFontAtlasRuntimeStats().usesRuntimeAtlas);
    EXPECT_EQ(0u, renderer.GetFontAtlasRuntimeStats().rebuildCount);
    EXPECT_EQ(0u, renderer.GetFontAtlasRuntimeStats().pendingCodepointCount);
    EXPECT_EQ(nullptr, atlas.FindGlyph('B'));
}

TEST(UIValidation, TextUsesPackedAtlasGlyphGeometryWhenScaled)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 32.0f;
    atlasDesc.width = 512;
    atlasDesc.height = 512;
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));

    const UIFontAtlasGlyph* glyph = atlas.FindGlyph('H');
    ASSERT_NE(nullptr, glyph);

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);

    constexpr float RVX_TEST_FONT_SIZE = 24.0f;
    const Rect bounds(8.0f, 10.0f, 128.0f, 64.0f);
    renderer.BeginRecordOnlyFrame(256, 128, RHIFormat::RGBA8_UNORM);
    renderer.DrawText("H", bounds, RVX_TEST_FONT_SIZE, UIColor::White());
    renderer.EndFrame();

    ASSERT_GE(renderer.GetVertices().size(), 4u);
    const float atlasScale = RVX_TEST_FONT_SIZE / atlasDesc.fontSize;
    const float baselineY = bounds.y + font.GetAscent(RVX_TEST_FONT_SIZE);
    const Rect expectedGlyphRect = GlyphRectForExpectation(
        Rect(bounds.x + glyph->offset.x * atlasScale,
             baselineY + glyph->offset.y * atlasScale,
             glyph->size.x * atlasScale,
             glyph->size.y * atlasScale));

    EXPECT_NEAR(expectedGlyphRect.Left(), renderer.GetVertices()[0].position.x, 0.001f);
    EXPECT_NEAR(expectedGlyphRect.Top(), renderer.GetVertices()[0].position.y, 0.001f);
    EXPECT_NEAR(expectedGlyphRect.Right(), renderer.GetVertices()[1].position.x, 0.001f);
    EXPECT_NEAR(expectedGlyphRect.Bottom(), renderer.GetVertices()[2].position.y, 0.001f);
}

TEST(UIValidation, TextRendererSnapsPackedGlyphQuadsToPixelGrid)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 64.0f;
    atlasDesc.width = 512;
    atlasDesc.height = 512;
    atlasDesc.codepoints = {'H'};
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);

    renderer.BeginRecordOnlyFrame(320, 160, RHIFormat::RGBA8_UNORM);
    renderer.DrawText("H", Rect(8.35f, 10.45f, 128.0f, 64.0f), 23.5f, UIColor::White());
    renderer.EndFrame();

    ASSERT_GE(renderer.GetVertices().size(), 4u);
    for (uint32 i = 0; i < 4u; ++i)
    {
        EXPECT_FLOAT_EQ(std::round(renderer.GetVertices()[i].position.x),
                        renderer.GetVertices()[i].position.x);
        EXPECT_FLOAT_EQ(std::round(renderer.GetVertices()[i].position.y),
                        renderer.GetVertices()[i].position.y);
    }
    EXPECT_GT(renderer.GetVertices()[1].position.x,
              renderer.GetVertices()[0].position.x);
    EXPECT_GT(renderer.GetVertices()[2].position.y,
              renderer.GetVertices()[1].position.y);
}

TEST(UIValidation, TextRendererAdvancesWithPackedAtlasGlyphScale)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 64.0f;
    atlasDesc.width = 512;
    atlasDesc.height = 512;
    atlasDesc.codepoints = {'W', 'i'};
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));

    const UIFontAtlasGlyph* firstGlyph = atlas.FindGlyph('W');
    const UIFontAtlasGlyph* secondGlyph = atlas.FindGlyph('i');
    ASSERT_NE(nullptr, firstGlyph);
    ASSERT_NE(nullptr, secondGlyph);

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);

    constexpr float RVX_TEST_FONT_SIZE = 24.0f;
    const Rect bounds(8.0f, 10.0f, 180.0f, 64.0f);
    renderer.BeginRecordOnlyFrame(320, 160, RHIFormat::RGBA8_UNORM);
    renderer.DrawText("Wi", bounds, RVX_TEST_FONT_SIZE, UIColor::White());
    renderer.EndFrame();

    ASSERT_GE(renderer.GetVertices().size(), 8u);
    ASSERT_FALSE(renderer.GetCommands().empty());

    const float atlasScale = RVX_TEST_FONT_SIZE / atlasDesc.fontSize;
    const float kerning =
        font.GetKerningAdvance('W', 'i', RVX_TEST_FONT_SIZE);
    const float baselineY = bounds.y + font.GetAscent(RVX_TEST_FONT_SIZE);
    const float rawSecondLeft =
        bounds.x +
        firstGlyph->advance * atlasScale +
        kerning +
        secondGlyph->offset.x * atlasScale;
    const Rect expectedSecondRect = GlyphRectForExpectation(
        Rect(rawSecondLeft,
             baselineY + secondGlyph->offset.y * atlasScale,
             secondGlyph->size.x * atlasScale,
             secondGlyph->size.y * atlasScale));
    const float expectedWidth =
        firstGlyph->advance * atlasScale +
        kerning +
        secondGlyph->advance * atlasScale;

    EXPECT_NEAR(expectedSecondRect.Left(),
                renderer.GetVertices()[4].position.x,
                0.001f);
    EXPECT_NEAR(expectedSecondRect.Top(),
                renderer.GetVertices()[4].position.y,
                0.001f);
    EXPECT_NEAR(expectedWidth,
                renderer.GetCommands().back().bounds.width,
                0.001f);
}

TEST(UIValidation, TextRendererPositionsPackedGlyphsWithKerning)
{
    constexpr float RVX_TEST_FONT_SIZE = 48.0f;
    const KerningFontSample sample = FindKerningFontSample(RVX_TEST_FONT_SIZE);
    if (sample.path.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate exposes a tested kern pair";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(sample.path.string()));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = RVX_TEST_FONT_SIZE;
    atlasDesc.width = 512;
    atlasDesc.height = 512;
    atlasDesc.codepoints = {
        sample.previousCodepoint,
        sample.codepoint};
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));

    const UIFontAtlasGlyph* firstGlyph = atlas.FindGlyph(sample.previousCodepoint);
    const UIFontAtlasGlyph* secondGlyph = atlas.FindGlyph(sample.codepoint);
    ASSERT_NE(nullptr, firstGlyph);
    ASSERT_NE(nullptr, secondGlyph);

    FakeDevice device;
    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);

    std::string text;
    text.push_back(static_cast<char>(sample.previousCodepoint));
    text.push_back(static_cast<char>(sample.codepoint));

    const Rect bounds(8.0f, 10.0f, 160.0f, 64.0f);
    renderer.BeginRecordOnlyFrame(320, 160, RHIFormat::RGBA8_UNORM);
    renderer.DrawText(text, bounds, RVX_TEST_FONT_SIZE, UIColor::White());
    renderer.EndFrame();

    ASSERT_GE(renderer.GetVertices().size(), 8u);
    const float atlasScale = RVX_TEST_FONT_SIZE / atlasDesc.fontSize;
    const float rawSecondLeft =
        bounds.x +
        firstGlyph->advance * atlasScale +
        font.GetKerningAdvance(sample.previousCodepoint,
                               sample.codepoint,
                               RVX_TEST_FONT_SIZE) +
        secondGlyph->offset.x * atlasScale;
    const float baselineY = bounds.y + font.GetAscent(RVX_TEST_FONT_SIZE);
    const Rect expectedSecondRect = GlyphRectForExpectation(
        Rect(rawSecondLeft,
             baselineY + secondGlyph->offset.y * atlasScale,
             secondGlyph->size.x * atlasScale,
             secondGlyph->size.y * atlasScale));

    EXPECT_NEAR(expectedSecondRect.Left(),
                renderer.GetVertices()[4].position.x,
                0.001f);
}

TEST(UIValidation, DefaultFontAtlasUploadsHighResolutionTextureForScaledText)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    FakeDevice device;
    FakeCommandContext context;
    auto pipeline = MakeRef<FakePipeline>();
    auto targetView = CreateView(RHITextureViewType::RenderTarget);

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    ASSERT_NE(nullptr, renderer.GetFontAtlas());
    EXPECT_FLOAT_EQ(64.0f, renderer.GetFontAtlas()->GetDesc().fontSize);
    EXPECT_GE(renderer.GetFontAtlas()->GetDesc().fontSize,
              UITheme::RuntimeDark().metrics.largeFontSize * 3.0f);
    EXPECT_GE(renderer.GetFontAtlas()->GetDesc().padding, 4u);
    EXPECT_GE(renderer.GetFontAtlas()->GetDesc().oversampleX, 3u);
    EXPECT_GE(renderer.GetFontAtlas()->GetDesc().oversampleY, 3u);
    renderer.BeginFrame({320, 180, RHIFormat::RGBA8_UNORM, targetView.Get()});
    renderer.DrawText("Scaled", Rect(8.0f, 16.0f, 260.0f, 72.0f), 54.0f, UIColor::White());
    renderer.EndFrame();

    UIRenderSubmitDesc submitDesc;
    submitDesc.commandContext = &context;
    submitDesc.pipeline = pipeline.Get();
    ASSERT_TRUE(renderer.Submit(submitDesc));

    ASSERT_EQ(2u, device.createdTextureDescs.size());
    EXPECT_EQ(2048u, device.createdTextureDescs[1].width);
    EXPECT_EQ(2048u, device.createdTextureDescs[1].height);
    ASSERT_EQ(2u, device.createdStagingBufferDescs.size());
    EXPECT_EQ(2048u * 2048u * 4u, device.createdStagingBufferDescs[1].size);
}

TEST(UIValidation, SubmitUploadsAtlasAndDrawsCommandsThroughRHI)
{
    const std::filesystem::path fontPath = FindSystemFontPath();
    if (fontPath.empty())
    {
        GTEST_SKIP() << "No system TrueType font candidate is available";
    }

    UIFontMetrics font;
    ASSERT_TRUE(font.LoadTrueTypeFile(fontPath.string()));

    UIFontAtlas atlas;
    UIFontAtlasDesc atlasDesc;
    atlasDesc.fontSize = 20.0f;
    atlasDesc.width = 256;
    atlasDesc.height = 256;
    atlasDesc.codepoints = {'U', 'I'};
    ASSERT_TRUE(font.BuildAtlas(atlas, atlasDesc));

    FakeDevice device;
    FakeCommandContext context;
    auto pipeline = MakeRef<FakePipeline>();
    auto targetView = CreateView(RHITextureViewType::RenderTarget);
    auto imageView = CreateView(RHITextureViewType::ShaderResource);

    UIRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(&device));
    renderer.SetFontAtlas(&atlas);
    renderer.BeginFrame({320, 180, RHIFormat::RGBA8_UNORM, targetView.Get()});
    renderer.DrawRect(Rect(8.0f, 8.0f, 64.0f, 24.0f), UIColor::Red());
    renderer.DrawText("UI", Rect(8.0f, 40.0f, 96.0f, 32.0f), 20.0f, UIColor::White());
    renderer.DrawImage(imageView.Get(), Rect(80.0f, 8.0f, 32.0f, 32.0f));
    renderer.EndFrame();

    UIRenderSubmitDesc submitDesc;
    submitDesc.commandContext = &context;
    submitDesc.pipeline = pipeline.Get();
    submitDesc.colorLoadOp = RHILoadOp::Clear;
    submitDesc.clearColor = {0.10f, 0.11f, 0.12f, 1.0f};
    ASSERT_TRUE(renderer.Submit(submitDesc));

    const UIRenderSubmitStats& submitStats = renderer.GetSubmitStats();
    EXPECT_TRUE(submitStats.submitted);
    EXPECT_TRUE(submitStats.whiteTextureUploaded);
    EXPECT_TRUE(submitStats.atlasUploaded);
    EXPECT_EQ(3u, submitStats.drawCallCount);
    EXPECT_EQ(renderer.GetVertices().size() * sizeof(UIVertex), submitStats.vertexBytesUploaded);
    EXPECT_EQ(renderer.GetIndices().size() * sizeof(uint32), submitStats.indexBytesUploaded);
    EXPECT_EQ(static_cast<uint64>(atlas.GetPixels().size() * 4u), submitStats.atlasBytesUploaded);
    EXPECT_EQ(4u, submitStats.whiteTextureBytesUploaded);

    ASSERT_EQ(2u, device.createdSamplerDescs.size());
    const RHISamplerDesc& textureSamplerDesc = device.createdSamplerDescs[0];
    EXPECT_EQ(RHIFilterMode::Linear, textureSamplerDesc.minFilter);
    EXPECT_EQ(RHIFilterMode::Linear, textureSamplerDesc.magFilter);
    EXPECT_EQ(RHIFilterMode::Nearest, textureSamplerDesc.mipFilter);
    EXPECT_FLOAT_EQ(0.0f, textureSamplerDesc.maxLod);

    const RHISamplerDesc& fontSamplerDesc = device.createdSamplerDescs[1];
    EXPECT_EQ(RHIFilterMode::Linear, fontSamplerDesc.minFilter);
    EXPECT_EQ(RHIFilterMode::Linear, fontSamplerDesc.magFilter);
    EXPECT_EQ(RHIFilterMode::Nearest, fontSamplerDesc.mipFilter);
    EXPECT_FLOAT_EQ(0.0f, fontSamplerDesc.maxLod);

    ASSERT_EQ(2u, device.createdBufferDescs.size());
    EXPECT_EQ(RHIBufferUsage::Vertex, device.createdBufferDescs[0].usage);
    EXPECT_EQ(RHIMemoryType::Upload, device.createdBufferDescs[0].memoryType);
    EXPECT_EQ(sizeof(UIVertex), device.createdBufferDescs[0].stride);
    EXPECT_EQ(RHIBufferUsage::Index, device.createdBufferDescs[1].usage);
    EXPECT_EQ(RHIMemoryType::Upload, device.createdBufferDescs[1].memoryType);
    EXPECT_EQ(0u, device.createdBufferDescs[1].stride);

    ASSERT_EQ(2u, device.createdTextureDescs.size());
    EXPECT_EQ(RHIFormat::RGBA8_UNORM, device.createdTextureDescs[0].format);
    EXPECT_EQ(1u, device.createdTextureDescs[0].width);
    EXPECT_EQ(1u, device.createdTextureDescs[0].height);
    EXPECT_EQ(RHIFormat::RGBA8_UNORM, device.createdTextureDescs[1].format);
    EXPECT_EQ(atlasDesc.width, device.createdTextureDescs[1].width);
    EXPECT_EQ(atlasDesc.height, device.createdTextureDescs[1].height);

    ASSERT_EQ(2u, context.textureCopies.size());
    EXPECT_EQ(4u, context.textureCopies[0].bufferRowPitch);
    EXPECT_EQ(atlasDesc.width * 4u, context.textureCopies[1].bufferRowPitch);
    EXPECT_EQ(atlasDesc.height, context.textureCopies[1].bufferImageHeight);

    EXPECT_EQ(3u, device.createdDescriptorSetDescs.size());
    EXPECT_EQ(1u, device.createdDescriptorSetLayoutDescs.size());
    ASSERT_EQ(1u, device.createdDescriptorSetLayoutDescs[0].entries.size());
    EXPECT_EQ(RHIBindingType::CombinedTextureSampler,
              device.createdDescriptorSetLayoutDescs[0].entries[0].type);
    ASSERT_EQ(1u, device.createdDescriptorSetDescs[0].bindings.size());
    ASSERT_EQ(1u, device.createdDescriptorSetDescs[1].bindings.size());
    ASSERT_EQ(1u, device.createdDescriptorSetDescs[2].bindings.size());
    EXPECT_EQ(device.createdDescriptorSetDescs[0].bindings[0].sampler,
              device.createdDescriptorSetDescs[2].bindings[0].sampler);
    EXPECT_NE(device.createdDescriptorSetDescs[0].bindings[0].sampler,
              device.createdDescriptorSetDescs[1].bindings[0].sampler);

    EXPECT_NE(nullptr, renderer.GetWhiteTextureView());
    EXPECT_NE(nullptr, renderer.GetFontAtlasTextureView());
    EXPECT_NE(nullptr, renderer.GetVertexBuffer());
    EXPECT_NE(nullptr, renderer.GetIndexBuffer());

    EXPECT_EQ(2u, std::count(context.calls.begin(), context.calls.end(), "CopyBufferToTexture"));
    EXPECT_EQ(1u, std::count(context.calls.begin(), context.calls.end(), "BeginRenderPass"));
    EXPECT_EQ(1u, std::count(context.calls.begin(), context.calls.end(), "SetPipeline"));
    EXPECT_EQ(1u, std::count(context.calls.begin(), context.calls.end(), "SetVertexBuffer"));
    EXPECT_EQ(1u, std::count(context.calls.begin(), context.calls.end(), "SetIndexBuffer"));
    EXPECT_EQ(3u, std::count(context.calls.begin(), context.calls.end(), "SetDescriptorSet"));
    EXPECT_EQ(3u, std::count(context.calls.begin(), context.calls.end(), "DrawIndexed"));
    EXPECT_EQ(1u, std::count(context.calls.begin(), context.calls.end(), "EndRenderPass"));
    ASSERT_EQ(1u, context.renderPassDesc.colorAttachmentCount);
    EXPECT_EQ(RHILoadOp::Clear,
              context.renderPassDesc.colorAttachments[0].loadOp);
    EXPECT_FLOAT_EQ(0.10f,
                    context.renderPassDesc.colorAttachments[0].clearColor.r);
    EXPECT_FLOAT_EQ(0.11f,
                    context.renderPassDesc.colorAttachments[0].clearColor.g);
    EXPECT_FLOAT_EQ(0.12f,
                    context.renderPassDesc.colorAttachments[0].clearColor.b);

    ASSERT_EQ(3u, context.drawIndexedCalls.size());
    ASSERT_EQ(3u, renderer.GetCommands().size());
    EXPECT_EQ(renderer.GetCommands()[0].indexCount, context.drawIndexedCalls[0].indexCount);
    EXPECT_EQ(renderer.GetCommands()[1].indexCount, context.drawIndexedCalls[1].indexCount);
    EXPECT_EQ(renderer.GetCommands()[2].indexCount, context.drawIndexedCalls[2].indexCount);
    EXPECT_EQ(renderer.GetCommands()[2].firstIndex, context.drawIndexedCalls[2].firstIndex);
    EXPECT_EQ(pipeline.Get(), context.boundPipeline);
    EXPECT_EQ(renderer.GetVertexBuffer(), context.boundVertexBuffer);
    EXPECT_EQ(renderer.GetIndexBuffer(), context.boundIndexBuffer);
    EXPECT_EQ(RHIFormat::R32_UINT, context.indexFormat);
    EXPECT_EQ(320.0f, context.boundViewport.width);
    EXPECT_EQ(180.0f, context.boundViewport.height);
    EXPECT_EQ(320u, context.boundScissor.width);
    EXPECT_EQ(180u, context.boundScissor.height);
    EXPECT_EQ(16u, context.pushConstantSize);
}

TEST(UIValidation, ContextTracksFrameInputAndDispatchesCanvasEvents)
{
    UIContext context;
    UIContextDesc desc;
    desc.width = 320;
    desc.height = 180;
    desc.scaleFactor = 1.0f;
    ASSERT_TRUE(context.Initialize(desc));

    auto widget = std::make_shared<Widget>();
    widget->SetName("Clickable");
    widget->SetPosition(8.0f, 8.0f);
    widget->SetSize(80.0f, 32.0f);

    uint32 clickCount = 0;
    uint32 focusCount = 0;
    widget->SetOnClick([&clickCount](const UIEvent& event) {
        EXPECT_EQ(UIEventType::Click, event.type);
        ++clickCount;
    });
    widget->SetOnFocus([&focusCount](const UIEvent& event) {
        if (event.type == UIEventType::Focus)
        {
            ++focusCount;
        }
    });
    context.GetCanvas().AddWidget(widget);

    UIFrameDesc frame;
    frame.width = 320;
    frame.height = 180;
    frame.deltaTime = 1.0f / 60.0f;
    frame.scaleFactor = 1.0f;
    frame.input.mousePosition = Vec2(16.0f, 16.0f);

    ASSERT_TRUE(context.BeginFrame(frame));
    EXPECT_EQ(1u, context.GetFrameIndex());
    EXPECT_TRUE(widget->IsHovered());
    context.EndFrame();

    frame.input.SetMouseButtonDown(UIMouseButton::Left, true);
    ASSERT_TRUE(context.BeginFrame(frame));
    EXPECT_TRUE(context.GetInput().WasMouseButtonPressed(UIMouseButton::Left));
    EXPECT_TRUE(widget->IsPressed());
    EXPECT_TRUE(widget->IsFocused());
    EXPECT_EQ(1u, focusCount);
    context.EndFrame();

    frame.input.SetMouseButtonDown(UIMouseButton::Left, false);
    ASSERT_TRUE(context.BeginFrame(frame));
    EXPECT_TRUE(context.GetInput().WasMouseButtonReleased(UIMouseButton::Left));
    EXPECT_EQ(1u, clickCount);
    context.EndFrame();

    context.Shutdown();
    EXPECT_FALSE(context.IsInitialized());
}

TEST(UIValidation, ContextScalesThemeMetricsSeparatelyFromInputScale)
{
    UIContext context;
    UIContextDesc desc;
    desc.width = 320;
    desc.height = 180;
    desc.scaleFactor = 1.5f;
    desc.inputScaleFactor = 1.0f;
    ASSERT_TRUE(context.Initialize(desc));

    EXPECT_FLOAT_EQ(1.5f, context.GetScaleFactor());
    EXPECT_FLOAT_EQ(1.0f, context.GetInputScaleFactor());
    EXPECT_FLOAT_EQ(1.0f, context.GetCanvas().GetScaleFactor());
    EXPECT_FLOAT_EQ(21.0f, context.GetTheme().metrics.fontSize);
    EXPECT_FLOAT_EQ(18.0f, context.GetTheme().metrics.smallFontSize);
    EXPECT_FLOAT_EQ(42.0f, context.GetTheme().metrics.controlHeight);

    UIFrameDesc frame;
    frame.width = 320;
    frame.height = 180;
    frame.deltaTime = 1.0f / 60.0f;
    frame.scaleFactor = 2.0f;
    frame.inputScaleFactor = 1.0f;
    ASSERT_TRUE(context.BeginFrame(frame));

    EXPECT_FLOAT_EQ(2.0f, context.GetScaleFactor());
    EXPECT_FLOAT_EQ(1.0f, context.GetInputScaleFactor());
    EXPECT_FLOAT_EQ(1.0f, context.GetCanvas().GetScaleFactor());
    EXPECT_FLOAT_EQ(28.0f, context.GetTheme().metrics.fontSize);
    EXPECT_FLOAT_EQ(24.0f, context.GetTheme().metrics.smallFontSize);
    EXPECT_FLOAT_EQ(56.0f, context.GetTheme().metrics.controlHeight);
    context.EndFrame();
}

TEST(UIValidation, ContextSnapsScaledThemeMetricsToPixelGrid)
{
    UIContext context;
    UIContextDesc desc;
    desc.width = 640;
    desc.height = 360;
    desc.scaleFactor = 1.75f;
    ASSERT_TRUE(context.Initialize(desc));

    EXPECT_FLOAT_EQ(1.75f, context.GetScaleFactor());
    EXPECT_FLOAT_EQ(25.0f, context.GetTheme().metrics.fontSize);
    EXPECT_FLOAT_EQ(21.0f, context.GetTheme().metrics.smallFontSize);
    EXPECT_FLOAT_EQ(32.0f, context.GetTheme().metrics.largeFontSize);
    EXPECT_FLOAT_EQ(49.0f, context.GetTheme().metrics.controlHeight);
    EXPECT_FLOAT_EQ(60.0f, context.GetTheme().metrics.toolbarHeight);
    EXPECT_FLOAT_EQ(42.0f, context.GetTheme().metrics.statusBarHeight);
    EXPECT_FLOAT_EQ(11.0f, context.GetTheme().metrics.spacing);
    EXPECT_FLOAT_EQ(14.0f, context.GetTheme().metrics.padding);
    EXPECT_FLOAT_EQ(2.0f, context.GetTheme().metrics.borderWidth);
    EXPECT_FLOAT_EQ(5.0f, context.GetTheme().metrics.cornerRadius);
}

TEST(UIValidation, ContextKeepsScaledThemeMetricsReadableAtSmallScale)
{
    UIContext context;
    UIContextDesc desc;
    desc.width = 320;
    desc.height = 180;
    desc.scaleFactor = 0.25f;
    ASSERT_TRUE(context.Initialize(desc));

    EXPECT_FLOAT_EQ(0.25f, context.GetScaleFactor());
    EXPECT_FLOAT_EQ(13.0f, context.GetTheme().metrics.fontSize);
    EXPECT_FLOAT_EQ(11.0f, context.GetTheme().metrics.smallFontSize);
    EXPECT_FLOAT_EQ(16.0f, context.GetTheme().metrics.largeFontSize);
    EXPECT_FLOAT_EQ(24.0f, context.GetTheme().metrics.controlHeight);
    EXPECT_FLOAT_EQ(28.0f, context.GetTheme().metrics.toolbarHeight);
    EXPECT_FLOAT_EQ(22.0f, context.GetTheme().metrics.statusBarHeight);
    EXPECT_FLOAT_EQ(2.0f, context.GetTheme().metrics.spacing);
    EXPECT_FLOAT_EQ(2.0f, context.GetTheme().metrics.padding);
    EXPECT_FLOAT_EQ(1.0f, context.GetTheme().metrics.borderWidth);
    EXPECT_FLOAT_EQ(1.0f, context.GetTheme().metrics.cornerRadius);
}

TEST(UIValidation, CanvasClearsTrackedWidgetsWhenRemoved)
{
    UICanvas canvas;
    canvas.Initialize(320.0f, 180.0f);

    auto first = Button::Create("First");
    first->SetName("First");
    first->SetPosition(8.0f, 8.0f);
    first->SetSize(80.0f, 28.0f);

    auto second = Button::Create("Second");
    second->SetName("Second");
    second->SetPosition(104.0f, 8.0f);
    second->SetSize(80.0f, 28.0f);

    uint32 secondClickCount = 0;
    second->SetOnClick([&secondClickCount](const UIEvent& event) {
        EXPECT_EQ(UIEventType::Click, event.type);
        ++secondClickCount;
    });

    canvas.AddWidget(first);
    canvas.AddWidget(second);

    UIEvent mouseDown;
    mouseDown.type = UIEventType::MouseDown;
    mouseDown.position = Vec2(16.0f, 16.0f);
    EXPECT_TRUE(canvas.HandleEvent(mouseDown));
    EXPECT_EQ(first.get(), canvas.GetFocusedWidget());

    canvas.RemoveWidget(first);
    first.reset();
    EXPECT_EQ(nullptr, canvas.GetFocusedWidget());

    UIEvent mouseUp = mouseDown;
    mouseUp.type = UIEventType::MouseUp;
    EXPECT_FALSE(canvas.HandleEvent(mouseUp));

    mouseDown.position = Vec2(112.0f, 16.0f);
    EXPECT_TRUE(canvas.HandleEvent(mouseDown));
    EXPECT_EQ(second.get(), canvas.GetFocusedWidget());
    mouseUp = mouseDown;
    mouseUp.type = UIEventType::MouseUp;
    EXPECT_TRUE(canvas.HandleEvent(mouseUp));
    EXPECT_EQ(1u, secondClickCount);

    canvas.Shutdown();
}

TEST(UIValidation, CanvasPreservesClickAcrossStableNamedRebuild)
{
    UICanvas canvas;
    canvas.Initialize(320.0f, 180.0f);

    uint32 clickCount = 0;
    auto createButton = [&clickCount]() {
        auto button = Button::Create("Rebuilt");
        button->SetName("Editor.Command.Rebuilt");
        button->SetPosition(8.0f, 8.0f);
        button->SetSize(96.0f, 28.0f);
        button->SetOnClick([&clickCount](const UIEvent& event) {
            EXPECT_EQ(UIEventType::Click, event.type);
            ++clickCount;
        });
        return button;
    };

    Button::Ptr original = createButton();
    canvas.AddWidget(original);

    UIEvent mouseDown;
    mouseDown.type = UIEventType::MouseDown;
    mouseDown.position = Vec2(16.0f, 16.0f);
    EXPECT_TRUE(canvas.HandleEvent(mouseDown));
    EXPECT_TRUE(original->IsPressed());

    canvas.RemoveWidget(original);
    original.reset();

    Button::Ptr replacement = createButton();
    canvas.AddWidget(replacement);

    UIEvent mouseUp = mouseDown;
    mouseUp.type = UIEventType::MouseUp;
    EXPECT_TRUE(canvas.HandleEvent(mouseUp));
    EXPECT_EQ(1u, clickCount);
    EXPECT_FALSE(replacement->IsPressed());
    EXPECT_EQ(replacement.get(), canvas.GetFocusedWidget());

    canvas.Shutdown();
}

TEST(UIValidation, ImageDefaultsToDisplayOnlyHitTesting)
{
    UICanvas canvas;
    canvas.Initialize(320.0f, 180.0f);

    auto button = Button::Create("Covered");
    button->SetName("CoveredButton");
    button->SetPosition(8.0f, 8.0f);
    button->SetSize(96.0f, 28.0f);
    canvas.AddWidget(button);

    auto image = Image::Create();
    image->SetName("DisplayOnlyImage");
    image->SetPosition(8.0f, 8.0f);
    image->SetSize(96.0f, 28.0f);
    canvas.AddWidget(image);

    EXPECT_FALSE(image->IsInteractive());
    EXPECT_EQ(button.get(), canvas.HitTest(Vec2(16.0f, 16.0f)));

    canvas.Shutdown();
}

TEST(UIValidation, CanvasMovesFocusAcrossExplicitTabStops)
{
    UICanvas canvas;
    canvas.Initialize(320.0f, 180.0f);

    auto plainWidget = std::make_shared<Widget>();
    plainWidget->SetName("PlainWidget");
    plainWidget->SetPosition(8.0f, 8.0f);
    plainWidget->SetSize(80.0f, 28.0f);

    auto first = Button::Create("First");
    first->SetName("FirstButton");
    first->SetPosition(8.0f, 44.0f);
    first->SetSize(80.0f, 28.0f);

    auto second = Button::Create("Second");
    second->SetName("SecondButton");
    second->SetPosition(104.0f, 44.0f);
    second->SetSize(80.0f, 28.0f);

    auto disabled = Button::Create("Disabled");
    disabled->SetName("DisabledButton");
    disabled->SetPosition(200.0f, 44.0f);
    disabled->SetSize(96.0f, 28.0f);
    disabled->SetEnabled(false);

    EXPECT_FALSE(plainWidget->IsTabStop());
    EXPECT_TRUE(first->IsTabStop());
    EXPECT_TRUE(second->IsTabStop());
    EXPECT_FALSE(disabled->IsTabStop());

    canvas.AddWidget(plainWidget);
    canvas.AddWidget(first);
    canvas.AddWidget(disabled);
    canvas.AddWidget(second);

    canvas.SetFocusedWidget(plainWidget.get());
    EXPECT_EQ(plainWidget.get(), canvas.GetFocusedWidget());

    UIEvent tab;
    tab.type = UIEventType::KeyDown;
    tab.keyCode = static_cast<int>(RVX_UI_KEY_TAB);
    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(first.get(), canvas.GetFocusedWidget());

    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(second.get(), canvas.GetFocusedWidget());

    tab.modifiers = ToMask(UIInputModifier::Shift);
    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(first.get(), canvas.GetFocusedWidget());

    second->SetVisible(false);
    tab.modifiers = 0;
    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(first.get(), canvas.GetFocusedWidget());

    canvas.Shutdown();
}

TEST(UIValidation, CanvasConstrainsTabTraversalToFocusScope)
{
    UICanvas canvas;
    canvas.Initialize(360.0f, 200.0f);

    auto outsideBefore = Button::Create("Before");
    outsideBefore->SetName("OutsideBefore");
    outsideBefore->SetPosition(8.0f, 8.0f);
    outsideBefore->SetSize(96.0f, 28.0f);

    auto scope = Panel::Create();
    scope->SetName("FocusScope");
    scope->SetPosition(8.0f, 48.0f);
    scope->SetSize(220.0f, 80.0f);

    auto scopedFirst = Button::Create("First");
    scopedFirst->SetName("ScopedFirst");
    scopedFirst->SetPosition(8.0f, 8.0f);
    scopedFirst->SetSize(88.0f, 28.0f);

    auto scopedSecond = Button::Create("Second");
    scopedSecond->SetName("ScopedSecond");
    scopedSecond->SetPosition(108.0f, 8.0f);
    scopedSecond->SetSize(96.0f, 28.0f);

    auto outsideAfter = Button::Create("After");
    outsideAfter->SetName("OutsideAfter");
    outsideAfter->SetPosition(8.0f, 144.0f);
    outsideAfter->SetSize(96.0f, 28.0f);

    scope->AddChild(scopedFirst);
    scope->AddChild(scopedSecond);
    canvas.AddWidget(outsideBefore);
    canvas.AddWidget(scope);
    canvas.AddWidget(outsideAfter);

    canvas.SetFocusedWidget(outsideBefore.get());
    EXPECT_EQ(outsideBefore.get(), canvas.GetFocusedWidget());

    canvas.SetFocusScopeRoot(scope.get());
    EXPECT_EQ(scope.get(), canvas.GetFocusScopeRoot());
    EXPECT_EQ(nullptr, canvas.GetFocusedWidget());

    UIEvent tab;
    tab.type = UIEventType::KeyDown;
    tab.keyCode = static_cast<int>(RVX_UI_KEY_TAB);
    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(scopedFirst.get(), canvas.GetFocusedWidget());

    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(scopedSecond.get(), canvas.GetFocusedWidget());

    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(scopedFirst.get(), canvas.GetFocusedWidget());

    tab.modifiers = ToMask(UIInputModifier::Shift);
    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(scopedSecond.get(), canvas.GetFocusedWidget());

    canvas.SetFocusedWidget(outsideAfter.get());
    EXPECT_EQ(nullptr, canvas.GetFocusedWidget());

    canvas.ClearFocusScopeRoot();
    canvas.SetFocusedWidget(scopedSecond.get());
    tab.modifiers = 0u;
    EXPECT_TRUE(canvas.HandleEvent(tab));
    EXPECT_EQ(outsideAfter.get(), canvas.GetFocusedWidget());

    canvas.SetFocusScopeRoot(scope.get());
    canvas.RemoveWidget(scope);
    EXPECT_EQ(nullptr, canvas.GetFocusScopeRoot());

    canvas.Shutdown();
}

TEST(UIValidation, ThemeAndLayoutRegionExposeReusableEditorScaleTokens)
{
    UITheme theme = UITheme::RuntimeDark();
    EXPECT_EQ("Runtime Dark", theme.name);
    EXPECT_GT(theme.metrics.controlHeight, theme.metrics.fontSize);
    EXPECT_GT(theme.metrics.toolbarHeight, theme.metrics.statusBarHeight);
    EXPECT_GT(theme.colors.accent.b, theme.colors.accent.r);

    UILayoutRegion region(Rect(10.0f, 20.0f, 100.0f, 80.0f),
                          EdgeInsets(5.0f),
                          4.0f);
    Rect firstRow = region.AllocateRow(12.0f);
    EXPECT_EQ(15.0f, firstRow.x);
    EXPECT_EQ(25.0f, firstRow.y);
    EXPECT_EQ(90.0f, firstRow.width);
    EXPECT_EQ(12.0f, firstRow.height);

    Rect firstColumn = region.AllocateColumn(20.0f, 10.0f);
    EXPECT_EQ(15.0f, firstColumn.x);
    EXPECT_EQ(41.0f, firstColumn.y);
    EXPECT_EQ(20.0f, firstColumn.width);
    EXPECT_EQ(10.0f, firstColumn.height);
}
