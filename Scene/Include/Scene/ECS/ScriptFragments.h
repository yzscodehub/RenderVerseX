#pragma once

/**
 * @file ScriptFragments.h
 * @brief Data-only scripting configuration, intent, and status fragments.
 *
 * Lua VM objects, source paths, and runtime script handles intentionally stay
 * outside ECS storage. Scripting bridges resolve ScriptBehaviour::scriptAssetId
 * and retain their transient generation-safe identities in side tables.
 */

#include "ECS/Fragment.h"
#include "RenderContracts/RenderIdentity.h"

namespace RVX::SceneECS
{
    /** @brief Explicit lifecycle command consumed by a scripting bridge. */
    enum class ScriptExecutionCommand : uint8
    {
        None = 0,
        Start,
        Stop,
        Reload,
    };

    /** @brief Observable value-only result of an ECS script binding. */
    enum class ScriptBindingStatus : uint8
    {
        Unbound = 0,
        Loaded,
        Running,
        Paused,
        Stopped,
        PendingDestroy,
        InvalidConfiguration,
        ProgramRejected,
        InvocationFailed,
    };

    /**
     * @brief Authored script program identity and configuration revision.
     *
     * scriptAssetId is a stable logical identity, never a filesystem path,
     * source string, Lua object, or ScriptHandle.
     */
    struct ScriptBehaviour
    {
        AssetId scriptAssetId{};
        uint64 configurationRevision = 1;
        /** @brief False preserves the legacy component's explicit Start behavior. */
        bool startOnLoad = false;
    };

    /** @brief Monotonic, value-only script lifecycle command. */
    struct ScriptExecutionIntent
    {
        ScriptExecutionCommand command = ScriptExecutionCommand::None;
        uint64 sequence = 0;
    };

    /** @brief Runtime-visible bridge state; no VM object or pointer escapes here. */
    struct ScriptExecutionState
    {
        ScriptBindingStatus status = ScriptBindingStatus::Unbound;
        uint64 lastConsumedCommandSequence = 0;
        uint64 lastAppliedConfigurationRevision = 0;
        uint64 synchronizationRevision = 0;
    };

    static_assert(ECS::Fragment<ScriptBehaviour>);
    static_assert(ECS::Fragment<ScriptExecutionIntent>);
    static_assert(ECS::Fragment<ScriptExecutionState>);
} // namespace RVX::SceneECS
