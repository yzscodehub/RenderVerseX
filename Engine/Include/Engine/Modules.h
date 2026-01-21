#pragma once

/**
 * @file Modules.h
 * @brief Module overview and include helpers
 * 
 * RenderVerseX Engine Module Architecture
 * =======================================
 * 
 * After refactoring, the engine is organized into the following layers:
 * 
 * APPLICATION LAYER
 * -----------------
 * - Samples/     : Example applications
 * - Tests/       : Validation and unit tests
 * 
 * ENGINE LAYER
 * ------------
 * - Engine/      : Main engine coordinator, subsystem management
 * 
 * FEATURE MODULES
 * ---------------
 * - Runtime/     : Window, Input, Time, Camera subsystems
 * - World/       : Scene, Spatial, Picking (integrated)
 * - Render/      : RenderSubsystem, RenderService, RenderGraph integration
 * - Resource/    : ResourceManager, ResourceSubsystem, IResource, ResourceHandle
 * - Animation/   : Animation playback
 * 
 * HARDWARE ABSTRACTION
 * --------------------
 * - HAL/         : Hardware Abstraction Layer (Window, Input backends)
 * - RHI/         : Render Hardware Interface
 * - RHI_DX11/    : DirectX 11 backend
 * - RHI_DX12/    : DirectX 12 backend
 * - RHI_Vulkan/  : Vulkan backend
 * - RHI_Metal/   : Metal backend (macOS/iOS)
 * - RHI_OpenGL/  : OpenGL backend
 * 
 * FOUNDATION
 * ----------
 * - Core/        : Math, Event, Job, Log, Subsystem base classes
 * - ShaderCompiler/: HLSL compilation
 * 
 * SUPPORTING MODULES
 * ------------------
 * - Scene/       : Core scene module (used by World)
 * - Spatial/     : Core spatial module (used by World)
 * - Picking/     : Forwards to World/PickingService
 * 
 * SUBSYSTEM OVERVIEW
 * ==================
 * 
 * Engine Subsystems (EngineSubsystem):
 * - WindowSubsystem    : Window lifecycle (Runtime)
 * - InputSubsystem     : Input polling (Runtime)
 * - TimeSubsystem      : Frame timing (Runtime)
 * - RenderSubsystem    : Rendering coordination (Render)
 * - ResourceSubsystem  : Asset management (Resource)
 * 
 * World Subsystems (WorldSubsystem):
 * - SpatialSubsystem   : Spatial queries, raycasting (World)
 * 
 * USAGE
 * =====
 * 
 * @code
 * #include "Engine/Engine.h"
 * #include "Runtime/Runtime.h"
 * #include "World/World.h"
 * #include "Render/Render.h"
 * #include "Resource/Resource.h"
 * 
 * int main() {
 *     RVX::Engine engine;
 *     
 *     // Add subsystems
 *     engine.AddSubsystem<RVX::WindowSubsystem>();
 *     engine.AddSubsystem<RVX::InputSubsystem>();
 *     engine.AddSubsystem<RVX::TimeSubsystem>();
 *     engine.AddSubsystem<RVX::RenderSubsystem>();
 *     engine.AddSubsystem<RVX::ResourceSubsystem>();
 *     
 *     engine.Initialize();
 *     
 *     // Create world
 *     RVX::World world;
 *     world.Initialize();
 *     
 *     while (!engine.ShouldShutdown()) {
 *         engine.Tick();
 *         world.Tick(RVX::Time::DeltaTime());
 *     }
 *     
 *     world.Shutdown();
 *     engine.Shutdown();
 *     
 *     return 0;
 * }
 * @endcode
 */

// All module headers
#include "Engine/Engine.h"

// For convenience, include all runtime subsystems
// Users can include individual headers if preferred
// #include "Runtime/Runtime.h"
// #include "World/World.h"
// #include "Render/Render.h"
// #include "Resource/Resource.h"
