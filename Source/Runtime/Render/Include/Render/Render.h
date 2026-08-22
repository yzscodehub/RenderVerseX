#pragma once

/**
 * @file Render.h
 * @brief Thin public facade for the composed runtime renderer.
 *
 * Renderer features, RenderGraph, scene internals, and RHI-facing utilities
 * have explicit headers and targets.  Product code should enter the renderer
 * through RenderSubsystem and its immutable value contracts.
 */

#include "Render/RenderDiagnostics.h"
#include "Render/RenderRuntimeTypes.h"
#include "Render/RenderSubsystem.h"
