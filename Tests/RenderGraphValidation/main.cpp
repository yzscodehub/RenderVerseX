#include "Core/Core.h"
#include "RenderGraph/RenderGraph.h"
#include "TestFramework/TestRunner.h"

using namespace RVX;
using namespace RVX::Test;

// =============================================================================
// RenderGraph Validation Tests
// =============================================================================

bool Test_GraphCreation()
{
    RenderGraph graph;
    // Just test construction/destruction
    return true;
}

bool Test_ResourceCreation()
{
    RenderGraph graph;
    
    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    auto texture = graph.CreateTexture(texDesc);
    
    RHIBufferDesc bufDesc;
    bufDesc.size = 1024 * 1024;
    bufDesc.usage = RHIBufferUsage::Structured;
    auto buffer = graph.CreateBuffer(bufDesc);
    
    // Placeholders - will be properly tested when implementation is complete
    (void)texture;
    (void)buffer;
    return true;
}

struct BarrierPassData
{
    RGTextureHandle texture;
    RGBufferHandle buffer;
};

bool Test_BarrierCompilation()
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(256, 256, RHIFormat::RGBA8_UNORM);
    auto texture = graph.CreateTexture(texDesc);

    RHIBufferDesc bufDesc;
    bufDesc.size = 256 * 4;
    bufDesc.usage = RHIBufferUsage::Structured;
    auto buffer = graph.CreateBuffer(bufDesc);

    graph.AddPass<BarrierPassData>(
        "BarrierPass",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, BarrierPassData& data)
        {
            data.texture = builder.WriteMip(texture, 0);
            data.buffer = builder.Read(buffer.Range(0, 128));
        },
        [](const BarrierPassData&, RHICommandContext&)
        {
            // No-op
        });

    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    graph.SetExportState(buffer, RHIResourceState::CopySource);
    graph.Compile();

    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("RenderGraph Validation Tests");
    
    TestSuite suite;
    suite.AddTest("GraphCreation", Test_GraphCreation);
    suite.AddTest("ResourceCreation", Test_ResourceCreation);
    suite.AddTest("BarrierCompilation", Test_BarrierCompilation);
    
    auto results = suite.Run();
    suite.PrintResults(results);
    
    Log::Shutdown();
    
    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }
    
    return 0;
}
