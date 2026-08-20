/**
 * @file UITextOverflow.cpp
 * @brief Single-line UI text overflow helper implementation
 */

#include "UI/UITextOverflow.h"

#include "UI/UIRenderer.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace RVX::UI
{
namespace
{
    constexpr const char* RVX_UI_ELLIPSIS_TEXT = "...";

    size_t GetUTF8SequenceLength(std::string_view text, size_t index)
    {
        if (index >= text.size())
        {
            return 0u;
        }

        const unsigned char lead =
            static_cast<unsigned char>(text[index]);
        if (lead < 0x80u)
        {
            return 1u;
        }

        size_t length = 1u;
        if ((lead & 0xE0u) == 0xC0u)
        {
            length = 2u;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            length = 3u;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            length = 4u;
        }

        if (index + length > text.size())
        {
            return 1u;
        }

        for (size_t offset = 1u; offset < length; ++offset)
        {
            const unsigned char next =
                static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0u) != 0x80u)
            {
                return 1u;
            }
        }
        return length;
    }

    std::vector<std::string_view> SplitUTF8Tokens(std::string_view text)
    {
        std::vector<std::string_view> tokens;
        tokens.reserve(text.size());

        size_t index = 0u;
        while (index < text.size())
        {
            const size_t length = std::max<size_t>(
                1u,
                GetUTF8SequenceLength(text, index));
            tokens.emplace_back(text.data() + index, length);
            index += length;
        }
        return tokens;
    }

    std::string JoinTokens(const std::vector<std::string_view>& tokens,
                           size_t begin,
                           size_t count)
    {
        if (begin >= tokens.size() || count == 0u)
        {
            return {};
        }

        const size_t end = std::min(tokens.size(), begin + count);
        std::string text;
        for (size_t index = begin; index < end; ++index)
        {
            text.append(tokens[index].data(), tokens[index].size());
        }
        return text;
    }

    bool TextFits(UIRenderer& renderer,
                  const std::string& text,
                  float width,
                  float fontSize)
    {
        return renderer.MeasureText(text, fontSize).width <= width + 0.5f;
    }

    std::string FitEndEllipsis(UIRenderer& renderer,
                               const std::string& text,
                               float width,
                               float fontSize)
    {
        if (TextFits(renderer, RVX_UI_ELLIPSIS_TEXT, width, fontSize))
        {
            const std::vector<std::string_view> tokens =
                SplitUTF8Tokens(text);
            size_t low = 0u;
            size_t high = tokens.size();
            while (low < high)
            {
                const size_t mid = (low + high + 1u) / 2u;
                std::string candidate = JoinTokens(tokens, 0u, mid);
                candidate += RVX_UI_ELLIPSIS_TEXT;
                if (TextFits(renderer, candidate, width, fontSize))
                {
                    low = mid;
                }
                else
                {
                    high = mid - 1u;
                }
            }

            std::string result = JoinTokens(tokens, 0u, low);
            result += RVX_UI_ELLIPSIS_TEXT;
            return result;
        }

        return {};
    }

    std::string FitMiddleEllipsis(UIRenderer& renderer,
                                  const std::string& text,
                                  float width,
                                  float fontSize)
    {
        if (!TextFits(renderer, RVX_UI_ELLIPSIS_TEXT, width, fontSize))
        {
            return {};
        }

        const std::vector<std::string_view> tokens = SplitUTF8Tokens(text);
        size_t low = 0u;
        size_t high = tokens.size();
        std::string best = RVX_UI_ELLIPSIS_TEXT;
        while (low < high)
        {
            const size_t visibleCount = (low + high + 1u) / 2u;
            const size_t headCount =
                visibleCount <= 1u
                    ? 0u
                    : std::max<size_t>(1u, visibleCount / 3u);
            const size_t tailCount = visibleCount - headCount;

            std::string candidate = JoinTokens(tokens, 0u, headCount);
            candidate += RVX_UI_ELLIPSIS_TEXT;
            candidate += JoinTokens(tokens,
                                    tokens.size() - tailCount,
                                    tailCount);
            if (TextFits(renderer, candidate, width, fontSize))
            {
                best = std::move(candidate);
                low = visibleCount;
            }
            else
            {
                high = visibleCount - 1u;
            }
        }

        return best;
    }
} // namespace

std::string ResolveSingleLineTextOverflow(UIRenderer& renderer,
                                          const std::string& text,
                                          float width,
                                          float fontSize,
                                          TextOverflowMode mode)
{
    if (text.empty() ||
        mode == TextOverflowMode::Clip ||
        width <= 0.0f ||
        TextFits(renderer, text, width, fontSize))
    {
        return text;
    }

    switch (mode)
    {
        case TextOverflowMode::EndEllipsis:
            return FitEndEllipsis(renderer, text, width, fontSize);
        case TextOverflowMode::MiddleEllipsis:
            return FitMiddleEllipsis(renderer, text, width, fontSize);
        case TextOverflowMode::Clip:
            break;
    }

    return text;
}

} // namespace RVX::UI
