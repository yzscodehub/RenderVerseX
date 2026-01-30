#pragma once

/**
 * @file Render.h
 * @brief Render module - Unified header
 * 
 * The Render module provides:
 * - RenderSubsystem (main render coordinator)
 * - RenderContext (RHI device and swap chain management)
 * - FrameSynchronizer (multi-frame GPU synchronization)
 * - SceneRenderer (scene rendering and pass management)
 * - ViewData/RenderScene (render data structures)
 * - IRenderPass (render pass interface)
 * - Integration with RenderGraph
 * - Post-processing effects
 * - Decal rendering
 * - Atmospheric scattering
 */

// Context - RHI resource management
#include "Render/Context/RenderContext.h"
#include "Render/Context/FrameSynchronizer.h"

// Renderer - Scene rendering
#include "Render/Renderer/SceneRenderer.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"

// Passes - Render pass interface and built-in passes
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/OpaquePass.h"

// Core render components
#include "Render/RenderSubsystem.h"

// RenderGraph (now part of Render module)
#include "Render/Graph/RenderGraph.h"

// Post-processing
#include "Render/PostProcess/PostProcessStack.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Render/PostProcess/FXAA.h"
#include "Render/PostProcess/SSAO.h"
#include "Render/PostProcess/SSR.h"
#include "Render/PostProcess/TAA.h"
#include "Render/PostProcess/DOF.h"
#include "Render/PostProcess/MotionBlur.h"
#include "Render/PostProcess/ColorGrading.h"
#include "Render/PostProcess/Vignette.h"
#include "Render/PostProcess/ChromaticAberration.h"
#include "Render/PostProcess/FilmGrain.h"
#include "Render/PostProcess/VolumetricLighting.h"

// Lighting
#include "Render/Lighting/LightManager.h"
#include "Render/Lighting/ClusteredLighting.h"

// Decal
#include "Render/Decal/DecalRenderer.h"

// Sky
#include "Render/Sky/AtmosphericScattering.h"

// Legacy components (deprecated, use RenderContext instead)
#include "Render/RenderService.h"
#include "Render/SwapChainManager.h"
