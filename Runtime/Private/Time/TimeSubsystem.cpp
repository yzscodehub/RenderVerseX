/**
 * @file TimeSubsystem.cpp
 * @brief TimeSubsystem implementation
 */

#include "Runtime/Time/TimeSubsystem.h"
#include "Core/Log.h"

namespace RVX
{

void TimeSubsystem::Initialize()
{
    RVX_CORE_INFO("TimeSubsystem initializing...");
    
    // Time::Initialize() is called by Engine, but ensure it's ready
    // In case this subsystem is used standalone
    Time::Initialize();
    
    RVX_CORE_INFO("TimeSubsystem initialized");
}

void TimeSubsystem::Deinitialize()
{
    RVX_CORE_DEBUG("TimeSubsystem deinitializing...");
    RVX_CORE_INFO("TimeSubsystem deinitialized");
}

void TimeSubsystem::Tick(float deltaTime)
{
    (void)deltaTime;
    // Time::Update() is called by Engine before subsystems are ticked
    // Nothing to do here, but kept for future extensions
}

} // namespace RVX
