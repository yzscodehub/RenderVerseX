# Material System Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete RenderVerseX's material system into a stable, glTF-oriented PBR pipeline with correct opaque/masked/transparent routing, normal mapping, image-based lighting, shader-layout validation, and cross-RHI runtime coverage.

**Architecture:** Keep the long-term split descriptor model already introduced: set 0 frame, set 1 object, set 2 material. Add an explicit material draw classification layer between `RenderScene` and render passes so pass selection is data-driven instead of inferred inside each pass. Expand `PipelineCache` from one opaque pipeline to a small set of material pipeline variants while `MaterialSystem` remains the owner of material constants, fallback textures, and descriptor sets.

**Tech Stack:** C++20, CMake, Render module, Scene/Resource material data, HLSL, RHI descriptor abstraction, DX11/DX12/Vulkan backend validation, standalone validation executables, ModelViewer visual smoke tests.

---

## Current Baseline

The current material system already supports:

- Stable descriptor sets:
  - set 0: frame/view constants
  - set 1: object constants with dynamic offset
  - set 2: material constants, five texture slots, sampler
- `MaterialGPUConstants` for base color, metallic, roughness, normal scale, occlusion, emissive, alpha mode, alpha cutoff, workflow, double-sided.
- `MaterialSystem` descriptor ownership with fallback textures.
- `DefaultLit.hlsl` sampling base color, metallic-roughness, occlusion, emissive.
- `OpaquePass`, `DepthPrepass`, and `TransparentPass` source paths updated to bind split descriptor sets.

The current gaps are:

- `TransparentPass` is not in the default pass chain.
- `TransparentPass` still uses the opaque pipeline.
- Opaque pass does not filter out transparent submeshes.
- Normal maps are bound but not sampled through tangent space.
- PBR lighting is a lightweight direct-light approximation, not full Cook-Torrance plus IBL.
- Shader/RHI layout validation is source-string based, not reflection/runtime based.
- No automated visual scenario covers alpha mask, alpha blend, normal map, or emissive behavior.

---

## Target File Structure

### New Files

- `Render/Include/Render/Material/MaterialClassification.h`
  - Defines `MaterialRenderMode`, `MaterialPipelineVariant`, and helpers for classifying material resources.
- `Render/Private/Material/MaterialClassification.cpp`
  - Implements classification from `Resource::MaterialResource` / `Scene::Material`.
- `Render/Include/Render/Renderer/RenderDrawItem.h`
  - Defines per-submesh draw items used by opaque, masked, and transparent passes.
- `Render/Private/Renderer/RenderDrawItem.cpp`
  - Implements sort-key and distance helpers.
- `Tests/MaterialSystemValidation/main.cpp`
  - Standalone source/runtime validation for material classification, draw-list routing, and shader layout.

### Modified Files

- `Render/Include/Render/PipelineCache.h`
  - Add pipeline accessors for opaque, masked, and transparent material variants.
- `Render/Private/PipelineCache.cpp`
  - Create opaque, masked, transparent pipelines with correct blend/depth states.
- `Render/Include/Render/Renderer/SceneRenderer.h`
  - Add draw lists and cached `TransparentPass*`.
- `Render/Private/Renderer/SceneRenderer.cpp`
  - Build draw lists after culling, bind transparent pass resources, register transparent pass.
- `Render/Private/Renderer/RenderFrameResourceBinder.h`
  - Accept opaque/masked/transparent draw lists and transparent pass.
- `Render/Private/Renderer/RenderFrameResourceBinder.cpp`
  - Inject draw lists and render targets into passes.
- `Render/Include/Render/Passes/OpaquePass.h`
  - Consume draw items instead of raw visible object indices.
- `Render/Private/Passes/OpaquePass.cpp`
  - Draw opaque and masked items only.
- `Render/Include/Render/Passes/TransparentPass.h`
  - Consume transparent draw items.
- `Render/Private/Passes/TransparentPass.cpp`
  - Use transparent pipeline, alpha blending, depth read-only, back-to-front sort.
- `Render/Private/Passes/DepthPrepass.cpp`
  - Include opaque and masked items; exclude blend items.
- `Render/Shaders/DefaultLit.hlsl`
  - Add alpha mask discard, tangent-space normal mapping, Cook-Torrance BRDF, and IBL hooks.
- `Render/Shaders/Include/BRDF.hlsli`
  - Reuse or align BRDF helper functions with `DefaultLit.hlsl`.
- `Render/CMakeLists.txt`
  - Add new source files and validation target.

---

## Phase 1: Correct Material Routing and Transparent Pipeline

### Task 1: Add Material Classification

**Files:**
- Create: `Render/Include/Render/Material/MaterialClassification.h`
- Create: `Render/Private/Material/MaterialClassification.cpp`
- Test: `Tests/MaterialSystemValidation/main.cpp`

- [x] **Step 1: Write the failing material classification test**

Add a test that constructs three `Scene::Material` instances and verifies classification:

```cpp
bool Test_MaterialClassificationMatchesAlphaMode()
{
    auto opaque = std::make_shared<RVX::Material>();
    opaque->SetAlphaMode(RVX::Material::AlphaMode::Opaque);
    TEST_ASSERT_EQ(RVX::MaterialRenderMode::Opaque,
                   RVX::ClassifyMaterialRenderMode(opaque.get()));

    auto masked = std::make_shared<RVX::Material>();
    masked->SetAlphaMode(RVX::Material::AlphaMode::Mask);
    TEST_ASSERT_EQ(RVX::MaterialRenderMode::Masked,
                   RVX::ClassifyMaterialRenderMode(masked.get()));

    auto transparent = std::make_shared<RVX::Material>();
    transparent->SetAlphaMode(RVX::Material::AlphaMode::Blend);
    TEST_ASSERT_EQ(RVX::MaterialRenderMode::Transparent,
                   RVX::ClassifyMaterialRenderMode(transparent.get()));

    return true;
}
```

- [x] **Step 2: Run the test and verify RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target MaterialSystemValidation
build\Debug\Tests\Debug\MaterialSystemValidation.exe
```

Expected: compile or test failure because `MaterialClassification.h` and `ClassifyMaterialRenderMode` do not exist yet.

- [x] **Step 3: Implement material classification**

Create `Render/Include/Render/Material/MaterialClassification.h`:

```cpp
#pragma once

/**
 * @file MaterialClassification.h
 * @brief Helpers for routing materials to render passes and pipeline variants
 */

#include "Core/Types.h"

namespace RVX
{
    class Material;

    enum class MaterialRenderMode : uint8
    {
        Opaque = 0,
        Masked,
        Transparent
    };

    enum class MaterialPipelineVariant : uint8
    {
        Opaque = 0,
        Masked,
        Transparent
    };

    MaterialRenderMode ClassifyMaterialRenderMode(const Material* material);
    MaterialPipelineVariant GetPipelineVariantForRenderMode(MaterialRenderMode mode);

} // namespace RVX
```

Create `Render/Private/Material/MaterialClassification.cpp`:

```cpp
#include "Render/Material/MaterialClassification.h"
#include "Scene/Material.h"

namespace RVX
{
    MaterialRenderMode ClassifyMaterialRenderMode(const Material* material)
    {
        if (!material)
            return MaterialRenderMode::Opaque;

        switch (material->GetAlphaMode())
        {
            case Material::AlphaMode::Mask:
                return MaterialRenderMode::Masked;
            case Material::AlphaMode::Blend:
                return MaterialRenderMode::Transparent;
            case Material::AlphaMode::Opaque:
            default:
                return MaterialRenderMode::Opaque;
        }
    }

    MaterialPipelineVariant GetPipelineVariantForRenderMode(MaterialRenderMode mode)
    {
        switch (mode)
        {
            case MaterialRenderMode::Masked:
                return MaterialPipelineVariant::Masked;
            case MaterialRenderMode::Transparent:
                return MaterialPipelineVariant::Transparent;
            case MaterialRenderMode::Opaque:
            default:
                return MaterialPipelineVariant::Opaque;
        }
    }

} // namespace RVX
```

- [x] **Step 4: Add the files to `Render/CMakeLists.txt` and test target**

Add `Private/Material/MaterialClassification.cpp` to the Render library sources and add `MaterialSystemValidation` to the tests CMake section following the existing standalone validation pattern.

- [x] **Step 5: Run the test and verify GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target MaterialSystemValidation
build\Debug\Tests\Debug\MaterialSystemValidation.exe
```

Expected: `MaterialClassificationMatchesAlphaMode` passes.

### Task 2: Build Per-Submesh Draw Lists

**Files:**
- Create: `Render/Include/Render/Renderer/RenderDrawItem.h`
- Create: `Render/Private/Renderer/RenderDrawItem.cpp`
- Modify: `Render/Include/Render/Renderer/SceneRenderer.h`
- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Test: `Tests/MaterialSystemValidation/main.cpp`

- [x] **Step 1: Write the failing draw-list routing test**

Add a source validation test that requires `SceneRenderer` to own three draw lists:

```cpp
bool Test_SceneRendererOwnsMaterialDrawLists()
{
    const std::filesystem::path headerPath =
        std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Include" / "Render" / "Renderer" / "SceneRenderer.h";
    const std::filesystem::path sourcePath =
        std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp";

    const std::string header = LoadSourceFile(headerPath);
    const std::string source = LoadSourceFile(sourcePath);

    TEST_ASSERT_TRUE(header.find("std::vector<RenderDrawItem> m_opaqueDrawItems") != std::string::npos);
    TEST_ASSERT_TRUE(header.find("std::vector<RenderDrawItem> m_maskedDrawItems") != std::string::npos);
    TEST_ASSERT_TRUE(header.find("std::vector<RenderDrawItem> m_transparentDrawItems") != std::string::npos);
    TEST_ASSERT_TRUE(source.find("BuildMaterialDrawLists") != std::string::npos);
    TEST_ASSERT_TRUE(source.find("ClassifyMaterialRenderMode") != std::string::npos);

    return true;
}
```

- [x] **Step 2: Run the test and verify RED**

Run `MaterialSystemValidation.exe`.

Expected: fails because the draw lists and builder do not exist yet.

- [x] **Step 3: Implement `RenderDrawItem`**

Create `Render/Include/Render/Renderer/RenderDrawItem.h`:

```cpp
#pragma once

/**
 * @file RenderDrawItem.h
 * @brief Per-submesh draw item used by material-aware passes
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Material/MaterialClassification.h"

namespace RVX
{
    namespace Resource
    {
        class MaterialResource;
    } // namespace Resource

    struct RenderDrawItem
    {
        uint32 objectIndex = 0;
        uint32 submeshIndex = 0;
        uint64 meshId = 0;
        uint64 materialId = 0;
        Resource::MaterialResource* materialResource = nullptr;
        MaterialRenderMode renderMode = MaterialRenderMode::Opaque;
        float depthFromCamera = 0.0f;
        uint64 sortKey = 0;
    };

    uint64 BuildOpaqueDrawSortKey(const RenderDrawItem& item);
    uint64 BuildTransparentDrawSortKey(const RenderDrawItem& item);

} // namespace RVX
```

- [x] **Step 4: Build draw lists in `SceneRenderer`**

Add members:

```cpp
std::vector<RenderDrawItem> m_opaqueDrawItems;
std::vector<RenderDrawItem> m_maskedDrawItems;
std::vector<RenderDrawItem> m_transparentDrawItems;
```

Add private method:

```cpp
void BuildMaterialDrawLists();
```

Implementation outline:

```cpp
void SceneRenderer::BuildMaterialDrawLists()
{
    m_opaqueDrawItems.clear();
    m_maskedDrawItems.clear();
    m_transparentDrawItems.clear();

    for (uint32 objectIndex : m_visibleObjectIndices)
    {
        const RenderObject& obj = m_renderScene.GetObject(objectIndex);
        const size_t submeshCount = obj.materialResources.empty() ? 1 : obj.materialResources.size();

        for (size_t submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex)
        {
            Resource::MaterialResource* materialResource =
                submeshIndex < obj.materialResources.size() ? obj.materialResources[submeshIndex] : nullptr;
            const Material* material = materialResource ? materialResource->GetMaterialData().get() : nullptr;
            const MaterialRenderMode mode = ClassifyMaterialRenderMode(material);

            RenderDrawItem item;
            item.objectIndex = objectIndex;
            item.submeshIndex = static_cast<uint32>(submeshIndex);
            item.meshId = obj.meshId;
            item.materialId = submeshIndex < obj.materialIds.size() ? obj.materialIds[submeshIndex] : 0;
            item.materialResource = materialResource;
            item.renderMode = mode;
            item.depthFromCamera = length(Vec3(obj.worldMatrix[3]) - m_viewData.cameraPosition);

            if (mode == MaterialRenderMode::Transparent)
                m_transparentDrawItems.push_back(item);
            else if (mode == MaterialRenderMode::Masked)
                m_maskedDrawItems.push_back(item);
            else
                m_opaqueDrawItems.push_back(item);
        }
    }

    std::sort(m_opaqueDrawItems.begin(), m_opaqueDrawItems.end(),
              [](const RenderDrawItem& a, const RenderDrawItem& b) { return a.sortKey < b.sortKey; });
    std::sort(m_maskedDrawItems.begin(), m_maskedDrawItems.end(),
              [](const RenderDrawItem& a, const RenderDrawItem& b) { return a.sortKey < b.sortKey; });
    std::sort(m_transparentDrawItems.begin(), m_transparentDrawItems.end(),
              [](const RenderDrawItem& a, const RenderDrawItem& b) { return a.depthFromCamera > b.depthFromCamera; });
}
```

Call `BuildMaterialDrawLists()` after frustum culling and before `UpdatePassResources()`.

- [x] **Step 5: Run tests and ModelViewer smoke**

Run:

```powershell
cmake --build build\Debug --config Debug --target MaterialSystemValidation
build\Debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

Expected: validation passes and ModelViewer still renders the helmet.

### Task 3: Add Opaque, Masked, and Transparent Pipeline Variants

**Files:**
- Modify: `Render/Include/Render/PipelineCache.h`
- Modify: `Render/Private/PipelineCache.cpp`
- Test: `Tests/MaterialSystemValidation/main.cpp`

- [x] **Step 1: Write the failing pipeline variant test**

Add source validation:

```cpp
bool Test_PipelineCacheCreatesMaterialPipelineVariants()
{
    const std::filesystem::path headerPath =
        std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Include" / "Render" / "PipelineCache.h";
    const std::filesystem::path sourcePath =
        std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "PipelineCache.cpp";

    const std::string header = LoadSourceFile(headerPath);
    const std::string source = LoadSourceFile(sourcePath);

    TEST_ASSERT_TRUE(header.find("GetMaskedPipeline") != std::string::npos);
    TEST_ASSERT_TRUE(header.find("GetTransparentPipeline") != std::string::npos);
    TEST_ASSERT_TRUE(source.find("DefaultMaskedPipeline") != std::string::npos);
    TEST_ASSERT_TRUE(source.find("DefaultTransparentPipeline") != std::string::npos);
    TEST_ASSERT_TRUE(source.find("RHIDepthStencilState::ReadOnly()") != std::string::npos);
    TEST_ASSERT_TRUE(source.find("RHIRenderTargetBlendState::AlphaBlend()") != std::string::npos);

    return true;
}
```

- [x] **Step 2: Run and verify RED**

Run `MaterialSystemValidation.exe`.

Expected: fails because the pipelines do not exist.

- [x] **Step 3: Add pipeline members and accessors**

In `PipelineCache.h`:

```cpp
RHIPipeline* GetMaskedPipeline() const { return m_maskedPipeline.Get(); }
RHIPipeline* GetTransparentPipeline() const { return m_transparentPipeline.Get(); }
RHIPipeline* GetPipelineForVariant(MaterialPipelineVariant variant) const;

RHIPipelineRef m_maskedPipeline;
RHIPipelineRef m_transparentPipeline;
```

- [x] **Step 4: Create pipeline variants**

Refactor `CreatePipeline()` into a helper:

```cpp
RHIPipelineRef PipelineCache::CreateDefaultLitPipeline(const char* debugName,
                                                       const RHIDepthStencilState& depthState,
                                                       const RHIBlendState& blendState)
{
    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.debugName = debugName;
    pipelineDesc.vertexShader = m_vertexShader.Get();
    pipelineDesc.pixelShader = m_pixelShader.Get();
    pipelineDesc.layout = m_pipelineLayout.Get();
    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("NORMAL", RHIFormat::RGB32_FLOAT, 1);
    pipelineDesc.inputLayout.AddElement("TEXCOORD", RHIFormat::RG32_FLOAT, 2);
    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.frontFace = RHIFrontFace::Clockwise;
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;
    pipelineDesc.depthStencilState = depthState;
    pipelineDesc.blendState = blendState;
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = m_renderTargetFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::D24_UNORM_S8_UINT;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    return m_device->CreateGraphicsPipeline(pipelineDesc);
}
```

Then:

```cpp
m_opaquePipeline = CreateDefaultLitPipeline("DefaultOpaquePipeline",
                                            RHIDepthStencilState::Default(),
                                            RHIBlendState::Default());

m_maskedPipeline = CreateDefaultLitPipeline("DefaultMaskedPipeline",
                                            RHIDepthStencilState::Default(),
                                            RHIBlendState::Default());

RHIBlendState transparentBlend = RHIBlendState::Default();
transparentBlend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();
m_transparentPipeline = CreateDefaultLitPipeline("DefaultTransparentPipeline",
                                                RHIDepthStencilState::ReadOnly(),
                                                transparentBlend);
```

- [x] **Step 5: Run tests and backend source validations**

Run:

```powershell
cmake --build build\Debug --config Debug --target MaterialSystemValidation
build\Debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build\Debug --config Debug --target RHIDX12SourceValidation
build\Debug\Tests\Debug\RHIDX12SourceValidation.exe
```

Expected: all pass.

### Task 4: Wire Transparent Pass into Default Renderer

**Files:**
- Modify: `Render/Include/Render/Renderer/SceneRenderer.h`
- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Modify: `Render/Private/Renderer/RenderFrameResourceBinder.h`
- Modify: `Render/Private/Renderer/RenderFrameResourceBinder.cpp`
- Modify: `Render/Include/Render/Passes/OpaquePass.h`
- Modify: `Render/Private/Passes/OpaquePass.cpp`
- Modify: `Render/Include/Render/Passes/TransparentPass.h`
- Modify: `Render/Private/Passes/TransparentPass.cpp`
- Test: `Tests/MaterialSystemValidation/main.cpp`

- [x] **Step 1: Write failing source validation**

Require default pass setup and binder to include transparent pass:

```cpp
bool Test_DefaultRendererWiresTransparentPass()
{
    const auto sceneRendererSource =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const auto binderHeader =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Renderer" / "RenderFrameResourceBinder.h");
    const auto binderSource =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Renderer" / "RenderFrameResourceBinder.cpp");
    const auto transparentSource =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "TransparentPass.cpp");

    TEST_ASSERT_TRUE(sceneRendererSource.find("std::make_unique<TransparentPass>()") != std::string::npos);
    TEST_ASSERT_TRUE(sceneRendererSource.find("m_transparentPass = transparentPass.get()") != std::string::npos);
    TEST_ASSERT_TRUE(binderHeader.find("TransparentPass* transparentPass") != std::string::npos);
    TEST_ASSERT_TRUE(binderSource.find("transparentPass->SetRenderScene") != std::string::npos);
    TEST_ASSERT_TRUE(transparentSource.find("GetTransparentPipeline()") != std::string::npos);

    return true;
}
```

- [x] **Step 2: Run and verify RED**

Run `MaterialSystemValidation.exe`.

Expected: fails because transparent pass is not wired by default.

- [x] **Step 3: Add transparent pass registration**

In `SceneRenderer::SetupDefaultPasses()` add transparent after skybox or after opaque depending skybox depth behavior. Preferred first implementation:

```cpp
auto transparentPass = std::make_unique<TransparentPass>();
transparentPass->SetResources(m_gpuResourceManager.get(), m_pipelineCache.get(), m_materialSystem.get());
m_transparentPass = transparentPass.get();
AddPass(std::move(transparentPass));
```

Use pass ordering constants so transparent runs after opaque and before late postprocess.

- [x] **Step 4: Update binder and pass draw-item inputs**

`RenderFrameResourceBinder::BindScenePassResources` should accept:

```cpp
const std::vector<RenderDrawItem>& opaqueDrawItems,
const std::vector<RenderDrawItem>& maskedDrawItems,
const std::vector<RenderDrawItem>& transparentDrawItems,
TransparentPass* transparentPass
```

`OpaquePass` should receive opaque and masked draw lists. `TransparentPass` should receive transparent draw list.

- [x] **Step 5: Change `TransparentPass` to use the transparent pipeline**

Replace:

```cpp
RHIPipeline* pipeline = m_pipelineCache->GetOpaquePipeline();
```

with:

```cpp
RHIPipeline* pipeline = m_pipelineCache->GetTransparentPipeline();
```

- [x] **Step 6: Run tests and ModelViewer smoke**

Run:

```powershell
cmake --build build\Debug --config Debug --target MaterialSystemValidation
build\Debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

Expected: tests pass and ModelViewer still renders the helmet once, without duplicate draw artifacts.

---

## Phase 2: Shader Material Correctness

### Task 5: Enable Tangent-Space Normal Mapping

**Files:**
- Modify: `Render/Shaders/DefaultLit.hlsl`
- Modify: `Render/Private/GPUResourceManager.cpp`
- Modify: `Render/Include/Render/GPUResourceManager.h`
- Test: `Tests/MaterialSystemValidation/main.cpp`

- [x] **Step 1: Write failing validation for tangent support**

Add source validation requiring tangent buffer binding and shader tangent input:

```cpp
bool Test_NormalMappingUsesTangentSpace()
{
    const auto shader =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Shaders" / "DefaultLit.hlsl");
    const auto opaquePass =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "OpaquePass.cpp");

    TEST_ASSERT_TRUE(shader.find("float4 Tangent") != std::string::npos);
    TEST_ASSERT_TRUE(shader.find("SampleNormalMap") != std::string::npos);
    TEST_ASSERT_TRUE(shader.find("NormalTexture.Sample") != std::string::npos);
    TEST_ASSERT_TRUE(opaquePass.find("ctx.SetVertexBuffer(3, buffers.tangentBuffer)") != std::string::npos);

    return true;
}
```

- [x] **Step 2: Run and verify RED**

Run `MaterialSystemValidation.exe`.

Expected: fails because tangent buffer and tangent shader input are not fully wired.

- [x] **Step 3: Extend `MeshGPUBuffers`**

Add:

```cpp
RHIBuffer* tangentBuffer = nullptr;
```

When uploading mesh data, upload the `VertexBufferNames::Tangent` attribute if present.

- [x] **Step 4: Bind tangent buffer in passes**

In opaque and transparent pass:

```cpp
if (buffers.tangentBuffer)
{
    ctx.SetVertexBuffer(3, buffers.tangentBuffer);
}
```

- [x] **Step 5: Update input layout and HLSL**

Add input layout element:

```cpp
pipelineDesc.inputLayout.AddElement("TANGENT", RHIFormat::RGBA32_FLOAT, 3);
```

Add tangent-space shader path:

```hlsl
float3 SampleNormalMap(float2 uv, float3 worldNormal, float4 worldTangent)
{
    float3 tangentNormal = NormalTexture.Sample(MaterialSampler, uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= NormalScale;

    float3 N = normalize(worldNormal);
    float3 T = normalize(worldTangent.xyz);
    float3 B = normalize(cross(N, T) * worldTangent.w);
    float3x3 tbn = float3x3(T, B, N);
    return normalize(mul(tangentNormal, tbn));
}
```

- [x] **Step 6: Run shader compile and ModelViewer**

Run:

```powershell
cmake --build build\Debug --config Debug --target ModelViewer
build\Debug\Tests\Debug\MaterialSystemValidation.exe
```

Expected: build passes and DamagedHelmet normal map visibly affects lighting.

### Task 6: Upgrade Lighting to Cook-Torrance BRDF

**Files:**
- Modify: `Render/Shaders/DefaultLit.hlsl`
- Modify: `Render/Shaders/Include/BRDF.hlsli`
- Test: `Tests/MaterialSystemValidation/main.cpp`

- [x] **Step 1: Write failing shader validation**

Require BRDF include and core functions:

```cpp
bool Test_DefaultLitUsesCookTorranceBRDF()
{
    const auto shader =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Shaders" / "DefaultLit.hlsl");

    TEST_ASSERT_TRUE(shader.find("#include \"Include/BRDF.hlsli\"") != std::string::npos);
    TEST_ASSERT_TRUE(shader.find("EvaluatePBR") != std::string::npos);
    TEST_ASSERT_TRUE(shader.find("F_Schlick") != std::string::npos);

    return true;
}
```

- [x] **Step 2: Run and verify RED**

Run `MaterialSystemValidation.exe`.

Expected: fails until shader uses shared BRDF helpers.

- [x] **Step 3: Refactor `DefaultLit.hlsl`**

Replace ad hoc diffuse/specular lighting with:

```hlsl
float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor.rgb, metallic);
float3 Lo = EvaluatePBR(baseColor.rgb, metallic, roughness, N, V, L, LightColor, F0);
float3 ambient = baseColor.rgb * 0.03 * ao;
float3 color = ambient + Lo + emissive;
return float4(color, baseColor.a);
```

- [x] **Step 4: Run DXC compile through ModelViewer build**

Run:

```powershell
cmake --build build\Debug --config Debug --target ModelViewer
```

Expected: shader compiles and ModelViewer renders.

---

## Phase 3: IBL and Global Material Resources

### Task 7: Add Global Lighting Resources to Set 0

**Files:**
- Modify: `Render/Include/Render/PipelineCache.h`
- Modify: `Render/Private/PipelineCache.cpp`
- Modify: `Render/Shaders/DefaultLit.hlsl`
- Modify: `Scene/Include/Scene/Components/SkyboxComponent.h`
- Test: `Tests/MaterialSystemValidation/main.cpp`

- [ ] **Step 1: Write failing descriptor layout validation**

Require set 0 bindings for irradiance, prefiltered environment, and BRDF LUT:

```cpp
bool Test_FrameSetDeclaresIBLResources()
{
    const auto pipeline =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "PipelineCache.cpp");
    const auto shader =
        LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Shaders" / "DefaultLit.hlsl");

    TEST_ASSERT_TRUE(pipeline.find("IrradianceMap") != std::string::npos);
    TEST_ASSERT_TRUE(pipeline.find("PrefilteredEnvironmentMap") != std::string::npos);
    TEST_ASSERT_TRUE(pipeline.find("BRDFLUT") != std::string::npos);
    TEST_ASSERT_TRUE(shader.find("TextureCube IrradianceMap") != std::string::npos);
    TEST_ASSERT_TRUE(shader.find("TextureCube PrefilteredEnvironmentMap") != std::string::npos);
    TEST_ASSERT_TRUE(shader.find("Texture2D BRDFLUT") != std::string::npos);

    return true;
}
```

- [ ] **Step 2: Run and verify RED**

Run `MaterialSystemValidation.exe`.

Expected: fails until set 0 global IBL resources exist.

- [ ] **Step 3: Extend frame descriptor set**

Add frame set bindings:

- `b0`: view constants
- `t1`: irradiance cubemap
- `t2`: prefiltered environment cubemap
- `t3`: BRDF LUT
- `s4`: global linear sampler

Use fallback black cubemap / white BRDF LUT when no skybox component provides IBL.

- [ ] **Step 4: Add shader IBL evaluation**

Add:

```hlsl
float3 diffuseIBL = IrradianceMap.Sample(GlobalSampler, N).rgb * baseColor.rgb * (1.0 - metallic);
float2 brdf = BRDFLUT.Sample(GlobalSampler, float2(saturate(dot(N, V)), roughness)).rg;
float3 reflection = PrefilteredEnvironmentMap.SampleLevel(GlobalSampler, R, roughness * MaxReflectionMip).rgb;
float3 specularIBL = reflection * (F * brdf.x + brdf.y);
```

- [ ] **Step 5: Run ModelViewer with and without skybox**

Run ModelViewer twice: once with default environment and once with no skybox resources.

Expected: both render; missing IBL uses fallbacks without descriptor errors.

---

## Phase 4: Validation and Cross-RHI Confidence

### Task 8: Add Shader Reflection/Layout Validation

**Files:**
- Create: `Tests/ShaderLayoutValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`
- Test: `Tests/ShaderLayoutValidation/main.cpp`

- [ ] **Step 1: Write test that compiles `DefaultLit.hlsl` to SPIR-V and checks bindings**

Use local DXC from the build/toolchain and parse `-Fc` disassembly. Required decorations:

- `ViewConstants`: set 0 binding 0
- `ObjectConstants`: set 1 binding 0
- `MaterialConstants`: set 2 binding 0
- `BaseColorTexture`: set 2 binding 1
- `NormalTexture`: set 2 binding 2
- `MetallicRoughnessTexture`: set 2 binding 3
- `OcclusionTexture`: set 2 binding 4
- `EmissiveTexture`: set 2 binding 5
- `MaterialSampler`: set 2 binding 6

- [ ] **Step 2: Run and verify RED if any binding drifts**

Run:

```powershell
cmake --build build\Debug --config Debug --target ShaderLayoutValidation
build\Debug\Tests\Debug\ShaderLayoutValidation.exe
```

Expected: passes after current split layout; fails if any shader binding regresses.

### Task 9: Add Material Visual Smoke Scenes

**Files:**
- Modify: `Samples/ModelViewer/main.cpp`
- Add assets under existing sample asset path if there is already a generated/sample asset convention.
- Test: manual screenshot plus log scan.

- [ ] **Step 1: Add selectable material stress scene**

Add a debug scene option with four primitives:

- opaque metallic-roughness
- alpha masked fence/leaves quad
- alpha blended glass quad
- normal-mapped surface

- [ ] **Step 2: Run ModelViewer and capture screenshots**

Run:

```powershell
cmake --build build\Debug --config Debug --target ModelViewer
```

Capture:

- `modelviewer-material-opaque.png`
- `modelviewer-material-masked.png`
- `modelviewer-material-transparent.png`
- `modelviewer-material-normal.png`

Expected: all four materials render visibly and latest `RenderVerseX.log` tail has no `error`, `critical`, or assertion.

### Task 10: Cross-RHI Runtime Validation

**Files:**
- Modify: `Tests/DX11Validation/main.cpp`
- Modify: `Tests/DX12Validation/main.cpp`
- Modify: `Tests/VulkanValidation/main.cpp`

- [ ] **Step 1: Add descriptor set count / dynamic offset validation hooks**

Add backend tests that create the default material pipeline layout and bind three descriptor sets in order.

- [ ] **Step 2: Run backend validations**

Run:

```powershell
cmake --build build\Debug --config Debug --target DX11Validation
cmake --build build\Debug --config Debug --target DX12Validation
cmake --build build\Debug --config Debug --target VulkanValidation
build\Debug\Tests\Debug\DX11Validation.exe
build\Debug\Tests\Debug\DX12Validation.exe
build\Debug\Tests\Debug\VulkanValidation.exe
```

Expected: all validation executables return exit code 0. Vulkan validation-layer warnings should be tracked separately if they are unrelated to material binding.

---

## Phase 5: Performance and Maintainability

### Task 11: Reduce Per-Submesh Descriptor Work

**Files:**
- Modify: `Render/Private/Material/MaterialSystem.cpp`
- Modify: `Render/Include/Render/Material/MaterialSystem.h`
- Modify: `Render/Private/Passes/OpaquePass.cpp`
- Modify: `Render/Private/Passes/TransparentPass.cpp`

- [ ] **Step 1: Add material update cache key validation**

Require a cache key containing:

- material resource id
- texture view generation
- material revision or material pointer generation

- [ ] **Step 2: Cache constant updates per frame**

Add:

```cpp
struct MaterialFrameBinding
{
    RHIDescriptorSet* descriptorSet = nullptr;
    uint32 dynamicOffset = 0;
};

MaterialFrameBinding GetOrUpdateMaterialFrameBinding(const Resource::MaterialResource* materialResource,
                                                     ResourceViewCache* viewCache);
```

This avoids updating material constants repeatedly for the same material in one frame.

- [ ] **Step 3: Sort opaque draw items by material and mesh**

Set opaque sort key:

```cpp
item.sortKey = HashCombine(item.materialId, item.meshId);
```

Expected: fewer descriptor set switches in heavy scenes.

### Task 12: Material Debug Metrics

**Files:**
- Modify: `Render/Include/Render/Material/MaterialSystem.h`
- Modify: `Render/Private/Material/MaterialSystem.cpp`

- [ ] **Step 1: Add stats struct**

```cpp
struct MaterialSystemStats
{
    uint32 descriptorSetCount = 0;
    uint32 descriptorCacheHits = 0;
    uint32 descriptorCacheMisses = 0;
    uint32 materialConstantUpdates = 0;
    uint32 fallbackTextureBindings = 0;
};
```

- [ ] **Step 2: Expose `GetStats()` and reset per-frame counters in `BeginFrame()`**

Expected: future debug overlay can show material pressure without digging through logs.

---

## Execution Order

1. Phase 1 first. It fixes correctness: transparent pass, pass routing, and pipeline variants.
2. Phase 2 second. It improves visible material quality without changing pass architecture.
3. Phase 3 third. IBL needs stable global resources and should not be mixed with transparent routing.
4. Phase 4 after every major phase, with `ShaderLayoutValidation` added as soon as split bindings are stable.
5. Phase 5 last. Optimize only after correctness and backend coverage are established.

---

## Verification Checklist

Before claiming this plan complete:

- [ ] `MaterialSystemValidation` passes.
- [ ] `RenderPassBindingValidation` passes.
- [ ] `ShaderLayoutValidation` passes.
- [ ] `RHIDX12SourceValidation` passes.
- [ ] `DX11Validation`, `DX12Validation`, and `VulkanValidation` pass or have documented unrelated backend issues.
- [ ] `ModelViewer` renders DamagedHelmet.
- [ ] `ModelViewer` renders opaque, masked, transparent, and normal-map stress materials.
- [ ] Latest ModelViewer log tail contains no new material/RHI errors.
- [ ] A GPT-5.3-Codex-Spark review has been requested after each phase and actionable findings have been addressed or explicitly deferred with technical reasoning.

---

## Risk Notes

- Transparent pass must not be enabled until opaque and transparent draw lists are separated; otherwise transparent submeshes may draw twice.
- Masked materials should remain in depth-writing opaque-style passes because alpha test participates in depth.
- Transparent materials need depth read-only, alpha blend, and back-to-front sorting; this is not equivalent to simply rebinding the opaque pipeline.
- Normal mapping depends on tangent buffer upload and vertex input layout. Binding the normal map alone is not enough.
- IBL should use safe fallback resources so scenes without skybox probes still render.
- Shader register spaces must stay synchronized with RHI descriptor set layouts; source-string tests are useful but insufficient alone.
