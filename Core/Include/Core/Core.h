#pragma once

// =============================================================================
// Core Module - Unified Header
// =============================================================================

// Basic Types
#include "Core/Types.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Core/RefCounted.h"
#include "Core/Handle.h"

// Event System
#include "Core/Event/Event.h"
#include "Core/Event/EventHandle.h"
#include "Core/Event/EventBus.h"

// Service Locator
#include "Core/Services.h"

// Subsystem Framework
#include "Core/Subsystem/ISubsystem.h"
#include "Core/Subsystem/EngineSubsystem.h"
#include "Core/Subsystem/WorldSubsystem.h"
#include "Core/Subsystem/SubsystemCollection.h"

// Job System
#include "Core/Job/ThreadPool.h"
#include "Core/Job/JobSystem.h"
