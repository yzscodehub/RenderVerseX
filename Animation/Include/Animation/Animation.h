/**
 * @file Animation.h
 * @brief Unified Animation System Header
 * 
 * This is the main include file for the animation system.
 * 
 * Supports:
 * - Skeletal bone animation
 * - Node transform animation
 * - BlendShape/Morph target animation
 * - Property animation
 * - Visibility animation
 * - Animation blending (1D, 2D blend spaces)
 * - Animation state machine
 * - Inverse kinematics (Two-bone IK, FABRIK)
 * - Animation events and callbacks
 * - Root motion extraction
 * - Animation compression
 */

#pragma once

// ============================================================================
// Core Module - Data Types and Algorithms
// ============================================================================
#include "Animation/Core/Types.h"
#include "Animation/Core/Keyframe.h"
#include "Animation/Core/TransformSample.h"
#include "Animation/Core/Interpolation.h"
#include "Animation/Core/AnimationEvent.h"
#include "Animation/Core/AnimationCompression.h"

// ============================================================================
// Data Module - Animation Data Structures
// ============================================================================
#include "Animation/Data/Skeleton.h"
#include "Animation/Data/AnimationTrack.h"
#include "Animation/Data/AnimationClip.h"

// ============================================================================
// Runtime Module - Animation Playback
// ============================================================================
#include "Animation/Runtime/SkeletonPose.h"
#include "Animation/Runtime/AnimationEvaluator.h"
#include "Animation/Runtime/AnimationPlayer.h"
#include "Animation/Runtime/RootMotion.h"

// ============================================================================
// Blend Module - Animation Blending
// ============================================================================
#include "Animation/Blend/BlendNode.h"
#include "Animation/Blend/Blend1D.h"
#include "Animation/Blend/Blend2D.h"
#include "Animation/Blend/BlendTree.h"

// ============================================================================
// State Module - Animation State Machine
// ============================================================================
#include "Animation/State/AnimationState.h"
#include "Animation/State/StateTransition.h"
#include "Animation/State/AnimationStateMachine.h"

// ============================================================================
// IK Module - Inverse Kinematics
// ============================================================================
#include "Animation/IK/IKSolver.h"
#include "Animation/IK/TwoBoneIK.h"
#include "Animation/IK/FABRIK.h"

namespace RVX::Animation
{

/**
 * @brief Animation system version information
 */
struct AnimationSystemInfo
{
    static constexpr int kMajorVersion = 2;
    static constexpr int kMinorVersion = 1;
    static constexpr int kPatchVersion = 0;
    static constexpr const char* kVersionString = "2.1.0";

    static const char* GetVersionString() { return kVersionString; }
    static constexpr int GetVersionNumber()
    {
        return kMajorVersion * 10000 + kMinorVersion * 100 + kPatchVersion;
    }
};

/**
 * @brief Feature flags for the animation system
 */
struct AnimationFeatures
{
    static constexpr bool HasEventSystem = true;
    static constexpr bool HasRootMotion = true;
    static constexpr bool HasCompression = true;
    static constexpr bool HasBlendTree = true;
    static constexpr bool HasStateMachine = true;
    static constexpr bool HasIK = true;
};

} // namespace RVX::Animation
