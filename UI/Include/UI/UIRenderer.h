/**
 * @file UIRenderer.h
 * @brief Runtime RHI-backed UI draw command generation
 */

#pragma once

#include "RHI/RHIBuffer.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHIDefinitions.h"
#include "RHI/RHIResources.h"
#include "RHI/RHISampler.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIUpload.h"
#include "UI/UITypes.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RVX
{
    class RHIBuffer;
    class RHICommandContext;
    class RHIDescriptorSet;
    class RHIDescriptorSetLayout;
    class IRHIDevice;
    class RHIPipeline;
    class RHISampler;
    class RHIStagingBuffer;
    class RHITexture;
    class RHITextureView;
}

namespace RVX::UI
{

enum class UIRenderBackend : uint8
{
    None = 0,
    RuntimeRHI
};

enum class UIDrawCommandType : uint8
{
    Rect = 0,
    Text,
    Image
};

struct UIVertex
{
    Vec2 position{0.0f};
    Vec2 uv{0.0f};
    UIColor color = UIColor::White();
};

struct UITextMetrics
{
    float width = 0.0f;
    float height = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineGap = 0.0f;
    uint32 glyphCount = 0;
    uint32 lineCount = 0;
};

struct UIGlyphMetrics
{
    float advance = 0.0f;
    float bearingX = 0.0f;
    float bearingY = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct UIFontAtlasDesc
{
    float fontSize = 32.0f;
    uint32 width = 1024;
    uint32 height = 1024;
    uint32 padding = 2;
    uint32 oversampleX = 2;
    uint32 oversampleY = 2;
    uint32 firstCodepoint = 32;
    uint32 codepointCount = 95;
    std::vector<uint32> codepoints;
};

struct UIFontAtlasGlyph
{
    uint32 codepoint = 0;
    uint32 fontIndex = 0;
    Rect uvRect;
    Vec2 size{0.0f};
    Vec2 offset{0.0f};
    float advance = 0.0f;
    bool valid = false;
};

class UIFontFallbackChain;

struct UIFontFallbackChainDesc
{
    std::vector<std::string> fontPaths;
    bool appendDefaultSystemFonts = true;
};

struct UIFontFallbackChainDiagnostics
{
    uint64 generation = 0;
    uint32 fontCount = 0;
    uint32 missingCoreCodepointCount = 0;
    std::vector<uint32> missingCoreCodepoints;
    bool hasPrimaryFont = false;
    bool hasLatinText = false;
    bool hasEditorSymbols = false;
    bool hasCjkSample = false;
};

class UIFontAtlas
{
public:
    bool IsBuilt() const { return m_built; }

    const UIFontAtlasDesc& GetDesc() const { return m_desc; }
    uint32 GetWidth() const { return m_desc.width; }
    uint32 GetHeight() const { return m_desc.height; }
    uint32 GetGlyphCount() const { return static_cast<uint32>(m_glyphs.size()); }
    const std::vector<uint8>& GetPixels() const { return m_pixels; }

    const UIFontAtlasGlyph* FindGlyph(uint32 codepoint) const;
    bool HasGlyph(uint32 codepoint) const { return FindGlyph(codepoint) != nullptr; }
    bool HasLayoutMetrics() const { return m_layoutFonts != nullptr; }
    float GetKerningAdvance(uint32 previousCodepoint,
                            uint32 codepoint,
                            float fontSize) const;
    float GetAscent(float fontSize) const;
    float GetDescent(float fontSize) const;
    float GetLineGap(float fontSize) const;

private:
    friend class UIFontMetrics;
    friend class UIFontFallbackChain;
    friend class UIRenderer;

    void Reset();

    bool m_built = false;
    UIFontAtlasDesc m_desc;
    std::vector<uint8> m_pixels;
    std::vector<UIFontAtlasGlyph> m_glyphs;
    std::shared_ptr<const UIFontFallbackChain> m_layoutFonts;
};

class UIFontMetrics
{
public:
    static const UIFontMetrics& Default();

    bool LoadDefaultSystemFont();
    bool LoadTrueTypeFile(const std::string& path, uint32 fontIndex = 0);
    bool LoadTrueTypeData(std::vector<uint8> data, uint32 fontIndex = 0);
    bool HasTrueTypeData() const;
    bool HasCodepoint(uint32 codepoint) const;
    bool BuildAtlas(UIFontAtlas& atlas, const UIFontAtlasDesc& desc) const;

    UIGlyphMetrics GetGlyphMetrics(char glyph, float fontSize) const;
    UIGlyphMetrics GetCodepointMetrics(uint32 codepoint, float fontSize) const;
    float GetKerningAdvance(uint32 previousCodepoint,
                            uint32 codepoint,
                            float fontSize) const;
    UITextMetrics MeasureText(const std::string& text, float fontSize) const;

    float GetAscent(float fontSize) const;
    float GetDescent(float fontSize) const;
    float GetLineGap(float fontSize) const;
    float GetLineHeight(float fontSize) const;

private:
    friend class UIFontFallbackChain;

    struct FontData;

    static int ResolveFontCodepoint(const FontData& fontData, uint32 codepoint);
    static UIGlyphMetrics GetFallbackGlyphMetrics(uint32 codepoint, float fontSize);

    std::shared_ptr<FontData> m_fontData;
};

class UIFontFallbackChain
{
public:
    static const UIFontFallbackChain& Default();
    static uint64 GetDefaultGeneration();
    static UIFontFallbackChainDiagnostics GetDefaultDiagnostics();
    static bool ConfigureDefault(const UIFontFallbackChainDesc& desc,
                                 std::string* error = nullptr);
    static bool ResetDefault(std::string* error = nullptr);

    bool LoadDefaultSystemFonts();
    bool LoadFontFile(const std::string& path, uint32 fontIndex = 0);
    bool AddFont(UIFontMetrics font);
    void Clear();
    uint32 GetFontCount() const { return static_cast<uint32>(m_fonts.size()); }
    UIFontFallbackChainDiagnostics GetDiagnostics(
        uint64 generation = 0) const;

    const UIFontMetrics* GetPrimaryFont() const;
    const UIFontMetrics* FindFontForCodepoint(uint32 codepoint) const;
    bool HasCodepoint(uint32 codepoint) const;
    bool BuildAtlas(UIFontAtlas& atlas, const UIFontAtlasDesc& desc) const;

    UIGlyphMetrics GetCodepointMetrics(uint32 codepoint, float fontSize) const;
    float GetKerningAdvance(uint32 previousCodepoint,
                            uint32 codepoint,
                            float fontSize) const;
    UITextMetrics MeasureText(const std::string& text, float fontSize) const;

    float GetAscent(float fontSize) const;
    float GetDescent(float fontSize) const;
    float GetLineGap(float fontSize) const;
    float GetLineHeight(float fontSize) const;

private:
    uint32 FindFontIndexForCodepoint(uint32 codepoint) const;

    std::vector<UIFontMetrics> m_fonts;
};

struct UIRenderTargetDesc
{
    uint32 width = 0;
    uint32 height = 0;
    RHIFormat format = RHIFormat::Unknown;
    RHITextureView* colorTarget = nullptr;
};

struct UIDrawCommand
{
    UIDrawCommandType type = UIDrawCommandType::Rect;
    Rect bounds;
    Rect clipRect;
    Rect uvRect = Rect(0.0f, 0.0f, 1.0f, 1.0f);
    UIColor color = UIColor::White();
    RHITextureView* textureView = nullptr;
    std::string text;
    float fontSize = 0.0f;
    uint32 firstVertex = 0;
    uint32 vertexCount = 0;
    uint32 firstIndex = 0;
    uint32 indexCount = 0;
};

struct UIRenderStats
{
    uint32 commandCount = 0;
    uint32 rectCount = 0;
    uint32 textCount = 0;
    uint32 imageCount = 0;
    uint32 glyphCount = 0;
    uint32 vertexCount = 0;
    uint32 indexCount = 0;
};

struct UIFontAtlasRuntimeStats
{
    bool usesRuntimeAtlas = false;
    bool rebuildPending = false;
    uint32 codepointCount = 0;
    uint32 pendingCodepointCount = 0;
    uint32 rebuildCount = 0;
};

struct UIRenderSubmitDesc
{
    RHICommandContext* commandContext = nullptr;
    RHIPipeline* pipeline = nullptr;
    RHIDescriptorSetLayout* textureSetLayout = nullptr;
    bool allowDefaultFramebufferTarget = false;
    bool beginRenderPass = true;
    RHILoadOp colorLoadOp = RHILoadOp::Load;
    RHIStoreOp colorStoreOp = RHIStoreOp::Store;
    RHIClearColor clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct UIRenderSubmitStats
{
    bool submitted = false;
    bool defaultFramebufferTarget = false;
    bool atlasUploaded = false;
    bool whiteTextureUploaded = false;
    uint32 drawCallCount = 0;
    uint64 vertexBytesUploaded = 0;
    uint64 indexBytesUploaded = 0;
    uint64 atlasBytesUploaded = 0;
    uint64 whiteTextureBytesUploaded = 0;
};

class UIRenderer
{
public:
    bool Initialize(IRHIDevice* device);
    void Shutdown();

    bool IsInitialized() const { return m_device != nullptr; }
    bool IsRecording() const { return m_recording; }
    UIRenderBackend GetBackend() const { return m_backend; }
    IRHIDevice* GetDevice() const { return m_device; }

    void SetFontAtlas(const UIFontAtlas* atlas);
    const UIFontAtlas* GetFontAtlas() const { return m_fontAtlas; }

    void BeginFrame(const UIRenderTargetDesc& target);
    void BeginRecordOnlyFrame(uint32 width,
                              uint32 height,
                              RHIFormat format = RHIFormat::RGBA8_UNORM);
    bool BindRecordedFrameTarget(const UIRenderTargetDesc& target);
    void EndFrame();

    void DrawRect(const Rect& rect, const UIColor& color);
    void DrawBorder(const Rect& rect, const UIColor& color, float width = 1.0f);
    void DrawLine(const Vec2& start,
                  const Vec2& end,
                  float thickness,
                  const UIColor& color);
    void DrawText(const std::string& text,
                  const Rect& bounds,
                  float fontSize,
                  const UIColor& color,
                  TextAlign horizontalAlign = TextAlign::Left,
                  VerticalAlign verticalAlign = VerticalAlign::Top);
    void DrawImage(RHITextureView* textureView,
                   const Rect& rect,
                   const Rect& uvRect = Rect(0.0f, 0.0f, 1.0f, 1.0f),
                   const UIColor& color = UIColor::White());
    void PushClipRect(const Rect& rect);
    void PopClipRect();
    Rect GetCurrentClipRect() const;

    UITextMetrics MeasureText(const std::string& text, float fontSize) const;

    bool Submit(const UIRenderSubmitDesc& desc);

    const UIRenderTargetDesc& GetCurrentTarget() const { return m_target; }
    const std::vector<UIDrawCommand>& GetCommands() const { return m_commands; }
    const std::vector<UIVertex>& GetVertices() const { return m_vertices; }
    const std::vector<uint32>& GetIndices() const { return m_indices; }
    const UIRenderStats& GetStats() const { return m_stats; }
    const UIRenderSubmitStats& GetSubmitStats() const { return m_submitStats; }
    const UIFontAtlasRuntimeStats& GetFontAtlasRuntimeStats() const
    {
        return m_fontAtlasRuntimeStats;
    }

    RHITextureView* GetFontAtlasTextureView() const { return m_fontAtlasTextureView.Get(); }
    RHITextureView* GetWhiteTextureView() const { return m_whiteTextureView.Get(); }
    RHIBuffer* GetVertexBuffer() const { return m_vertexBuffer.Get(); }
    RHIBuffer* GetIndexBuffer() const { return m_indexBuffer.Get(); }

private:
    void ResetFrameData();
    void ResetGPUResources();
    void InitializeClipStack();
    void EmitQuad(const Rect& rect, const Rect& uvRect, const UIColor& color, UIDrawCommand& command);
    void EmitQuad(const Vec2& topLeft,
                  const Vec2& topRight,
                  const Vec2& bottomRight,
                  const Vec2& bottomLeft,
                  const Rect& uvRect,
                  const UIColor& color,
                  UIDrawCommand& command);
    bool EnsureDynamicBuffers();
    bool EnsureBuffer(RHIBufferRef& buffer,
                      uint64& capacity,
                      uint64 requiredSize,
                      RHIBufferUsage usage,
                      const char* debugName,
                      uint32 stride);
    bool UploadBuffer(RHIBuffer* buffer, const void* data, uint64 size);
    bool EnsureTextureDescriptorResources();
    bool EnsureWhiteTexture(RHICommandContext& context);
    bool EnsureFontAtlasTexture(RHICommandContext& context);
    RHIDescriptorSet* GetOrCreateTextureDescriptor(RHITextureView* textureView);
    RHIDescriptorSet* GetDescriptorSetForCommand(const UIDrawCommand& command);
    RHIDescriptorSetLayout* GetActiveTextureSetLayout() const;
    void ApplyTextureSetLayoutOverride(RHIDescriptorSetLayout* layout);
    void QueueMissingFontCodepoint(uint32 codepoint);
    bool RebuildRuntimeFontAtlasIfNeeded();

    IRHIDevice* m_device = nullptr;
    UIRenderBackend m_backend = UIRenderBackend::None;
    UIRenderTargetDesc m_target;
    bool m_recording = false;
    const UIFontAtlas* m_fontAtlas = nullptr;
    bool m_runtimeFontAtlasExpansionEnabled = false;
    UIFontAtlas m_runtimeFontAtlas;
    UIFontAtlasDesc m_runtimeFontAtlasDesc;
    UIFontAtlasRuntimeStats m_fontAtlasRuntimeStats;
    std::vector<uint32> m_runtimeFontAtlasCodepoints;
    std::vector<uint32> m_pendingFontAtlasCodepoints;

    RHIBufferRef m_vertexBuffer;
    RHIBufferRef m_indexBuffer;
    uint64 m_vertexBufferCapacity = 0;
    uint64 m_indexBufferCapacity = 0;

    RHITextureRef m_fontAtlasTexture;
    RHITextureViewRef m_fontAtlasTextureView;
    RHIStagingBufferRef m_fontAtlasStagingBuffer;
    RHIDescriptorSetRef m_fontAtlasDescriptorSet;
    const UIFontAtlas* m_uploadedFontAtlas = nullptr;

    RHITextureRef m_whiteTexture;
    RHITextureViewRef m_whiteTextureView;
    RHIStagingBufferRef m_whiteTextureStagingBuffer;
    RHIDescriptorSetRef m_whiteDescriptorSet;

    RHISamplerRef m_textureSampler;
    RHISamplerRef m_fontSampler;
    RHIDescriptorSetLayoutRef m_textureSetLayout;
    RHIDescriptorSetLayout* m_textureSetLayoutOverride = nullptr;
    std::vector<std::pair<RHITextureView*, RHIDescriptorSetRef>> m_textureDescriptorSets;

    std::vector<UIDrawCommand> m_commands;
    std::vector<UIVertex> m_vertices;
    std::vector<uint32> m_indices;
    std::vector<Rect> m_clipStack;
    UIRenderStats m_stats;
    UIRenderSubmitStats m_submitStats;
};

} // namespace RVX::UI
