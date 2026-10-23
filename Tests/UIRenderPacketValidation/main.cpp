#include "RenderContracts/UIRenderPacket.h"

#include <gtest/gtest.h>

using namespace RVX;

namespace
{
    UIRenderPacket MakePacket()
    {
        UIRenderPacket packet;
        packet.targetWidth = 320;
        packet.targetHeight = 180;
        packet.generation = 1;
        packet.vertices = {
            {{0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        };
        packet.indices = {0, 1, 2};
        UIRenderDrawCommand command;
        command.clipRect = {0.0f, 0.0f, 320.0f, 180.0f};
        command.firstIndex = 0;
        command.indexCount = 3;
        packet.commands.push_back(command);
        return packet;
    }
}

TEST(UIRenderPacketValidation, CompleteBackendNeutralPacketIsValid)
{
    const UIRenderPacket packet = MakePacket();
    EXPECT_TRUE(packet.IsValid());
}

TEST(UIRenderPacketValidation, IndexAndDrawRangesFailClosed)
{
    UIRenderPacket packet = MakePacket();
    packet.indices.back() = 7;
    EXPECT_FALSE(packet.IsValid());

    packet = MakePacket();
    packet.commands.front().indexCount = 4;
    EXPECT_FALSE(packet.IsValid());
}

TEST(UIRenderPacketValidation, ExternalTextureRequiresExactGenerationHandle)
{
    UIRenderPacket packet = MakePacket();
    packet.commands.front().primitive = UIRenderPrimitive::Image;
    packet.commands.front().resourceKind = UIRenderResourceKind::Texture;
    EXPECT_FALSE(packet.IsValid());

    packet.commands.front().texture = {7u, 3u};
    EXPECT_TRUE(packet.IsValid());
}
