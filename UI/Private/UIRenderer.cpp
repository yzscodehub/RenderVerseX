/**
 * @file UIRenderer.cpp
 * @brief UIRenderer implementation
 */

#include "UI/UIRenderer.h"

#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIRenderPass.h"
#include "RHI/RHISampler.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIUpload.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace RVX::UI
{

namespace
{
    constexpr float RVX_UI_DEFAULT_ATLAS_FONT_SIZE = 64.0f;
    constexpr uint32 RVX_UI_DEFAULT_ATLAS_SIZE = 2048;
    constexpr uint32 RVX_UI_DEFAULT_ATLAS_PADDING = 4;
    constexpr uint32 RVX_UI_DEFAULT_ATLAS_OVERSAMPLE = 3;

    float GlyphAdvanceScale(uint32 codepoint)
    {
        if (codepoint == '\t')
        {
            return 1.32f;
        }
        if (codepoint == ' ')
        {
            return 0.33f;
        }
        if (codepoint == 'i' || codepoint == 'l' || codepoint == 'I' || codepoint == '!' ||
            codepoint == '.' || codepoint == ',' || codepoint == ':' || codepoint == ';' ||
            codepoint == '|')
        {
            return 0.28f;
        }
        if (codepoint == 'm' || codepoint == 'w' || codepoint == 'M' || codepoint == 'W' ||
            codepoint == '@' || codepoint == '#')
        {
            return 0.88f;
        }
        if (codepoint <= 0x7Fu &&
            std::isdigit(static_cast<unsigned char>(codepoint)) != 0)
        {
            return 0.56f;
        }
        if (codepoint <= 0x7Fu &&
            std::isupper(static_cast<unsigned char>(codepoint)) != 0)
        {
            return 0.68f;
        }
        if (codepoint <= 0x7Fu &&
            std::ispunct(static_cast<unsigned char>(codepoint)) != 0)
        {
            return 0.38f;
        }
        return 0.56f;
    }

    uint32 RemapAtlasCodepoint(uint32 codepoint)
    {
        return codepoint <= 127u ? codepoint : static_cast<uint32>('?');
    }

    Rect GlyphAtlasUV(uint32 codepoint)
    {
        const uint32 code = RemapAtlasCodepoint(codepoint);
        const float cellWidth = 1.0f / 16.0f;
        const float cellHeight = 1.0f / 8.0f;
        const float u = static_cast<float>(code % 16u) * cellWidth;
        const float v = static_cast<float>((code / 16u) % 8u) * cellHeight;
        return Rect(u, v, cellWidth, cellHeight);
    }

    std::vector<uint32> BuildDefaultUIFontAtlasCodepoints()
    {
        std::vector<uint32> codepoints;
        codepoints.reserve(112u);
        for (uint32 codepoint = 32u; codepoint <= 126u; ++codepoint)
        {
            codepoints.push_back(codepoint);
        }

        constexpr std::array<uint32, 9> smokeCodepoints = {
            0x00D7u, // multiplication sign used as a close affordance
            0x2013u, // en dash
            0x2014u, // em dash
            0x2026u, // horizontal ellipsis
            0x2190u, // left arrow
            0x2192u, // right arrow
            0x2713u, // check mark
            0x2715u, // multiplication x
            0x4E2Du, // CJK smoke glyph for localized editor strings
        };
        codepoints.insert(codepoints.end(),
                          smokeCodepoints.begin(),
                          smokeCodepoints.end());
        return codepoints;
    }

    constexpr std::array<uint32, 12> RVX_UI_CORE_FONT_DIAGNOSTIC_CODEPOINTS = {
        static_cast<uint32>(' '),
        static_cast<uint32>('0'),
        static_cast<uint32>('9'),
        static_cast<uint32>('A'),
        static_cast<uint32>('Z'),
        static_cast<uint32>('a'),
        static_cast<uint32>('z'),
        static_cast<uint32>('.'),
        static_cast<uint32>(':'),
        0x2026u, // horizontal ellipsis
        0x2192u, // right arrow
        0x2713u, // check mark
    };

    struct UIFontFallbackChainDefaultState
    {
        UIFontFallbackChain chain;
        uint64 generation = 1;
    };

    UIFontFallbackChainDefaultState& GetDefaultFontFallbackChainState()
    {
        static UIFontFallbackChainDefaultState state = [] {
            UIFontFallbackChainDefaultState defaultState;
            defaultState.chain.LoadDefaultSystemFonts();
            return defaultState;
        }();
        return state;
    }

    const UIFontAtlas* GetDefaultFontAtlas()
    {
        static UIFontAtlas atlas;
        static uint64 atlasGeneration = 0;

        const uint64 defaultGeneration = UIFontFallbackChain::GetDefaultGeneration();
        if (atlasGeneration != defaultGeneration || !atlas.IsBuilt())
        {
            UIFontAtlasDesc desc;
            desc.fontSize = RVX_UI_DEFAULT_ATLAS_FONT_SIZE;
            desc.width = RVX_UI_DEFAULT_ATLAS_SIZE;
            desc.height = RVX_UI_DEFAULT_ATLAS_SIZE;
            desc.padding = RVX_UI_DEFAULT_ATLAS_PADDING;
            desc.oversampleX = RVX_UI_DEFAULT_ATLAS_OVERSAMPLE;
            desc.oversampleY = RVX_UI_DEFAULT_ATLAS_OVERSAMPLE;
            desc.codepoints = BuildDefaultUIFontAtlasCodepoints();
            if (UIFontFallbackChain::Default().BuildAtlas(atlas, desc))
            {
                atlasGeneration = defaultGeneration;
            }
        }

        return atlas.IsBuilt() ? &atlas : nullptr;
    }

    bool IsRenderableGlyph(uint32 codepoint)
    {
        return codepoint != '\n' && codepoint != '\r' && codepoint != '\t' && codepoint != ' ';
    }

    float GetAtlasGlyphScale(const UIFontAtlas& atlas, float fontSize)
    {
        const float atlasFontSize = atlas.GetDesc().fontSize;
        if (atlasFontSize <= 0.0f)
        {
            return 1.0f;
        }
        return fontSize / atlasFontSize;
    }

    Rect InsetGlyphUVForLinearSampling(const UIFontAtlas& atlas, const Rect& uvRect)
    {
        if (atlas.GetWidth() == 0u || atlas.GetHeight() == 0u)
        {
            return uvRect;
        }

        const float halfTexelU = 0.5f / static_cast<float>(atlas.GetWidth());
        const float halfTexelV = 0.5f / static_cast<float>(atlas.GetHeight());
        if (uvRect.width <= halfTexelU * 2.0f ||
            uvRect.height <= halfTexelV * 2.0f)
        {
            return uvRect;
        }

        return Rect(uvRect.x + halfTexelU,
                    uvRect.y + halfTexelV,
                    uvRect.width - halfTexelU * 2.0f,
                    uvRect.height - halfTexelV * 2.0f);
    }

    Rect SnapGlyphRectToPixelGrid(const Rect& rect)
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

    bool DecodeNextCodepoint(const std::string& text, size_t& index, uint32& codepoint)
    {
        const unsigned char lead = static_cast<unsigned char>(text[index++]);
        if (lead < 0x80u)
        {
            codepoint = lead;
            return true;
        }

        uint32 value = 0;
        uint32 continuationCount = 0;
        if ((lead & 0xE0u) == 0xC0u)
        {
            value = lead & 0x1Fu;
            continuationCount = 1;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            value = lead & 0x0Fu;
            continuationCount = 2;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            value = lead & 0x07u;
            continuationCount = 3;
        }
        else
        {
            codepoint = '?';
            return false;
        }

        if (index + continuationCount > text.size())
        {
            codepoint = '?';
            return false;
        }

        for (uint32 i = 0; i < continuationCount; ++i)
        {
            const unsigned char next = static_cast<unsigned char>(text[index]);
            if ((next & 0xC0u) != 0x80u)
            {
                codepoint = '?';
                return false;
            }

            value = (value << 6u) | static_cast<uint32>(next & 0x3Fu);
            ++index;
        }

        codepoint = value;
        return true;
    }

    uint64 GrowCapacity(uint64 current, uint64 required)
    {
        uint64 capacity = std::max<uint64>(current, 4096);
        while (capacity < required)
        {
            capacity *= 2;
        }
        return capacity;
    }

    Rect IntersectRect(const Rect& lhs, const Rect& rhs)
    {
        const float left = std::max(lhs.Left(), rhs.Left());
        const float top = std::max(lhs.Top(), rhs.Top());
        const float right = std::min(lhs.Right(), rhs.Right());
        const float bottom = std::min(lhs.Bottom(), rhs.Bottom());
        if (right <= left || bottom <= top)
        {
            return Rect(left, top, 0.0f, 0.0f);
        }
        return Rect(left, top, right - left, bottom - top);
    }

    bool RectOverlapsPositive(const Rect& lhs, const Rect& rhs)
    {
        return lhs.width > 0.0f && lhs.height > 0.0f && rhs.width > 0.0f &&
               rhs.height > 0.0f && lhs.Overlaps(rhs);
    }

    RHIRect ToScissorRect(const Rect& rect, uint32 targetWidth, uint32 targetHeight)
    {
        const int32 left = std::clamp(static_cast<int32>(std::floor(rect.Left())),
                                      0,
                                      static_cast<int32>(targetWidth));
        const int32 top = std::clamp(static_cast<int32>(std::floor(rect.Top())),
                                     0,
                                     static_cast<int32>(targetHeight));
        const int32 right = std::clamp(static_cast<int32>(std::ceil(rect.Right())),
                                       0,
                                       static_cast<int32>(targetWidth));
        const int32 bottom = std::clamp(static_cast<int32>(std::ceil(rect.Bottom())),
                                        0,
                                        static_cast<int32>(targetHeight));

        RHIRect scissor;
        scissor.x = left;
        scissor.y = top;
        scissor.width = static_cast<uint32>(std::max(0, right - left));
        scissor.height = static_cast<uint32>(std::max(0, bottom - top));
        return scissor;
    }

    bool SameScissor(const RHIRect& lhs, const RHIRect& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y &&
               lhs.width == rhs.width && lhs.height == rhs.height;
    }

    std::vector<uint8> ExpandAlphaAtlasToRGBA(const UIFontAtlas& atlas)
    {
        const std::vector<uint8>& alphaPixels = atlas.GetPixels();
        std::vector<uint8> rgbaPixels(alphaPixels.size() * 4u, 255u);
        for (size_t i = 0; i < alphaPixels.size(); ++i)
        {
            rgbaPixels[i * 4u + 0u] = 255u;
            rgbaPixels[i * 4u + 1u] = 255u;
            rgbaPixels[i * 4u + 2u] = 255u;
            rgbaPixels[i * 4u + 3u] = alphaPixels[i];
        }
        return rgbaPixels;
    }

    struct UIPushConstants
    {
        float targetSize[2] = {0.0f, 0.0f};
        float inverseTargetSize[2] = {0.0f, 0.0f};
    };

    std::vector<uint32> BuildAtlasCodepointList(const UIFontAtlasDesc& desc)
    {
        std::vector<uint32> codepoints;
        codepoints.reserve(desc.codepoints.empty()
                               ? static_cast<size_t>(desc.codepointCount)
                               : desc.codepoints.size());

        const auto appendUnique = [&codepoints](uint32 codepoint) {
            if (codepoint > static_cast<uint32>(std::numeric_limits<int>::max()))
            {
                return;
            }
            if (std::find(codepoints.begin(), codepoints.end(), codepoint) ==
                codepoints.end())
            {
                codepoints.push_back(codepoint);
            }
        };

        if (!desc.codepoints.empty())
        {
            for (uint32 codepoint : desc.codepoints)
            {
                appendUnique(codepoint);
            }
            return codepoints;
        }

        for (uint32 index = 0; index < desc.codepointCount; ++index)
        {
            const uint64 codepoint =
                static_cast<uint64>(desc.firstCodepoint) + index;
            if (codepoint > std::numeric_limits<uint32>::max())
            {
                break;
            }
            appendUnique(static_cast<uint32>(codepoint));
        }
        return codepoints;
    }

    float GetAtlasGlyphAdvance(const UIFontAtlas* atlas, uint32 codepoint, float fontSize)
    {
        if (!atlas)
        {
            return -1.0f;
        }

        const UIFontAtlasGlyph* glyph = atlas->FindGlyph(codepoint);
        if (!glyph)
        {
            return -1.0f;
        }

        return glyph->advance * GetAtlasGlyphScale(*atlas, fontSize);
    }

    float GetTextKerningAdvance(const UIFontFallbackChain& font,
                                const UIFontAtlas* atlas,
                                uint32 previousCodepoint,
                                uint32 codepoint,
                                float fontSize)
    {
        if (atlas &&
            atlas->HasLayoutMetrics() &&
            atlas->HasGlyph(previousCodepoint) &&
            atlas->HasGlyph(codepoint))
        {
            return atlas->GetKerningAdvance(
                previousCodepoint, codepoint, fontSize);
        }
        return font.GetKerningAdvance(
            previousCodepoint, codepoint, fontSize);
    }

    UITextMetrics MeasureTextForAtlas(const UIFontFallbackChain& font,
                                      const UIFontAtlas* atlas,
                                      const std::string& text,
                                      float fontSize)
    {
        UITextMetrics metrics;
        const bool useAtlasMetrics =
            atlas && atlas->HasLayoutMetrics();
        metrics.ascent = useAtlasMetrics
                             ? atlas->GetAscent(fontSize)
                             : font.GetAscent(fontSize);
        metrics.descent = useAtlasMetrics
                              ? atlas->GetDescent(fontSize)
                              : font.GetDescent(fontSize);
        metrics.lineGap = useAtlasMetrics
                              ? atlas->GetLineGap(fontSize)
                              : font.GetLineGap(fontSize);
        metrics.lineCount = text.empty() ? 0u : 1u;

        float currentLineWidth = 0.0f;
        uint32 previousCodepoint = 0u;
        bool hasPreviousCodepoint = false;
        size_t index = 0;
        while (index < text.size())
        {
            uint32 codepoint = 0;
            DecodeNextCodepoint(text, index, codepoint);
            if (codepoint == '\r')
            {
                continue;
            }
            if (codepoint == '\n')
            {
                metrics.width = std::max(metrics.width, currentLineWidth);
                currentLineWidth = 0.0f;
                previousCodepoint = 0u;
                hasPreviousCodepoint = false;
                ++metrics.lineCount;
                continue;
            }

            if (hasPreviousCodepoint)
            {
                currentLineWidth += GetTextKerningAdvance(
                    font,
                    atlas,
                    previousCodepoint,
                    codepoint,
                    fontSize);
            }

            const float atlasAdvance = GetAtlasGlyphAdvance(atlas, codepoint, fontSize);
            currentLineWidth += atlasAdvance >= 0.0f
                                    ? atlasAdvance
                                    : font.GetCodepointMetrics(codepoint, fontSize).advance;
            previousCodepoint = codepoint;
            hasPreviousCodepoint = true;
            ++metrics.glyphCount;
        }

        metrics.width = std::max(metrics.width, currentLineWidth);
        if (metrics.lineCount > 0)
        {
            metrics.height =
                static_cast<float>(metrics.lineCount) * (metrics.ascent + metrics.descent) +
                static_cast<float>(metrics.lineCount - 1u) * metrics.lineGap;
        }
        return metrics;
    }
}

struct UIFontMetrics::FontData
{
    std::vector<uint8> bytes;
    stbtt_fontinfo info{};
    uint32 fontIndex = 0;
    int fontOffset = 0;
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
};

int UIFontMetrics::ResolveFontCodepoint(const FontData& fontData, uint32 codepoint)
{
    int stbCodepoint = static_cast<int>('?');
    if (codepoint <= static_cast<uint32>(std::numeric_limits<int>::max()))
    {
        stbCodepoint = static_cast<int>(codepoint);
    }
    if (stbtt_FindGlyphIndex(&fontData.info, stbCodepoint) == 0)
    {
        stbCodepoint = '?';
    }
    return stbCodepoint;
}

void UIFontAtlas::Reset()
{
    m_built = false;
    m_desc = {};
    m_pixels.clear();
    m_glyphs.clear();
    m_layoutFonts.reset();
}

const UIFontAtlasGlyph* UIFontAtlas::FindGlyph(uint32 codepoint) const
{
    const auto it = std::find_if(m_glyphs.begin(),
                                 m_glyphs.end(),
                                 [codepoint](const UIFontAtlasGlyph& glyph) {
                                     return glyph.valid && glyph.codepoint == codepoint;
                                 });
    return it != m_glyphs.end() ? &(*it) : nullptr;
}

float UIFontAtlas::GetKerningAdvance(uint32 previousCodepoint,
                                     uint32 codepoint,
                                     float fontSize) const
{
    return m_layoutFonts
               ? m_layoutFonts->GetKerningAdvance(
                     previousCodepoint, codepoint, fontSize)
               : 0.0f;
}

float UIFontAtlas::GetAscent(float fontSize) const
{
    return m_layoutFonts ? m_layoutFonts->GetAscent(fontSize) : 0.0f;
}

float UIFontAtlas::GetDescent(float fontSize) const
{
    return m_layoutFonts ? m_layoutFonts->GetDescent(fontSize) : 0.0f;
}

float UIFontAtlas::GetLineGap(float fontSize) const
{
    return m_layoutFonts ? m_layoutFonts->GetLineGap(fontSize) : 0.0f;
}

const UIFontMetrics& UIFontMetrics::Default()
{
    static const UIFontMetrics metrics = [] {
        UIFontMetrics defaultMetrics;
        defaultMetrics.LoadDefaultSystemFont();
        return defaultMetrics;
    }();
    return metrics;
}

bool UIFontMetrics::LoadDefaultSystemFont()
{
#if defined(_WIN32)
    constexpr std::array<const char*, 3> fontCandidates = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/calibri.ttf",
    };
#elif defined(__APPLE__)
    constexpr std::array<const char*, 3> fontCandidates = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
    };
#else
    constexpr std::array<const char*, 4> fontCandidates = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    };
#endif

    for (const char* candidate : fontCandidates)
    {
        if (LoadTrueTypeFile(candidate))
        {
            return true;
        }
    }

    return false;
}

bool UIFontMetrics::LoadTrueTypeFile(const std::string& path, uint32 fontIndex)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return false;
    }

    const std::streamoff fileSize = stream.tellg();
    if (fileSize <= 0 || fileSize > static_cast<std::streamoff>(std::numeric_limits<uint32>::max()))
    {
        return false;
    }

    std::vector<uint8> bytes(static_cast<size_t>(fileSize));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fileSize));
    if (!stream)
    {
        return false;
    }

    return LoadTrueTypeData(std::move(bytes), fontIndex);
}

bool UIFontMetrics::LoadTrueTypeData(std::vector<uint8> data, uint32 fontIndex)
{
    if (data.empty() || fontIndex > static_cast<uint32>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    const int offset = stbtt_GetFontOffsetForIndex(data.data(), static_cast<int>(fontIndex));
    if (offset < 0)
    {
        return false;
    }

    auto fontData = std::make_shared<FontData>();
    fontData->bytes = std::move(data);
    fontData->fontIndex = fontIndex;
    fontData->fontOffset = offset;
    if (stbtt_InitFont(&fontData->info, fontData->bytes.data(), offset) == 0)
    {
        return false;
    }

    stbtt_GetFontVMetrics(&fontData->info,
                          &fontData->ascent,
                          &fontData->descent,
                          &fontData->lineGap);
    m_fontData = std::move(fontData);
    return true;
}

bool UIFontMetrics::HasTrueTypeData() const
{
    return m_fontData != nullptr;
}

bool UIFontMetrics::HasCodepoint(uint32 codepoint) const
{
    return m_fontData &&
           codepoint <= static_cast<uint32>(std::numeric_limits<int>::max()) &&
           stbtt_FindGlyphIndex(&m_fontData->info,
                                static_cast<int>(codepoint)) != 0;
}

bool UIFontMetrics::BuildAtlas(UIFontAtlas& atlas, const UIFontAtlasDesc& desc) const
{
    atlas.Reset();
    std::vector<uint32> codepoints = BuildAtlasCodepointList(desc);
    if (!m_fontData ||
        desc.fontSize <= 0.0f ||
        desc.width == 0 ||
        desc.height == 0 ||
        codepoints.empty() ||
        codepoints.size() > 4096u)
    {
        return false;
    }

    UIFontAtlasDesc resolvedDesc = desc;
    resolvedDesc.padding = std::min<uint32>(resolvedDesc.padding, 64u);
    resolvedDesc.oversampleX = std::clamp<uint32>(resolvedDesc.oversampleX, 1u, 8u);
    resolvedDesc.oversampleY = std::clamp<uint32>(resolvedDesc.oversampleY, 1u, 8u);
    resolvedDesc.firstCodepoint = codepoints.front();
    resolvedDesc.codepointCount = static_cast<uint32>(codepoints.size());
    resolvedDesc.codepoints = codepoints;

    atlas.m_desc = resolvedDesc;
    atlas.m_pixels.assign(static_cast<size_t>(resolvedDesc.width) * resolvedDesc.height, 0u);
    atlas.m_glyphs.clear();
    atlas.m_glyphs.reserve(codepoints.size());

    std::vector<int> stbCodepoints;
    stbCodepoints.reserve(codepoints.size());
    for (uint32 codepoint : codepoints)
    {
        stbCodepoints.push_back(static_cast<int>(codepoint));
    }

    std::vector<stbtt_packedchar> packedChars(codepoints.size());
    stbtt_pack_context packContext{};
    if (stbtt_PackBegin(&packContext,
                        atlas.m_pixels.data(),
                        static_cast<int>(resolvedDesc.width),
                        static_cast<int>(resolvedDesc.height),
                        0,
                        static_cast<int>(resolvedDesc.padding),
                        nullptr) == 0)
    {
        atlas.Reset();
        return false;
    }

    stbtt_PackSetOversampling(&packContext,
                              static_cast<uint32>(resolvedDesc.oversampleX),
                              static_cast<uint32>(resolvedDesc.oversampleY));
    stbtt_pack_range packRange{};
    packRange.font_size = resolvedDesc.fontSize;
    packRange.first_unicode_codepoint_in_range = 0;
    packRange.array_of_unicode_codepoints = stbCodepoints.data();
    packRange.num_chars = static_cast<int>(stbCodepoints.size());
    packRange.chardata_for_range = packedChars.data();
    const int packResult = stbtt_PackFontRanges(&packContext,
                                                m_fontData->bytes.data(),
                                                static_cast<int>(m_fontData->fontIndex),
                                                &packRange,
                                                1);
    stbtt_PackEnd(&packContext);

    if (packResult == 0)
    {
        atlas.Reset();
        return false;
    }

    const float invWidth = 1.0f / static_cast<float>(resolvedDesc.width);
    const float invHeight = 1.0f / static_cast<float>(resolvedDesc.height);
    for (uint32 i = 0; i < static_cast<uint32>(codepoints.size()); ++i)
    {
        const stbtt_packedchar& packed = packedChars[i];
        const uint32 codepoint = codepoints[i];
        if (!HasCodepoint(codepoint))
        {
            continue;
        }

        UIFontAtlasGlyph glyph;
        glyph.codepoint = codepoint;
        glyph.fontIndex = 0u;
        glyph.uvRect = Rect(static_cast<float>(packed.x0) * invWidth,
                            static_cast<float>(packed.y0) * invHeight,
                            static_cast<float>(packed.x1 - packed.x0) * invWidth,
                            static_cast<float>(packed.y1 - packed.y0) * invHeight);
        glyph.size = Vec2(packed.xoff2 - packed.xoff,
                          packed.yoff2 - packed.yoff);
        glyph.offset = Vec2(packed.xoff, packed.yoff);
        glyph.advance = packed.xadvance;
        glyph.valid = true;
        atlas.m_glyphs.push_back(glyph);
    }

    auto layoutFonts = std::make_shared<UIFontFallbackChain>();
    if (!layoutFonts->AddFont(*this))
    {
        atlas.Reset();
        return false;
    }
    atlas.m_layoutFonts = std::move(layoutFonts);
    atlas.m_built = true;
    return true;
}

UIGlyphMetrics UIFontMetrics::GetFallbackGlyphMetrics(uint32 codepoint, float fontSize)
{
    UIGlyphMetrics metrics;
    metrics.advance = GlyphAdvanceScale(codepoint) * fontSize;
    metrics.bearingX = 0.0f;
    metrics.bearingY = fontSize * 0.78f;
    metrics.width = std::max(0.0f, metrics.advance * 0.9f);
    metrics.height = fontSize;
    return metrics;
}

UIGlyphMetrics UIFontMetrics::GetGlyphMetrics(char glyph, float fontSize) const
{
    return GetCodepointMetrics(static_cast<uint32>(static_cast<unsigned char>(glyph)), fontSize);
}

UIGlyphMetrics UIFontMetrics::GetCodepointMetrics(uint32 codepoint, float fontSize) const
{
    fontSize = std::max(0.0f, fontSize);
    if (!m_fontData || codepoint == '\t')
    {
        return GetFallbackGlyphMetrics(codepoint, fontSize);
    }

    const int stbCodepoint = ResolveFontCodepoint(*m_fontData, codepoint);

    const float scale = stbtt_ScaleForPixelHeight(&m_fontData->info, fontSize);
    int advance = 0;
    int leftSideBearing = 0;
    stbtt_GetCodepointHMetrics(&m_fontData->info, stbCodepoint, &advance, &leftSideBearing);

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetCodepointBitmapBox(&m_fontData->info, stbCodepoint, scale, scale, &x0, &y0, &x1, &y1);

    UIGlyphMetrics metrics;
    metrics.advance = static_cast<float>(advance) * scale;
    metrics.bearingX = static_cast<float>(leftSideBearing) * scale;
    metrics.bearingY = static_cast<float>(-y0);
    metrics.width = std::max(0.0f, static_cast<float>(x1 - x0));
    metrics.height = std::max(0.0f, static_cast<float>(y1 - y0));
    return metrics;
}

float UIFontMetrics::GetKerningAdvance(uint32 previousCodepoint,
                                        uint32 codepoint,
                                        float fontSize) const
{
    fontSize = std::max(0.0f, fontSize);
    if (!m_fontData ||
        fontSize <= 0.0f ||
        previousCodepoint == 0u ||
        previousCodepoint == '\t' ||
        codepoint == '\t')
    {
        return 0.0f;
    }

    const int previous = ResolveFontCodepoint(*m_fontData, previousCodepoint);
    const int current = ResolveFontCodepoint(*m_fontData, codepoint);
    const float scale = stbtt_ScaleForPixelHeight(&m_fontData->info, fontSize);
    return static_cast<float>(
               stbtt_GetCodepointKernAdvance(&m_fontData->info,
                                             previous,
                                             current)) *
           scale;
}

UITextMetrics UIFontMetrics::MeasureText(const std::string& text, float fontSize) const
{
    UITextMetrics metrics;
    metrics.ascent = GetAscent(fontSize);
    metrics.descent = GetDescent(fontSize);
    metrics.lineGap = GetLineGap(fontSize);
    metrics.lineCount = text.empty() ? 0u : 1u;

    float currentLineWidth = 0.0f;
    uint32 previousCodepoint = 0u;
    bool hasPreviousCodepoint = false;
    size_t index = 0;
    while (index < text.size())
    {
        uint32 codepoint = 0;
        DecodeNextCodepoint(text, index, codepoint);
        if (codepoint == '\r')
        {
            continue;
        }
        if (codepoint == '\n')
        {
            metrics.width = std::max(metrics.width, currentLineWidth);
            currentLineWidth = 0.0f;
            previousCodepoint = 0u;
            hasPreviousCodepoint = false;
            ++metrics.lineCount;
            continue;
        }

        if (hasPreviousCodepoint)
        {
            currentLineWidth +=
                GetKerningAdvance(previousCodepoint, codepoint, fontSize);
        }
        currentLineWidth += GetCodepointMetrics(codepoint, fontSize).advance;
        previousCodepoint = codepoint;
        hasPreviousCodepoint = true;
        ++metrics.glyphCount;
    }

    metrics.width = std::max(metrics.width, currentLineWidth);
    if (metrics.lineCount > 0)
    {
        metrics.height = static_cast<float>(metrics.lineCount) * (metrics.ascent + metrics.descent) +
                         static_cast<float>(metrics.lineCount - 1u) * metrics.lineGap;
    }
    return metrics;
}

float UIFontMetrics::GetAscent(float fontSize) const
{
    if (m_fontData)
    {
        return static_cast<float>(m_fontData->ascent) *
               stbtt_ScaleForPixelHeight(&m_fontData->info, std::max(0.0f, fontSize));
    }
    return std::max(0.0f, fontSize) * 0.78f;
}

float UIFontMetrics::GetDescent(float fontSize) const
{
    if (m_fontData)
    {
        return static_cast<float>(-m_fontData->descent) *
               stbtt_ScaleForPixelHeight(&m_fontData->info, std::max(0.0f, fontSize));
    }
    return std::max(0.0f, fontSize) * 0.22f;
}

float UIFontMetrics::GetLineGap(float fontSize) const
{
    if (m_fontData)
    {
        return std::max(0.0f,
                        static_cast<float>(m_fontData->lineGap) *
                            stbtt_ScaleForPixelHeight(&m_fontData->info, std::max(0.0f, fontSize)));
    }
    return std::max(0.0f, fontSize) * 0.2f;
}

float UIFontMetrics::GetLineHeight(float fontSize) const
{
    return GetAscent(fontSize) + GetDescent(fontSize) + GetLineGap(fontSize);
}

const UIFontFallbackChain& UIFontFallbackChain::Default()
{
    return GetDefaultFontFallbackChainState().chain;
}

uint64 UIFontFallbackChain::GetDefaultGeneration()
{
    return GetDefaultFontFallbackChainState().generation;
}

UIFontFallbackChainDiagnostics UIFontFallbackChain::GetDefaultDiagnostics()
{
    const UIFontFallbackChainDefaultState& state =
        GetDefaultFontFallbackChainState();
    return state.chain.GetDiagnostics(state.generation);
}

bool UIFontFallbackChain::ConfigureDefault(const UIFontFallbackChainDesc& desc,
                                           std::string* error)
{
    if (error)
    {
        error->clear();
    }

    UIFontFallbackChain configured;
    for (const std::string& fontPath : desc.fontPaths)
    {
        if (fontPath.empty())
        {
            continue;
        }
        configured.LoadFontFile(fontPath);
    }

    if (desc.appendDefaultSystemFonts)
    {
        configured.LoadDefaultSystemFonts();
    }

    if (configured.GetFontCount() == 0u)
    {
        if (error)
        {
            *error = "No usable UI fonts were loaded";
        }
        return false;
    }

    UIFontFallbackChainDefaultState& state = GetDefaultFontFallbackChainState();
    state.chain = std::move(configured);
    ++state.generation;
    if (state.generation == 0u)
    {
        state.generation = 1u;
    }
    return true;
}

bool UIFontFallbackChain::ResetDefault(std::string* error)
{
    UIFontFallbackChainDesc desc;
    desc.appendDefaultSystemFonts = true;
    return ConfigureDefault(desc, error);
}

bool UIFontFallbackChain::LoadDefaultSystemFonts()
{
#if defined(_WIN32)
    constexpr std::array<const char*, 10> fontCandidates = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/calibri.ttf",
        "C:/Windows/Fonts/seguisym.ttf",
        "C:/Windows/Fonts/NotoSansSC-VF.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/Deng.ttf",
        "C:/Windows/Fonts/SimsunExtG.ttf",
    };
#elif defined(__APPLE__)
    constexpr std::array<const char*, 7> fontCandidates = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial.ttf",
    };
#else
    constexpr std::array<const char*, 10> fontCandidates = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.otf",
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
    };
#endif

    bool loadedAny = false;
    for (const char* candidate : fontCandidates)
    {
        UIFontMetrics font;
        if (font.LoadTrueTypeFile(candidate))
        {
            loadedAny = AddFont(std::move(font)) || loadedAny;
        }
    }

    return loadedAny;
}

bool UIFontFallbackChain::LoadFontFile(const std::string& path,
                                       uint32 fontIndex)
{
    UIFontMetrics font;
    if (!font.LoadTrueTypeFile(path, fontIndex))
    {
        return false;
    }

    return AddFont(std::move(font));
}

bool UIFontFallbackChain::AddFont(UIFontMetrics font)
{
    if (!font.HasTrueTypeData())
    {
        return false;
    }

    m_fonts.push_back(std::move(font));
    return true;
}

void UIFontFallbackChain::Clear()
{
    m_fonts.clear();
}

UIFontFallbackChainDiagnostics UIFontFallbackChain::GetDiagnostics(
    uint64 generation) const
{
    UIFontFallbackChainDiagnostics diagnostics;
    diagnostics.generation = generation;
    diagnostics.fontCount = GetFontCount();
    diagnostics.hasPrimaryFont = GetPrimaryFont() != nullptr;
    diagnostics.hasLatinText =
        HasCodepoint(static_cast<uint32>('A')) &&
        HasCodepoint(static_cast<uint32>('a')) &&
        HasCodepoint(static_cast<uint32>('0'));
    diagnostics.hasEditorSymbols =
        HasCodepoint(0x2026u) && HasCodepoint(0x2192u) &&
        HasCodepoint(0x2713u);
    diagnostics.hasCjkSample = HasCodepoint(0x4E2Du);

    for (uint32 codepoint : RVX_UI_CORE_FONT_DIAGNOSTIC_CODEPOINTS)
    {
        if (!HasCodepoint(codepoint))
        {
            diagnostics.missingCoreCodepoints.push_back(codepoint);
        }
    }
    diagnostics.missingCoreCodepointCount =
        static_cast<uint32>(diagnostics.missingCoreCodepoints.size());
    return diagnostics;
}

const UIFontMetrics* UIFontFallbackChain::GetPrimaryFont() const
{
    return m_fonts.empty() ? nullptr : &m_fonts.front();
}

uint32 UIFontFallbackChain::FindFontIndexForCodepoint(uint32 codepoint) const
{
    if (m_fonts.empty())
    {
        return RVX_INVALID_INDEX;
    }

    if (codepoint == '\t' || codepoint == '\n' || codepoint == '\r')
    {
        return 0u;
    }

    for (uint32 index = 0; index < static_cast<uint32>(m_fonts.size()); ++index)
    {
        if (m_fonts[index].HasCodepoint(codepoint))
        {
            return index;
        }
    }

    return RVX_INVALID_INDEX;
}

const UIFontMetrics* UIFontFallbackChain::FindFontForCodepoint(uint32 codepoint) const
{
    const uint32 fontIndex = FindFontIndexForCodepoint(codepoint);
    return fontIndex != RVX_INVALID_INDEX ? &m_fonts[fontIndex] : nullptr;
}

bool UIFontFallbackChain::HasCodepoint(uint32 codepoint) const
{
    return FindFontForCodepoint(codepoint) != nullptr;
}

bool UIFontFallbackChain::BuildAtlas(UIFontAtlas& atlas,
                                     const UIFontAtlasDesc& desc) const
{
    atlas.Reset();
    std::vector<uint32> codepoints = BuildAtlasCodepointList(desc);
    if (m_fonts.empty() ||
        desc.fontSize <= 0.0f ||
        desc.width == 0 ||
        desc.height == 0 ||
        codepoints.empty() ||
        codepoints.size() > 4096u)
    {
        return false;
    }

    UIFontAtlasDesc resolvedDesc = desc;
    resolvedDesc.padding = std::min<uint32>(resolvedDesc.padding, 64u);
    resolvedDesc.oversampleX = std::clamp<uint32>(resolvedDesc.oversampleX, 1u, 8u);
    resolvedDesc.oversampleY = std::clamp<uint32>(resolvedDesc.oversampleY, 1u, 8u);
    resolvedDesc.firstCodepoint = codepoints.front();
    resolvedDesc.codepointCount = static_cast<uint32>(codepoints.size());
    resolvedDesc.codepoints = codepoints;

    struct FontAtlasBuildGroup
    {
        uint32 fontIndex = 0;
        std::vector<uint32> codepoints;
        std::vector<int> stbCodepoints;
        std::vector<stbtt_packedchar> packedChars;
    };

    std::vector<FontAtlasBuildGroup> groups;
    std::vector<uint32> fontToGroup(m_fonts.size(), RVX_INVALID_INDEX);
    for (uint32 codepoint : codepoints)
    {
        const uint32 fontIndex = FindFontIndexForCodepoint(codepoint);
        if (fontIndex == RVX_INVALID_INDEX ||
            codepoint > static_cast<uint32>(std::numeric_limits<int>::max()))
        {
            continue;
        }

        uint32 groupIndex = fontToGroup[fontIndex];
        if (groupIndex == RVX_INVALID_INDEX)
        {
            groupIndex = static_cast<uint32>(groups.size());
            fontToGroup[fontIndex] = groupIndex;
            FontAtlasBuildGroup group;
            group.fontIndex = fontIndex;
            groups.push_back(std::move(group));
        }

        FontAtlasBuildGroup& group = groups[groupIndex];
        group.codepoints.push_back(codepoint);
        group.stbCodepoints.push_back(static_cast<int>(codepoint));
    }

    if (groups.empty())
    {
        return false;
    }

    atlas.m_desc = resolvedDesc;
    atlas.m_pixels.assign(static_cast<size_t>(resolvedDesc.width) * resolvedDesc.height, 0u);
    atlas.m_glyphs.clear();
    atlas.m_glyphs.reserve(codepoints.size());

    stbtt_pack_context packContext{};
    if (stbtt_PackBegin(&packContext,
                        atlas.m_pixels.data(),
                        static_cast<int>(resolvedDesc.width),
                        static_cast<int>(resolvedDesc.height),
                        0,
                        static_cast<int>(resolvedDesc.padding),
                        nullptr) == 0)
    {
        atlas.Reset();
        return false;
    }

    stbtt_PackSetOversampling(&packContext,
                              static_cast<uint32>(resolvedDesc.oversampleX),
                              static_cast<uint32>(resolvedDesc.oversampleY));

    bool packSucceeded = true;
    for (FontAtlasBuildGroup& group : groups)
    {
        group.packedChars.resize(group.codepoints.size());

        stbtt_pack_range packRange{};
        packRange.font_size = resolvedDesc.fontSize;
        packRange.first_unicode_codepoint_in_range = 0;
        packRange.array_of_unicode_codepoints = group.stbCodepoints.data();
        packRange.num_chars = static_cast<int>(group.stbCodepoints.size());
        packRange.chardata_for_range = group.packedChars.data();

        const UIFontMetrics::FontData& fontData = *m_fonts[group.fontIndex].m_fontData;
        const int packResult = stbtt_PackFontRanges(&packContext,
                                                    fontData.bytes.data(),
                                                    static_cast<int>(fontData.fontIndex),
                                                    &packRange,
                                                    1);
        if (packResult == 0)
        {
            packSucceeded = false;
            break;
        }
    }

    stbtt_PackEnd(&packContext);
    if (!packSucceeded)
    {
        atlas.Reset();
        return false;
    }

    const float invWidth = 1.0f / static_cast<float>(resolvedDesc.width);
    const float invHeight = 1.0f / static_cast<float>(resolvedDesc.height);
    for (const FontAtlasBuildGroup& group : groups)
    {
        for (uint32 i = 0; i < static_cast<uint32>(group.codepoints.size()); ++i)
        {
            const stbtt_packedchar& packed = group.packedChars[i];

            UIFontAtlasGlyph glyph;
            glyph.codepoint = group.codepoints[i];
            glyph.fontIndex = group.fontIndex;
            glyph.uvRect = Rect(static_cast<float>(packed.x0) * invWidth,
                                static_cast<float>(packed.y0) * invHeight,
                                static_cast<float>(packed.x1 - packed.x0) * invWidth,
                                static_cast<float>(packed.y1 - packed.y0) * invHeight);
            glyph.size = Vec2(packed.xoff2 - packed.xoff,
                              packed.yoff2 - packed.yoff);
            glyph.offset = Vec2(packed.xoff, packed.yoff);
            glyph.advance = packed.xadvance;
            glyph.valid = true;
            atlas.m_glyphs.push_back(glyph);
        }
    }

    atlas.m_layoutFonts =
        std::make_shared<UIFontFallbackChain>(*this);
    atlas.m_built = true;
    return true;
}

UIGlyphMetrics UIFontFallbackChain::GetCodepointMetrics(uint32 codepoint,
                                                        float fontSize) const
{
    const UIFontMetrics* font = FindFontForCodepoint(codepoint);
    return font ? font->GetCodepointMetrics(codepoint, fontSize)
                : UIFontMetrics::GetFallbackGlyphMetrics(codepoint, fontSize);
}

float UIFontFallbackChain::GetKerningAdvance(uint32 previousCodepoint,
                                             uint32 codepoint,
                                             float fontSize) const
{
    const UIFontMetrics* previousFont = FindFontForCodepoint(previousCodepoint);
    const UIFontMetrics* currentFont = FindFontForCodepoint(codepoint);
    if (!previousFont || previousFont != currentFont)
    {
        return 0.0f;
    }

    return currentFont->GetKerningAdvance(previousCodepoint, codepoint, fontSize);
}

UITextMetrics UIFontFallbackChain::MeasureText(const std::string& text,
                                               float fontSize) const
{
    UITextMetrics metrics;
    metrics.ascent = GetAscent(fontSize);
    metrics.descent = GetDescent(fontSize);
    metrics.lineGap = GetLineGap(fontSize);
    metrics.lineCount = text.empty() ? 0u : 1u;

    float currentLineWidth = 0.0f;
    uint32 previousCodepoint = 0u;
    bool hasPreviousCodepoint = false;
    size_t index = 0;
    while (index < text.size())
    {
        uint32 codepoint = 0;
        DecodeNextCodepoint(text, index, codepoint);
        if (codepoint == '\r')
        {
            continue;
        }
        if (codepoint == '\n')
        {
            metrics.width = std::max(metrics.width, currentLineWidth);
            currentLineWidth = 0.0f;
            previousCodepoint = 0u;
            hasPreviousCodepoint = false;
            ++metrics.lineCount;
            continue;
        }

        if (hasPreviousCodepoint)
        {
            currentLineWidth +=
                GetKerningAdvance(previousCodepoint, codepoint, fontSize);
        }
        currentLineWidth += GetCodepointMetrics(codepoint, fontSize).advance;
        previousCodepoint = codepoint;
        hasPreviousCodepoint = true;
        ++metrics.glyphCount;
    }

    metrics.width = std::max(metrics.width, currentLineWidth);
    if (metrics.lineCount > 0)
    {
        metrics.height =
            static_cast<float>(metrics.lineCount) * (metrics.ascent + metrics.descent) +
            static_cast<float>(metrics.lineCount - 1u) * metrics.lineGap;
    }
    return metrics;
}

float UIFontFallbackChain::GetAscent(float fontSize) const
{
    const UIFontMetrics* primary = GetPrimaryFont();
    return primary ? primary->GetAscent(fontSize) : std::max(0.0f, fontSize) * 0.78f;
}

float UIFontFallbackChain::GetDescent(float fontSize) const
{
    const UIFontMetrics* primary = GetPrimaryFont();
    return primary ? primary->GetDescent(fontSize) : std::max(0.0f, fontSize) * 0.22f;
}

float UIFontFallbackChain::GetLineGap(float fontSize) const
{
    const UIFontMetrics* primary = GetPrimaryFont();
    return primary ? primary->GetLineGap(fontSize) : std::max(0.0f, fontSize) * 0.2f;
}

float UIFontFallbackChain::GetLineHeight(float fontSize) const
{
    return GetAscent(fontSize) + GetDescent(fontSize) + GetLineGap(fontSize);
}

void UIRenderer::SetFontAtlas(const UIFontAtlas* atlas)
{
    if (m_fontAtlas == atlas)
    {
        return;
    }

    m_fontAtlas = atlas;
    m_runtimeFontAtlasExpansionEnabled = false;
    m_runtimeFontAtlas.Reset();
    m_runtimeFontAtlasDesc = {};
    m_runtimeFontAtlasCodepoints.clear();
    m_pendingFontAtlasCodepoints.clear();
    m_fontAtlasRuntimeStats = {};
    m_fontAtlasTexture.Reset();
    m_fontAtlasTextureView.Reset();
    m_fontAtlasStagingBuffer.Reset();
    m_fontAtlasDescriptorSet.Reset();
    m_uploadedFontAtlas = nullptr;
}

bool UIRenderer::Initialize(IRHIDevice* device)
{
    m_device = device;
    m_backend = device ? UIRenderBackend::RuntimeRHI : UIRenderBackend::None;
    m_fontAtlas = GetDefaultFontAtlas();
    m_runtimeFontAtlasExpansionEnabled = true;
    if (m_fontAtlas && m_fontAtlas->IsBuilt())
    {
        m_runtimeFontAtlasDesc = m_fontAtlas->GetDesc();
        m_runtimeFontAtlasCodepoints = m_runtimeFontAtlasDesc.codepoints;
        m_fontAtlasRuntimeStats.codepointCount =
            static_cast<uint32>(m_runtimeFontAtlasCodepoints.size());
    }
    ResetFrameData();
    return m_device != nullptr;
}

void UIRenderer::Shutdown()
{
    m_recording = false;
    m_device = nullptr;
    m_backend = UIRenderBackend::None;
    m_target = {};
    m_fontAtlas = nullptr;
    m_runtimeFontAtlasExpansionEnabled = false;
    m_runtimeFontAtlas.Reset();
    m_runtimeFontAtlasDesc = {};
    m_runtimeFontAtlasCodepoints.clear();
    m_pendingFontAtlasCodepoints.clear();
    m_fontAtlasRuntimeStats = {};
    ResetGPUResources();
    ResetFrameData();
}

void UIRenderer::BeginFrame(const UIRenderTargetDesc& target)
{
    ResetFrameData();
    m_submitStats = {};
    m_target = target;
    m_recording = m_device != nullptr &&
                  target.colorTarget != nullptr &&
                  target.width > 0 &&
                  target.height > 0 &&
                  target.format != RHIFormat::Unknown;
    InitializeClipStack();
}

void UIRenderer::BeginRecordOnlyFrame(uint32 width, uint32 height, RHIFormat format)
{
    ResetFrameData();
    m_submitStats = {};
    m_target = {};
    m_target.width = width;
    m_target.height = height;
    m_target.format = format;
    m_recording = width > 0 && height > 0 && format != RHIFormat::Unknown;
    InitializeClipStack();
}

bool UIRenderer::BindRecordedFrameTarget(const UIRenderTargetDesc& target)
{
    if (m_recording ||
        target.width == 0 ||
        target.height == 0 ||
        target.format == RHIFormat::Unknown ||
        target.width != m_target.width ||
        target.height != m_target.height)
    {
        return false;
    }

    m_target.colorTarget = target.colorTarget;
    m_target.format = target.format;
    return true;
}

void UIRenderer::EndFrame()
{
    m_recording = false;
    RebuildRuntimeFontAtlasIfNeeded();
}

void UIRenderer::QueueMissingFontCodepoint(uint32 codepoint)
{
    if (!m_fontAtlas ||
        !m_runtimeFontAtlasExpansionEnabled ||
        !IsRenderableGlyph(codepoint) ||
        codepoint > static_cast<uint32>(std::numeric_limits<int>::max()) ||
        m_fontAtlas->HasGlyph(codepoint) ||
        !UIFontFallbackChain::Default().HasCodepoint(codepoint))
    {
        return;
    }

    if (std::find(m_runtimeFontAtlasCodepoints.begin(),
                  m_runtimeFontAtlasCodepoints.end(),
                  codepoint) != m_runtimeFontAtlasCodepoints.end())
    {
        return;
    }

    if (std::find(m_pendingFontAtlasCodepoints.begin(),
                  m_pendingFontAtlasCodepoints.end(),
                  codepoint) != m_pendingFontAtlasCodepoints.end())
    {
        return;
    }

    m_pendingFontAtlasCodepoints.push_back(codepoint);
    m_fontAtlasRuntimeStats.pendingCodepointCount =
        static_cast<uint32>(m_pendingFontAtlasCodepoints.size());
    m_fontAtlasRuntimeStats.rebuildPending = true;
}

bool UIRenderer::RebuildRuntimeFontAtlasIfNeeded()
{
    if (m_pendingFontAtlasCodepoints.empty())
    {
        m_fontAtlasRuntimeStats.rebuildPending = false;
        return false;
    }

    if (m_runtimeFontAtlasDesc.width == 0u ||
        m_runtimeFontAtlasDesc.height == 0u ||
        m_runtimeFontAtlasDesc.fontSize <= 0.0f)
    {
        if (!m_fontAtlas || !m_fontAtlas->IsBuilt())
        {
            m_pendingFontAtlasCodepoints.clear();
            m_fontAtlasRuntimeStats.pendingCodepointCount = 0u;
            m_fontAtlasRuntimeStats.rebuildPending = false;
            return false;
        }

        m_runtimeFontAtlasDesc = m_fontAtlas->GetDesc();
        m_runtimeFontAtlasCodepoints = m_runtimeFontAtlasDesc.codepoints;
    }

    for (uint32 codepoint : m_pendingFontAtlasCodepoints)
    {
        if (std::find(m_runtimeFontAtlasCodepoints.begin(),
                      m_runtimeFontAtlasCodepoints.end(),
                      codepoint) == m_runtimeFontAtlasCodepoints.end())
        {
            m_runtimeFontAtlasCodepoints.push_back(codepoint);
        }
    }

    UIFontAtlasDesc atlasDesc = m_runtimeFontAtlasDesc;
    atlasDesc.codepoints = m_runtimeFontAtlasCodepoints;
    atlasDesc.firstCodepoint = atlasDesc.codepoints.empty()
                                   ? 0u
                                   : atlasDesc.codepoints.front();
    atlasDesc.codepointCount = static_cast<uint32>(atlasDesc.codepoints.size());

    UIFontAtlas rebuiltAtlas;
    if (!UIFontFallbackChain::Default().BuildAtlas(rebuiltAtlas, atlasDesc))
    {
        m_pendingFontAtlasCodepoints.clear();
        m_fontAtlasRuntimeStats.pendingCodepointCount = 0u;
        m_fontAtlasRuntimeStats.rebuildPending = false;
        return false;
    }

    m_runtimeFontAtlas = std::move(rebuiltAtlas);
    m_runtimeFontAtlasDesc = m_runtimeFontAtlas.GetDesc();
    m_runtimeFontAtlasCodepoints = m_runtimeFontAtlasDesc.codepoints;
    m_fontAtlas = &m_runtimeFontAtlas;
    m_fontAtlasTexture.Reset();
    m_fontAtlasTextureView.Reset();
    m_fontAtlasStagingBuffer.Reset();
    m_fontAtlasDescriptorSet.Reset();
    m_uploadedFontAtlas = nullptr;
    m_pendingFontAtlasCodepoints.clear();

    m_fontAtlasRuntimeStats.usesRuntimeAtlas = true;
    m_fontAtlasRuntimeStats.rebuildPending = false;
    m_fontAtlasRuntimeStats.pendingCodepointCount = 0u;
    m_fontAtlasRuntimeStats.codepointCount =
        static_cast<uint32>(m_runtimeFontAtlasCodepoints.size());
    ++m_fontAtlasRuntimeStats.rebuildCount;
    return true;
}

void UIRenderer::DrawRect(const Rect& rect, const UIColor& color)
{
    if (!m_recording || rect.width <= 0.0f || rect.height <= 0.0f || color.a <= 0.0f)
    {
        return;
    }
    const Rect clipRect = GetCurrentClipRect();
    if (!RectOverlapsPositive(rect, clipRect))
    {
        return;
    }

    UIDrawCommand command;
    command.type = UIDrawCommandType::Rect;
    command.bounds = rect;
    command.clipRect = clipRect;
    command.color = color;
    EmitQuad(rect, Rect(0.0f, 0.0f, 1.0f, 1.0f), color, command);
    m_commands.push_back(command);
    ++m_stats.rectCount;
    m_stats.commandCount = static_cast<uint32>(m_commands.size());
}

void UIRenderer::DrawBorder(const Rect& rect, const UIColor& color, float width)
{
    if (!m_recording || rect.width <= 0.0f || rect.height <= 0.0f ||
        width <= 0.0f || color.a <= 0.0f)
    {
        return;
    }

    const float clampedWidth =
        std::min(width, std::min(rect.width * 0.5f, rect.height * 0.5f));
    if (clampedWidth <= 0.0f)
    {
        return;
    }

    DrawRect(Rect(rect.x, rect.y, rect.width, clampedWidth), color);
    DrawRect(Rect(rect.x,
                  rect.Bottom() - clampedWidth,
                  rect.width,
                  clampedWidth),
             color);
    DrawRect(Rect(rect.x,
                  rect.y + clampedWidth,
                  clampedWidth,
                  rect.height - clampedWidth * 2.0f),
             color);
    DrawRect(Rect(rect.Right() - clampedWidth,
                  rect.y + clampedWidth,
                  clampedWidth,
                  rect.height - clampedWidth * 2.0f),
             color);
}

void UIRenderer::DrawLine(const Vec2& start,
                          const Vec2& end,
                          float thickness,
                          const UIColor& color)
{
    if (!m_recording || thickness <= 0.0f || color.a <= 0.0f)
    {
        return;
    }

    const Vec2 delta = end - start;
    const float lengthSquared = delta.x * delta.x + delta.y * delta.y;
    if (lengthSquared <= std::numeric_limits<float>::epsilon())
    {
        const float halfThickness = thickness * 0.5f;
        DrawRect(Rect(start.x - halfThickness,
                      start.y - halfThickness,
                      thickness,
                      thickness),
                 color);
        return;
    }

    const float length = std::sqrt(lengthSquared);
    const Vec2 normal(-delta.y / length * thickness * 0.5f,
                      delta.x / length * thickness * 0.5f);
    const Vec2 topLeft = start + normal;
    const Vec2 topRight = end + normal;
    const Vec2 bottomRight = end - normal;
    const Vec2 bottomLeft = start - normal;

    const float left = std::min({topLeft.x, topRight.x, bottomRight.x, bottomLeft.x});
    const float top = std::min({topLeft.y, topRight.y, bottomRight.y, bottomLeft.y});
    const float right = std::max({topLeft.x, topRight.x, bottomRight.x, bottomLeft.x});
    const float bottom = std::max({topLeft.y, topRight.y, bottomRight.y, bottomLeft.y});
    const Rect bounds(left, top, right - left, bottom - top);

    const Rect clipRect = GetCurrentClipRect();
    if (!RectOverlapsPositive(bounds, clipRect))
    {
        return;
    }

    UIDrawCommand command;
    command.type = UIDrawCommandType::Rect;
    command.bounds = bounds;
    command.clipRect = clipRect;
    command.color = color;
    EmitQuad(topLeft,
             topRight,
             bottomRight,
             bottomLeft,
             Rect(0.0f, 0.0f, 1.0f, 1.0f),
             color,
             command);
    m_commands.push_back(command);
    ++m_stats.rectCount;
    m_stats.commandCount = static_cast<uint32>(m_commands.size());
}

void UIRenderer::DrawText(const std::string& text,
                          const Rect& bounds,
                          float fontSize,
                          const UIColor& color,
                          TextAlign horizontalAlign,
                          VerticalAlign verticalAlign)
{
    if (!m_recording || text.empty() || fontSize <= 0.0f || color.a <= 0.0f ||
        bounds.width <= 0.0f || bounds.height <= 0.0f)
    {
        return;
    }
    const Rect clipRect = GetCurrentClipRect();
    if (!RectOverlapsPositive(bounds, clipRect))
    {
        return;
    }

    const UIFontFallbackChain& font = UIFontFallbackChain::Default();
    const UITextMetrics textMetrics = MeasureTextForAtlas(font, m_fontAtlas, text, fontSize);
    if (textMetrics.glyphCount == 0)
    {
        return;
    }

    float originX = bounds.x;
    if (horizontalAlign == TextAlign::Center)
    {
        originX += std::max(0.0f, (bounds.width - textMetrics.width) * 0.5f);
    }
    else if (horizontalAlign == TextAlign::Right)
    {
        originX += std::max(0.0f, bounds.width - textMetrics.width);
    }

    float originY = bounds.y;
    if (verticalAlign == VerticalAlign::Middle)
    {
        originY += std::max(0.0f, (bounds.height - textMetrics.height) * 0.5f);
    }
    else if (verticalAlign == VerticalAlign::Bottom)
    {
        originY += std::max(0.0f, bounds.height - textMetrics.height);
    }

    originX = std::round(originX);
    originY = std::round(originY);

    UIDrawCommand command;
    command.type = UIDrawCommandType::Text;
    command.bounds = Rect(originX, originY, textMetrics.width, textMetrics.height);
    command.clipRect = clipRect;
    command.color = color;
    command.text = text;
    command.fontSize = fontSize;
    command.firstVertex = static_cast<uint32>(m_vertices.size());
    command.firstIndex = static_cast<uint32>(m_indices.size());

    float penX = originX;
    float lineTop = originY;
    float baselineY = lineTop + textMetrics.ascent;
    uint32 previousCodepoint = 0u;
    bool hasPreviousCodepoint = false;
    size_t index = 0;
    while (index < text.size())
    {
        uint32 codepoint = 0;
        DecodeNextCodepoint(text, index, codepoint);
        if (codepoint == '\r')
        {
            continue;
        }
        if (codepoint == '\n')
        {
            penX = originX;
            lineTop += textMetrics.ascent +
                       textMetrics.descent +
                       textMetrics.lineGap;
            baselineY = lineTop + textMetrics.ascent;
            previousCodepoint = 0u;
            hasPreviousCodepoint = false;
            continue;
        }

        if (hasPreviousCodepoint)
        {
            penX += GetTextKerningAdvance(
                font,
                m_fontAtlas,
                previousCodepoint,
                codepoint,
                fontSize);
        }
        const UIGlyphMetrics glyphMetrics = font.GetCodepointMetrics(codepoint, fontSize);
        float glyphAdvance = glyphMetrics.advance;
        if (IsRenderableGlyph(codepoint))
        {
            const UIFontAtlasGlyph* atlasGlyph = m_fontAtlas ? m_fontAtlas->FindGlyph(codepoint) : nullptr;
            if (!atlasGlyph)
            {
                QueueMissingFontCodepoint(codepoint);
            }
            const Rect glyphUV = (atlasGlyph && m_fontAtlas)
                                     ? InsetGlyphUVForLinearSampling(*m_fontAtlas,
                                                                     atlasGlyph->uvRect)
                                     : GlyphAtlasUV(codepoint);
            Rect glyphRect;
            if (atlasGlyph && m_fontAtlas)
            {
                const float atlasScale = GetAtlasGlyphScale(*m_fontAtlas, fontSize);
                glyphAdvance = atlasGlyph->advance * atlasScale;
                glyphRect = Rect(penX + atlasGlyph->offset.x * atlasScale,
                                 baselineY + atlasGlyph->offset.y * atlasScale,
                                 atlasGlyph->size.x * atlasScale,
                                 atlasGlyph->size.y * atlasScale);
            }
            else
            {
                glyphRect = Rect(penX + glyphMetrics.bearingX,
                                 baselineY - glyphMetrics.bearingY,
                                 glyphMetrics.width,
                                 glyphMetrics.height);
            }

            if (glyphRect.width > 0.0f && glyphRect.height > 0.0f)
            {
                UIDrawCommand glyphCommand;
                EmitQuad(SnapGlyphRectToPixelGrid(glyphRect), glyphUV, color, glyphCommand);
            }
        }
        else
        {
            const float atlasAdvance = GetAtlasGlyphAdvance(m_fontAtlas, codepoint, fontSize);
            if (atlasAdvance >= 0.0f)
            {
                glyphAdvance = atlasAdvance;
            }
        }
        penX += glyphAdvance;
        previousCodepoint = codepoint;
        hasPreviousCodepoint = true;
    }

    command.vertexCount = static_cast<uint32>(m_vertices.size()) - command.firstVertex;
    command.indexCount = static_cast<uint32>(m_indices.size()) - command.firstIndex;
    if (command.vertexCount == 0 || command.indexCount == 0)
    {
        return;
    }

    m_commands.push_back(command);
    ++m_stats.textCount;
    m_stats.glyphCount += textMetrics.glyphCount;
    m_stats.commandCount = static_cast<uint32>(m_commands.size());
    m_stats.vertexCount = static_cast<uint32>(m_vertices.size());
    m_stats.indexCount = static_cast<uint32>(m_indices.size());
}

void UIRenderer::DrawImage(RHITextureView* textureView,
                           const Rect& rect,
                           const Rect& uvRect,
                           const UIColor& color)
{
    if (!m_recording || textureView == nullptr ||
        rect.width <= 0.0f || rect.height <= 0.0f || color.a <= 0.0f)
    {
        return;
    }
    const Rect clipRect = GetCurrentClipRect();
    if (!RectOverlapsPositive(rect, clipRect))
    {
        return;
    }

    UIDrawCommand command;
    command.type = UIDrawCommandType::Image;
    command.bounds = rect;
    command.clipRect = clipRect;
    command.uvRect = uvRect;
    command.color = color;
    command.textureView = textureView;
    EmitQuad(rect, uvRect, color, command);
    m_commands.push_back(command);
    ++m_stats.imageCount;
    m_stats.commandCount = static_cast<uint32>(m_commands.size());
}

void UIRenderer::PushClipRect(const Rect& rect)
{
    if (!m_recording)
    {
        return;
    }

    const Rect current = GetCurrentClipRect();
    m_clipStack.push_back(IntersectRect(current, rect));
}

void UIRenderer::PopClipRect()
{
    if (m_clipStack.size() > 1u)
    {
        m_clipStack.pop_back();
    }
}

Rect UIRenderer::GetCurrentClipRect() const
{
    if (!m_clipStack.empty())
    {
        return m_clipStack.back();
    }
    return Rect(0.0f,
                0.0f,
                static_cast<float>(m_target.width),
                static_cast<float>(m_target.height));
}

UITextMetrics UIRenderer::MeasureText(const std::string& text, float fontSize) const
{
    return MeasureTextForAtlas(UIFontFallbackChain::Default(),
                               m_fontAtlas,
                               text,
                               fontSize);
}

bool UIRenderer::Submit(const UIRenderSubmitDesc& desc)
{
    m_submitStats = {};
    const bool useDefaultFramebufferTarget =
        desc.allowDefaultFramebufferTarget && m_target.colorTarget == nullptr;
    if (!m_device ||
        !desc.commandContext ||
        !desc.pipeline ||
        (!m_target.colorTarget && !useDefaultFramebufferTarget) ||
        m_target.width == 0 ||
        m_target.height == 0 ||
        m_target.format == RHIFormat::Unknown ||
        m_commands.empty() ||
        m_vertices.empty() ||
        m_indices.empty())
    {
        return false;
    }

    RHICommandContext& context = *desc.commandContext;
    ApplyTextureSetLayoutOverride(desc.textureSetLayout);
    if (!EnsureTextureDescriptorResources() ||
        !EnsureWhiteTexture(context) ||
        !EnsureFontAtlasTexture(context) ||
        !EnsureDynamicBuffers())
    {
        return false;
    }

    for (const UIDrawCommand& command : m_commands)
    {
        if (!GetDescriptorSetForCommand(command))
        {
            return false;
        }
    }

    const uint64 vertexBytes = static_cast<uint64>(m_vertices.size() * sizeof(UIVertex));
    const uint64 indexBytes = static_cast<uint64>(m_indices.size() * sizeof(uint32));

    std::vector<UIVertex> submitVertices = m_vertices;
    const float invTargetWidth = 1.0f / static_cast<float>(m_target.width);
    const float invTargetHeight = 1.0f / static_cast<float>(m_target.height);
    for (UIVertex& vertex : submitVertices)
    {
        vertex.position.x = vertex.position.x * invTargetWidth * 2.0f - 1.0f;
        vertex.position.y = 1.0f - vertex.position.y * invTargetHeight * 2.0f;
    }
    if (!UploadBuffer(m_vertexBuffer.Get(), submitVertices.data(), vertexBytes) ||
        !UploadBuffer(m_indexBuffer.Get(), m_indices.data(), indexBytes))
    {
        return false;
    }

    context.BufferBarrier(m_vertexBuffer.Get(), RHIResourceState::Common, RHIResourceState::VertexBuffer);
    context.BufferBarrier(m_indexBuffer.Get(), RHIResourceState::Common, RHIResourceState::IndexBuffer);

    if (desc.beginRenderPass)
    {
        RHIRenderPassDesc renderPass;
        if (!useDefaultFramebufferTarget)
        {
            renderPass.AddColorAttachment(m_target.colorTarget,
                                          desc.colorLoadOp,
                                          desc.colorStoreOp,
                                          desc.clearColor);
        }
        renderPass.SetRenderArea(0, 0, m_target.width, m_target.height);
        context.BeginRenderPass(renderPass);
    }

    RHIViewport viewport;
    viewport.width = static_cast<float>(m_target.width);
    viewport.height = static_cast<float>(m_target.height);
    context.SetViewport(viewport);
    RHIRect currentScissor{0, 0, m_target.width, m_target.height};
    context.SetScissor(currentScissor);
    context.SetPipeline(desc.pipeline);
    context.SetVertexBuffer(0, m_vertexBuffer.Get());
    context.SetIndexBuffer(m_indexBuffer.Get(), RHIFormat::R32_UINT);

    UIPushConstants pushConstants;
    pushConstants.targetSize[0] = static_cast<float>(m_target.width);
    pushConstants.targetSize[1] = static_cast<float>(m_target.height);
    pushConstants.inverseTargetSize[0] = m_target.width > 0 ? 1.0f / static_cast<float>(m_target.width) : 0.0f;
    pushConstants.inverseTargetSize[1] = m_target.height > 0 ? 1.0f / static_cast<float>(m_target.height) : 0.0f;
    context.SetPushConstants(&pushConstants, sizeof(pushConstants));

    RHIDescriptorSet* currentDescriptorSet = nullptr;
    for (const UIDrawCommand& command : m_commands)
    {
        const RHIRect commandScissor =
            ToScissorRect(command.clipRect, m_target.width, m_target.height);
        if (commandScissor.width == 0u || commandScissor.height == 0u)
        {
            continue;
        }
        if (!SameScissor(commandScissor, currentScissor))
        {
            context.SetScissor(commandScissor);
            currentScissor = commandScissor;
        }

        RHIDescriptorSet* descriptorSet = GetDescriptorSetForCommand(command);
        if (descriptorSet && descriptorSet != currentDescriptorSet)
        {
            context.SetDescriptorSet(0, descriptorSet);
            currentDescriptorSet = descriptorSet;
        }

        context.DrawIndexed(command.indexCount, 1, command.firstIndex, 0, 0);
        ++m_submitStats.drawCallCount;
    }

    if (desc.beginRenderPass)
    {
        context.EndRenderPass();
    }

    m_submitStats.submitted = m_submitStats.drawCallCount > 0;
    m_submitStats.defaultFramebufferTarget = useDefaultFramebufferTarget;
    m_submitStats.vertexBytesUploaded = vertexBytes;
    m_submitStats.indexBytesUploaded = indexBytes;
    return m_submitStats.submitted;
}

void UIRenderer::ResetFrameData()
{
    m_commands.clear();
    m_vertices.clear();
    m_indices.clear();
    m_clipStack.clear();
    m_stats = {};
}

void UIRenderer::InitializeClipStack()
{
    m_clipStack.clear();
    if (m_target.width > 0u && m_target.height > 0u)
    {
        m_clipStack.push_back(Rect(0.0f,
                                   0.0f,
                                   static_cast<float>(m_target.width),
                                   static_cast<float>(m_target.height)));
    }
}

void UIRenderer::ResetGPUResources()
{
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_vertexBufferCapacity = 0;
    m_indexBufferCapacity = 0;

    m_fontAtlasTexture.Reset();
    m_fontAtlasTextureView.Reset();
    m_fontAtlasStagingBuffer.Reset();
    m_fontAtlasDescriptorSet.Reset();
    m_uploadedFontAtlas = nullptr;

    m_whiteTexture.Reset();
    m_whiteTextureView.Reset();
    m_whiteTextureStagingBuffer.Reset();
    m_whiteDescriptorSet.Reset();

    m_textureSampler.Reset();
    m_fontSampler.Reset();
    m_textureSetLayout.Reset();
    m_textureSetLayoutOverride = nullptr;
    m_textureDescriptorSets.clear();
    m_submitStats = {};
}

bool UIRenderer::EnsureDynamicBuffers()
{
    const uint64 vertexBytes = static_cast<uint64>(m_vertices.size() * sizeof(UIVertex));
    const uint64 indexBytes = static_cast<uint64>(m_indices.size() * sizeof(uint32));
    return EnsureBuffer(m_vertexBuffer,
                        m_vertexBufferCapacity,
                        vertexBytes,
                        RHIBufferUsage::Vertex,
                        "UI.VertexBuffer",
                        sizeof(UIVertex)) &&
           EnsureBuffer(m_indexBuffer,
                        m_indexBufferCapacity,
                        indexBytes,
                        RHIBufferUsage::Index,
                        "UI.IndexBuffer",
                        0);
}

bool UIRenderer::EnsureBuffer(RHIBufferRef& buffer,
                              uint64& capacity,
                              uint64 requiredSize,
                              RHIBufferUsage usage,
                              const char* debugName,
                              uint32 stride)
{
    if (requiredSize == 0)
    {
        return true;
    }
    if (buffer && capacity >= requiredSize)
    {
        return true;
    }
    if (!m_device)
    {
        return false;
    }

    const uint64 newCapacity = GrowCapacity(capacity, requiredSize);
    RHIBufferDesc desc;
    desc.size = newCapacity;
    desc.usage = usage;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = stride;
    desc.debugName = debugName;

    RHIBufferRef newBuffer = m_device->CreateBuffer(desc);
    if (!newBuffer)
    {
        return false;
    }

    buffer = std::move(newBuffer);
    capacity = newCapacity;
    return true;
}

bool UIRenderer::UploadBuffer(RHIBuffer* buffer, const void* data, uint64 size)
{
    if (!buffer || !data || size == 0)
    {
        return false;
    }

    void* mapped = buffer->Map();
    if (!mapped)
    {
        return false;
    }

    std::memcpy(mapped, data, static_cast<size_t>(size));
    buffer->Unmap();
    return true;
}

bool UIRenderer::EnsureTextureDescriptorResources()
{
    if (!m_device)
    {
        return false;
    }

    if (!m_textureSampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.mipFilter = RHIFilterMode::Nearest;
        samplerDesc.maxLod = 0.0f;
        samplerDesc.debugName = "UI.LinearClampSampler";
        m_textureSampler = m_device->CreateSampler(samplerDesc);
    }

    if (!m_fontSampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.mipFilter = RHIFilterMode::Nearest;
        samplerDesc.maxLod = 0.0f;
        samplerDesc.debugName = "UI.FontAtlasLinearSampler";
        m_fontSampler = m_device->CreateSampler(samplerDesc);
    }

    if (!m_textureSetLayoutOverride && !m_textureSetLayout)
    {
        RHIDescriptorSetLayoutDesc layoutDesc;
        layoutDesc.debugName = "UI.TextureSetLayout";
        layoutDesc.AddBinding(
            0,
            RHIBindingType::SampledTexture,
            RHIShaderStage::Pixel);
        layoutDesc.AddBinding(
            1,
            RHIBindingType::Sampler,
            RHIShaderStage::Pixel);
        m_textureSetLayout = m_device->CreateDescriptorSetLayout(layoutDesc);
    }

    return m_textureSampler && m_fontSampler && GetActiveTextureSetLayout();
}

bool UIRenderer::EnsureWhiteTexture(RHICommandContext& context)
{
    if (m_whiteTexture && m_whiteTextureView && m_whiteDescriptorSet)
    {
        return true;
    }
    RHIDescriptorSetLayout* textureSetLayout = GetActiveTextureSetLayout();
    if (!m_device || !textureSetLayout || !m_fontSampler)
    {
        return false;
    }

    constexpr std::array<uint8, 4> whitePixel = {255u, 255u, 255u, 255u};

    RHITextureDesc textureDesc = RHITextureDesc::Texture2D(1,
                                                          1,
                                                          RHIFormat::RGBA8_UNORM,
                                                          RHITextureUsage::ShaderResource |
                                                              RHITextureUsage::CopyDst);
    textureDesc.debugName = "UI.WhiteTexture";
    m_whiteTexture = m_device->CreateTexture(textureDesc);
    if (!m_whiteTexture)
    {
        return false;
    }

    RHITextureViewDesc viewDesc;
    viewDesc.type = RHITextureViewType::ShaderResource;
    viewDesc.format = RHIFormat::RGBA8_UNORM;
    viewDesc.debugName = "UI.WhiteTextureView";
    m_whiteTextureView = m_device->CreateTextureView(m_whiteTexture.Get(), viewDesc);
    if (!m_whiteTextureView)
    {
        return false;
    }

    RHIStagingBufferDesc stagingDesc;
    stagingDesc.size = whitePixel.size();
    stagingDesc.debugName = "UI.WhiteTextureUpload";
    m_whiteTextureStagingBuffer = m_device->CreateStagingBuffer(stagingDesc);
    if (!m_whiteTextureStagingBuffer)
    {
        return false;
    }

    void* mapped = m_whiteTextureStagingBuffer->Map(0, whitePixel.size());
    if (!mapped)
    {
        return false;
    }
    std::memcpy(mapped, whitePixel.data(), whitePixel.size());
    m_whiteTextureStagingBuffer->Unmap();

    context.TextureBarrier(m_whiteTexture.Get(), RHIResourceState::Common, RHIResourceState::CopyDest);

    RHIBufferTextureCopyDesc copyDesc;
    copyDesc.bufferRowPitch = 4;
    copyDesc.bufferImageHeight = 1;
    copyDesc.textureRegion = {0, 0, 1, 1};
    context.CopyBufferToTexture(m_whiteTextureStagingBuffer->GetBuffer(), m_whiteTexture.Get(), copyDesc);
    context.TextureBarrier(m_whiteTexture.Get(), RHIResourceState::CopyDest, RHIResourceState::ShaderResource);

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.debugName = "UI.WhiteTextureSet";
    descriptorDesc.SetLayout(textureSetLayout)
                  .BindTexture(0, m_whiteTextureView.Get())
                  .BindSampler(1, m_textureSampler.Get());
    m_whiteDescriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
    if (!m_whiteDescriptorSet)
    {
        return false;
    }

    m_submitStats.whiteTextureUploaded = true;
    m_submitStats.whiteTextureBytesUploaded = whitePixel.size();
    return true;
}

bool UIRenderer::EnsureFontAtlasTexture(RHICommandContext& context)
{
    if (!m_fontAtlas || !m_fontAtlas->IsBuilt())
    {
        return true;
    }
    if (m_uploadedFontAtlas == m_fontAtlas &&
        m_fontAtlasTexture &&
        m_fontAtlasTextureView &&
        m_fontAtlasDescriptorSet)
    {
        return true;
    }
    RHIDescriptorSetLayout* textureSetLayout = GetActiveTextureSetLayout();
    if (!m_device || !textureSetLayout || !m_textureSampler)
    {
        return false;
    }

    const std::vector<uint8> rgbaPixels = ExpandAlphaAtlasToRGBA(*m_fontAtlas);
    if (rgbaPixels.empty())
    {
        return false;
    }

    RHITextureDesc textureDesc = RHITextureDesc::Texture2D(m_fontAtlas->GetWidth(),
                                                          m_fontAtlas->GetHeight(),
                                                          RHIFormat::RGBA8_UNORM,
                                                          RHITextureUsage::ShaderResource |
                                                              RHITextureUsage::CopyDst);
    textureDesc.debugName = "UI.FontAtlasTexture";
    m_fontAtlasTexture = m_device->CreateTexture(textureDesc);
    if (!m_fontAtlasTexture)
    {
        return false;
    }

    RHITextureViewDesc viewDesc;
    viewDesc.type = RHITextureViewType::ShaderResource;
    viewDesc.format = RHIFormat::RGBA8_UNORM;
    viewDesc.debugName = "UI.FontAtlasTextureView";
    m_fontAtlasTextureView = m_device->CreateTextureView(m_fontAtlasTexture.Get(), viewDesc);
    if (!m_fontAtlasTextureView)
    {
        return false;
    }

    RHIStagingBufferDesc stagingDesc;
    stagingDesc.size = static_cast<uint64>(rgbaPixels.size());
    stagingDesc.debugName = "UI.FontAtlasUpload";
    m_fontAtlasStagingBuffer = m_device->CreateStagingBuffer(stagingDesc);
    if (!m_fontAtlasStagingBuffer)
    {
        return false;
    }

    void* mapped = m_fontAtlasStagingBuffer->Map(0, stagingDesc.size);
    if (!mapped)
    {
        return false;
    }
    std::memcpy(mapped, rgbaPixels.data(), rgbaPixels.size());
    m_fontAtlasStagingBuffer->Unmap();

    context.TextureBarrier(m_fontAtlasTexture.Get(), RHIResourceState::Common, RHIResourceState::CopyDest);

    RHIBufferTextureCopyDesc copyDesc;
    copyDesc.bufferRowPitch = m_fontAtlas->GetWidth() * 4u;
    copyDesc.bufferImageHeight = m_fontAtlas->GetHeight();
    copyDesc.textureRegion = {0, 0, m_fontAtlas->GetWidth(), m_fontAtlas->GetHeight()};
    context.CopyBufferToTexture(m_fontAtlasStagingBuffer->GetBuffer(), m_fontAtlasTexture.Get(), copyDesc);
    context.TextureBarrier(m_fontAtlasTexture.Get(), RHIResourceState::CopyDest, RHIResourceState::ShaderResource);

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.debugName = "UI.FontAtlasTextureSet";
    descriptorDesc.SetLayout(textureSetLayout)
                  .BindTexture(0, m_fontAtlasTextureView.Get())
                  .BindSampler(1, m_fontSampler.Get());
    m_fontAtlasDescriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
    if (!m_fontAtlasDescriptorSet)
    {
        return false;
    }

    m_uploadedFontAtlas = m_fontAtlas;
    m_submitStats.atlasUploaded = true;
    m_submitStats.atlasBytesUploaded = static_cast<uint64>(rgbaPixels.size());
    return true;
}

RHIDescriptorSet* UIRenderer::GetOrCreateTextureDescriptor(RHITextureView* textureView)
{
    RHIDescriptorSetLayout* textureSetLayout = GetActiveTextureSetLayout();
    if (!textureView || !m_device || !textureSetLayout || !m_textureSampler)
    {
        return nullptr;
    }

    const auto it = std::find_if(m_textureDescriptorSets.begin(),
                                 m_textureDescriptorSets.end(),
                                 [textureView](const auto& entry) {
                                     return entry.first == textureView;
                                 });
    if (it != m_textureDescriptorSets.end())
    {
        return it->second.Get();
    }

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.debugName = "UI.ImageTextureSet";
    descriptorDesc.SetLayout(textureSetLayout)
                  .BindTexture(0, textureView)
                  .BindSampler(1, m_textureSampler.Get());
    RHIDescriptorSetRef descriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
    if (!descriptorSet)
    {
        return nullptr;
    }

    RHIDescriptorSet* rawDescriptorSet = descriptorSet.Get();
    m_textureDescriptorSets.emplace_back(textureView, std::move(descriptorSet));
    return rawDescriptorSet;
}

RHIDescriptorSet* UIRenderer::GetDescriptorSetForCommand(const UIDrawCommand& command)
{
    if (command.type == UIDrawCommandType::Text)
    {
        return m_fontAtlasDescriptorSet ? m_fontAtlasDescriptorSet.Get() : m_whiteDescriptorSet.Get();
    }
    if (command.type == UIDrawCommandType::Image)
    {
        return GetOrCreateTextureDescriptor(command.textureView);
    }
    return m_whiteDescriptorSet.Get();
}

RHIDescriptorSetLayout* UIRenderer::GetActiveTextureSetLayout() const
{
    return m_textureSetLayoutOverride ? m_textureSetLayoutOverride : m_textureSetLayout.Get();
}

void UIRenderer::ApplyTextureSetLayoutOverride(RHIDescriptorSetLayout* layout)
{
    if (m_textureSetLayoutOverride == layout)
    {
        return;
    }

    m_textureSetLayoutOverride = layout;
    m_whiteDescriptorSet.Reset();
    m_fontAtlasDescriptorSet.Reset();
    m_textureDescriptorSets.clear();
}

void UIRenderer::EmitQuad(const Rect& rect,
                          const Rect& uvRect,
                          const UIColor& color,
                          UIDrawCommand& command)
{
    EmitQuad(Vec2(rect.Left(), rect.Top()),
             Vec2(rect.Right(), rect.Top()),
             Vec2(rect.Right(), rect.Bottom()),
             Vec2(rect.Left(), rect.Bottom()),
             uvRect,
             color,
             command);
}

void UIRenderer::EmitQuad(const Vec2& topLeft,
                          const Vec2& topRight,
                          const Vec2& bottomRight,
                          const Vec2& bottomLeft,
                          const Rect& uvRect,
                          const UIColor& color,
                          UIDrawCommand& command)
{
    const uint32 firstVertex = static_cast<uint32>(m_vertices.size());
    const uint32 firstIndex = static_cast<uint32>(m_indices.size());

    m_vertices.push_back({topLeft, Vec2(uvRect.Left(), uvRect.Top()), color});
    m_vertices.push_back({topRight, Vec2(uvRect.Right(), uvRect.Top()), color});
    m_vertices.push_back({bottomRight, Vec2(uvRect.Right(), uvRect.Bottom()), color});
    m_vertices.push_back({bottomLeft, Vec2(uvRect.Left(), uvRect.Bottom()), color});

    m_indices.push_back(firstVertex + 0u);
    m_indices.push_back(firstVertex + 1u);
    m_indices.push_back(firstVertex + 2u);
    m_indices.push_back(firstVertex + 0u);
    m_indices.push_back(firstVertex + 2u);
    m_indices.push_back(firstVertex + 3u);

    if (command.vertexCount == 0 && command.indexCount == 0)
    {
        command.firstVertex = firstVertex;
        command.vertexCount = 4;
        command.firstIndex = firstIndex;
        command.indexCount = 6;
    }

    m_stats.vertexCount = static_cast<uint32>(m_vertices.size());
    m_stats.indexCount = static_cast<uint32>(m_indices.size());
}

} // namespace RVX::UI
