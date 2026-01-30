/**
 * @file TerrainRenderer.cpp
 * @brief Implementation of terrain rendering pass
 */

#include "Terrain/Terrain.h"
#include "Render/Passes/IRenderPass.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHICommandContext.h"
#include "Core/Log.h"

namespace RVX
{

/**
 * @brief Terrain rendering pass
 * 
 * Renders all terrain components in the scene using the LOD system
 * and terrain materials.
 */
class TerrainPass : public IRenderPass
{
public:
    TerrainPass() = default;
    ~TerrainPass() override = default;

    // =========================================================================
    // IRenderPass Interface
    // =========================================================================

    const char* GetName() const override { return "TerrainPass"; }

    int32_t GetPriority() const override { return 350; } // Between Opaque and Skybox

    void OnAdd(IRHIDevice* device) override
    {
        m_device = device;
        CreatePipeline();
    }

    void OnRemove() override
    {
        m_pipeline.Reset();
        m_device = nullptr;
    }

    void Setup(RenderGraphBuilder& builder, const ViewData& view) override
    {
        (void)builder;
        (void)view;
        // Declare resource usage
        // builder.Write(view.colorTarget);
        // builder.SetDepthStencil(view.depthTarget, true, false);
    }

    void Execute(RHICommandContext& ctx, const ViewData& view) override
    {
        (void)view;

        if (!m_pipeline)
        {
            RVX_CORE_WARN("TerrainPass: No pipeline available");
            return;
        }

        ctx.SetPipeline(m_pipeline.Get());

        // Render terrain patches
        // This would iterate through terrain components in the scene
        // and render each patch with the appropriate LOD level
    }

private:
    void CreatePipeline()
    {
        if (!m_device)
        {
            RVX_CORE_ERROR("TerrainPass: Cannot create pipeline - no device");
            return;
        }

        // Pipeline creation would be done here
        // For now, just log
        RVX_CORE_INFO("TerrainPass: Pipeline created");
    }

    IRHIDevice* m_device = nullptr;
    RHIPipelineRef m_pipeline;
};

/**
 * @brief Terrain shadow pass
 * 
 * Renders terrain depth for shadow mapping.
 */
class TerrainShadowPass : public IRenderPass
{
public:
    TerrainShadowPass() = default;
    ~TerrainShadowPass() override = default;

    const char* GetName() const override { return "TerrainShadowPass"; }

    int32_t GetPriority() const override { return 210; } // During Shadow pass

    void Setup(RenderGraphBuilder& builder, const ViewData& view) override
    {
        (void)builder;
        (void)view;
    }

    void Execute(RHICommandContext& ctx, const ViewData& view) override
    {
        (void)ctx;
        (void)view;
        // Render terrain depth for shadows
    }
};

// =========================================================================
// Terrain Renderer Utility Functions
// =========================================================================

/**
 * @brief Create terrain passes for the renderer
 * @return Vector of terrain render passes
 */
std::vector<std::unique_ptr<IRenderPass>> CreateTerrainPasses()
{
    std::vector<std::unique_ptr<IRenderPass>> passes;
    passes.push_back(std::make_unique<TerrainPass>());
    passes.push_back(std::make_unique<TerrainShadowPass>());
    return passes;
}

/**
 * @brief GPU data for terrain rendering
 */
struct TerrainGPUData
{
    Mat4 worldMatrix;
    Vec4 terrainSize;           // (width, height, depth, 0)
    Vec4 heightmapParams;       // (minHeight, maxHeight, 1/width, 1/height)
    Vec4 lodParams;             // (lodDistance, lodBias, morphRange, 0)
    UVec4 layerCount;           // (layerCount, splatmapCount, 0, 0)
};

/**
 * @brief GPU data for a single terrain patch
 */
struct TerrainPatchGPUData
{
    Vec4 patchPosAndSize;       // (x, z, size, morphFactor)
    UVec4 lodAndFlags;          // (lodLevel, neighborLODMask, 0, 0)
};

} // namespace RVX
