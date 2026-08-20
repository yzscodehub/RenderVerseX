/**
 * @file UIRenderPacket.h
 * @brief Backend-neutral immutable UI rendering data contract
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderIdentity.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace RVX
{
    /** @brief Backend-neutral UI primitive classification. */
    enum class UIRenderPrimitive : uint8
    {
        Rect = 0,
        Text,
        Image
    };

    /** @brief Backend-neutral source selected by one UI draw command. */
    enum class UIRenderResourceKind : uint8
    {
        White = 0,
        FontAtlas,
        Texture
    };

    /** @brief Pixel-space rectangle carried across the UI/Render boundary. */
    struct UIRenderRect
    {
        float32 x = 0.0f;
        float32 y = 0.0f;
        float32 width = 0.0f;
        float32 height = 0.0f;
    };

    /** @brief Vertex data consumed by the Render-owned UI pass. */
    struct UIRenderVertex
    {
        Vec2 position{0.0f};
        Vec2 uv{0.0f};
        Vec4 color{1.0f};
    };

    /** @brief Draw range and backend-neutral resource identity. */
    struct UIRenderDrawCommand
    {
        UIRenderPrimitive primitive = UIRenderPrimitive::Rect;
        UIRenderRect clipRect{};
        UIRenderResourceKind resourceKind = UIRenderResourceKind::White;
        RenderResourceHandle texture{};
        uint32 firstIndex = 0;
        uint32 indexCount = 0;
    };

    /** @brief CPU-owned alpha atlas payload used by the Render UI pass. */
    struct UIRenderFontAtlas
    {
        uint32 width = 0;
        uint32 height = 0;
        uint64 revision = 0;
        std::shared_ptr<const std::vector<uint8>> alphaPixels;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return width != 0 && height != 0 && revision != 0 && alphaPixels &&
                   alphaPixels->size() ==
                       static_cast<size_t>(width) * static_cast<size_t>(height);
        }
    };

    /**
     * @brief Complete backend-neutral UI packet.
     *
     * The packet owns all CPU data needed by Render. It deliberately contains
     * no RHI object, format, command context, pipeline, or descriptor handle.
     */
    struct UIRenderPacket
    {
        uint32 targetWidth = 0;
        uint32 targetHeight = 0;
        uint64 generation = 0;
        std::vector<UIRenderVertex> vertices;
        std::vector<uint32> indices;
        std::vector<UIRenderDrawCommand> commands;
        UIRenderFontAtlas fontAtlas;

        [[nodiscard]] bool IsValid() const noexcept
        {
            if (targetWidth == 0 || targetHeight == 0 || generation == 0 ||
                vertices.empty() || indices.empty() || commands.empty())
            {
                return false;
            }
            for (const UIRenderDrawCommand& command : commands)
            {
                const uint64 end = static_cast<uint64>(command.firstIndex) +
                                   static_cast<uint64>(command.indexCount);
                if (command.indexCount == 0 || end > indices.size() ||
                    (command.resourceKind == UIRenderResourceKind::Texture &&
                     !command.texture.IsValid()))
                {
                    return false;
                }
            }
            for (uint32 index : indices)
            {
                if (index >= vertices.size())
                {
                    return false;
                }
            }
            return true;
        }
    };
} // namespace RVX
