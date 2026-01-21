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

// Legacy components (deprecated, use RenderContext instead)
#include "Render/RenderService.h"
#include "Render/SwapChainManager.h"
