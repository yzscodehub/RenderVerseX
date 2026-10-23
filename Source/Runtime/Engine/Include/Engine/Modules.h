#pragma once

/**
 * @file Modules.h
 * @brief Runtime-first module overview and Engine umbrella include.
 *
 * RenderVerseX separates Shipping-capable Runtime modules from source-facing
 * Developer libraries and developer Programs. Public include paths remain
 * logical (`Core/...`, `Render/...`, and so on) and never expose the physical
 * `Source/Runtime` or `Source/Developer` domain.
 *
 * Engine is the top-level Runtime composition. It owns the ECS World runtime,
 * render extraction/publication, Built-in Physics bridge, cooked Resource
 * manager, and explicitly linked Runtime features. Developer and Shipping
 * compositions construct the move-only Engine dependency bundle outside the
 * Engine module; Engine does not discover source importers or shader compilers
 * through fallback state.
 *
 * ResourceSubsystem owns one concrete ResourceManager. Loaders are selected
 * from a sealed catalog and execute `Supports -> Prepare -> Validate ->
 * Publish`; there is no process-wide ResourceManager singleton.
 *
 * RenderGraph is exposed as the narrow `RVX::RenderGraph` link target, but it
 * is registered as part of the Render Manifest module rather than claiming a
 * separate policy module.
 *
 * Audio and Scripting are currently partial Engine features. UI is an optional
 * backend-neutral packet producer. AI and Networking compile as experimental
 * modules but are not part of Engine composition. Editor remains deferred.
 * The authoritative status and evidence are recorded in
 * `Docs/engine-capabilities.json`.
 */

#include "Engine/Engine.h"
