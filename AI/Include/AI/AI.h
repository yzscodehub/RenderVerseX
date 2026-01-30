/**
 * @file AI.h
 * @brief AI Module unified header
 * 
 * Provides AI capabilities including:
 * - Navigation mesh and pathfinding
 * - Behavior trees for AI decision making
 * - Perception systems (sight, hearing)
 */

#pragma once

// Core AI
#include "AI/AISubsystem.h"
#include "AI/AITypes.h"

// Navigation
#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavMeshBuilder.h"
#include "AI/Navigation/PathFinder.h"
#include "AI/Navigation/NavigationAgent.h"

// Behavior Tree
#include "AI/BehaviorTree/BehaviorTree.h"
#include "AI/BehaviorTree/BTNode.h"
#include "AI/BehaviorTree/BTComposite.h"
#include "AI/BehaviorTree/BTTask.h"
#include "AI/BehaviorTree/BTDecorator.h"
#include "AI/BehaviorTree/BTService.h"
#include "AI/BehaviorTree/Blackboard.h"

// Perception
#include "AI/Perception/AIPerception.h"
#include "AI/Perception/SightSense.h"
#include "AI/Perception/HearingSense.h"

namespace RVX::AI
{

/**
 * @brief AI system version info
 */
struct AISystemInfo
{
    static constexpr const char* kModuleName = "AI";
    static constexpr int kMajorVersion = 1;
    static constexpr int kMinorVersion = 0;

#ifdef RVX_AI_RECAST
    static constexpr bool kHasRecast = true;
#else
    static constexpr bool kHasRecast = false;
#endif
};

} // namespace RVX::AI
