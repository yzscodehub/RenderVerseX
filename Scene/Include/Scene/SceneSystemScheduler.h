#pragma once

/**
 * @file SceneSystemScheduler.h
 * @brief Deterministic fixed-phase scheduling for scene system bridges
 */

#include "Core/Handle.h"

#include <functional>
#include <string>
#include <vector>

namespace RVX
{
    enum class SceneUpdatePhase : uint8
    {
        BeginFrame = 0,
        Gameplay,
        AnimationPrePhysics,
        FixedPhysics,
        TransformResolve,
        BoundsSpatial,
        FeatureSystems,
        RenderExtraction,
        EndFrame
    };

    struct SceneSystemHandleTag final
    {
    };

    using SceneSystemHandle = Handle<SceneSystemHandleTag, uint32>;
    inline constexpr SceneSystemHandle InvalidSceneSystemHandle = SceneSystemHandle::Invalid();

    using SceneSystemCallback = std::function<void(float)>;

    class SceneSystemScheduler
    {
    public:
        SceneSystemHandle Register(std::string name,
                                   SceneUpdatePhase phase,
                                   SceneSystemCallback callback,
                                   int32 order = 0);
        bool Unregister(SceneSystemHandle handle);
        void Execute(SceneUpdatePhase phase, float deltaTime);
        void Clear();
        size_t GetSystemCount() const { return m_entries.size(); }

    private:
        struct Entry
        {
            SceneSystemHandle handle = InvalidSceneSystemHandle;
            std::string name;
            SceneUpdatePhase phase = SceneUpdatePhase::Gameplay;
            SceneSystemCallback callback;
            int32 order = 0;
            uint64 registrationSequence = 0;
        };

        HandlePool<SceneSystemHandle> m_handles;
        std::vector<Entry> m_entries;
        uint64 m_nextRegistrationSequence = 1;
    };
} // namespace RVX
