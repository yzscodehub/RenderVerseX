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
 */

#pragma once

// Core Module
#include "Animation/Core/Types.h"
#include "Animation/Core/Keyframe.h"
#include "Animation/Core/TransformSample.h"
#include "Animation/Core/Interpolation.h"

// Data Module
#include "Animation/Data/Skeleton.h"
#include "Animation/Data/AnimationTrack.h"
#include "Animation/Data/AnimationClip.h"

namespace RVX::Animation
{

/**
 * @brief Animation system version information
 */
struct AnimationSystemInfo
{
    static constexpr int kMajorVersion = 1;
    static constexpr int kMinorVersion = 0;
    static constexpr int kPatchVersion = 0;
    static constexpr const char* kVersionString = "1.0.0";

    static const char* GetVersionString() { return kVersionString; }
    static constexpr int GetVersionNumber()
    {
        return kMajorVersion * 10000 + kMinorVersion * 100 + kPatchVersion;
    }
};

} // namespace RVX::Animation
