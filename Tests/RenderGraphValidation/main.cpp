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
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("RenderGraph Validation Tests");
    
    TestSuite suite;
    suite.AddTest("GraphCreation", Test_GraphCreation);
    suite.AddTest("ResourceCreation", Test_ResourceCreation);
    
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
