#include "Scene/ECS/TransformHierarchy.h"

#include "ECS/Query.h"
#include "Scene/ECS/TransformMath.h"

#include <algorithm>
#include <unordered_set>

namespace RVX::SceneECS
{
namespace
{
    template<ECS::Fragment T>
    std::vector<ECS::EntityHandle> CollectEnabledEntities(ECS::Registry& registry)
    {
        std::vector<ECS::EntityHandle> entities;
        registry.Query<T>().Each(
            [&entities](ECS::EntityHandle entity, const T&)
            {
                entities.push_back(entity);
            });
        std::sort(entities.begin(), entities.end());
        return entities;
    }
} // namespace

TransformHierarchy::TransformHierarchy(ECS::Registry& registry)
    : m_registry(&registry)
{
}

ECS::SceneRuntimeId TransformHierarchy::GetSceneRuntimeId() const
{
    return m_registry != nullptr ? m_registry->GetSceneRuntimeId() : ECS::SceneRuntimeId{};
}

ECS::EntityHandle TransformHierarchy::GetParent(ECS::EntityHandle entity) const
{
    if (m_registry == nullptr || !m_registry->IsAlive(entity))
    {
        return ECS::EntityHandle::Invalid();
    }

    const ParentRelation* relation = m_registry->TryGet<ParentRelation>(entity);
    return relation != nullptr ? relation->parent : ECS::EntityHandle::Invalid();
}

std::vector<ECS::EntityHandle> TransformHierarchy::GetChildren(ECS::EntityHandle parent) const
{
    std::vector<ECS::EntityHandle> children;
    if (m_registry == nullptr || !m_registry->IsAlive(parent))
    {
        return children;
    }

    m_registry->Query<ECS::Read<ParentRelation>>().EachIncludingAllDisabled(
        [parent, &children](ECS::EntityHandle child, const ParentRelation& relation)
        {
            if (relation.parent == parent)
            {
                children.push_back(child);
            }
        });
    std::sort(children.begin(), children.end());
    return children;
}

bool TransformHierarchy::SetLocalTransform(ECS::EntityHandle entity,
                                           const LocalTransform& transform)
{
    if (m_registry == nullptr)
    {
        return false;
    }

    return m_registry->Write<LocalTransform>(
        entity,
        [&transform](LocalTransform& target)
        {
            const uint64 nextRevision = target.revision + 1u;
            target = transform;
            target.revision = nextRevision;
        });
}

SetWorldPoseResult TransformHierarchy::SetWorldPose(ECS::EntityHandle entity,
                                                     const Vec3& worldTranslation,
                                                     const Quat& worldRotation)
{
    if (m_registry == nullptr || !m_registry->IsAlive(entity))
    {
        return SetWorldPoseResult::InvalidEntity;
    }
    if (!std::isfinite(worldTranslation.x) || !std::isfinite(worldTranslation.y) ||
        !std::isfinite(worldTranslation.z) || !std::isfinite(worldRotation.w) ||
        !std::isfinite(worldRotation.x) || !std::isfinite(worldRotation.y) ||
        !std::isfinite(worldRotation.z) ||
        glm::dot(worldRotation, worldRotation) <= 0.000001f)
    {
        return SetWorldPoseResult::InvalidPose;
    }
    if (!m_registry->Has<LocalTransform>(entity) ||
        !m_registry->Has<SimulationWorldTransform>(entity))
    {
        return SetWorldPoseResult::MissingTransform;
    }

    // Always convert from a current resolved hierarchy state. This makes a
    // teleport in the same substep use the parent pose seen by physics rather
    // than a stale local-space approximation.
    ResolveSimulationTransforms();
    const SimulationWorldTransform* currentWorld =
        m_registry->TryGet<SimulationWorldTransform>(entity);
    if (currentWorld == nullptr)
    {
        return SetWorldPoseResult::MissingTransform;
    }

    LocalTransform requestedWorld;
    DecomposeLocalTransformMatrix(currentWorld->matrix, requestedWorld);
    requestedWorld.translation = worldTranslation;
    requestedWorld.rotation = glm::normalize(worldRotation);
    const Mat4 requestedWorldMatrix = MakeLocalTransformMatrix(requestedWorld);

    LocalTransform requestedLocal;
    const ECS::EntityHandle parent = GetParent(entity);
    if (parent.IsValid())
    {
        const SimulationWorldTransform* parentWorld =
            m_registry->TryGet<SimulationWorldTransform>(parent);
        if (parentWorld == nullptr)
        {
            return SetWorldPoseResult::MissingTransform;
        }
        if (!TryMakeKeepWorldLocalTransform(
                parentWorld->matrix, requestedWorldMatrix, requestedLocal))
        {
            return SetWorldPoseResult::NonInvertibleParent;
        }
    }
    else
    {
        DecomposeLocalTransformMatrix(requestedWorldMatrix, requestedLocal);
    }

    return SetLocalTransform(entity, requestedLocal) ? SetWorldPoseResult::Applied
                                                       : SetWorldPoseResult::MutationRejected;
}

ReparentResult TransformHierarchy::Reparent(ECS::EntityHandle child,
                                            ECS::EntityHandle parent,
                                            ReparentMode mode)
{
    if (m_registry == nullptr || !m_registry->IsAlive(child))
    {
        return ReparentResult::InvalidChild;
    }
    if (parent.IsValid() && !m_registry->IsAlive(parent))
    {
        return ReparentResult::InvalidParent;
    }
    if (child == parent)
    {
        return ReparentResult::SelfParent;
    }
    if (!m_registry->Has<LocalTransform>(child) ||
        !m_registry->Has<SimulationWorldTransform>(child) ||
        (parent.IsValid() &&
         (!m_registry->Has<LocalTransform>(parent) ||
          !m_registry->Has<SimulationWorldTransform>(parent))))
    {
        return ReparentResult::MissingTransform;
    }
    if (parent.IsValid() && WouldCreateCycle(child, parent))
    {
        return ReparentResult::Cycle;
    }

    const ECS::EntityHandle currentParent = GetParent(child);
    if (currentParent == parent)
    {
        return ReparentResult::Applied;
    }

    ResolveSimulationTransforms();

    LocalTransform keepWorldLocal;
    if (mode == ReparentMode::KeepWorld)
    {
        const SimulationWorldTransform* childWorld =
            m_registry->TryGet<SimulationWorldTransform>(child);
        if (childWorld == nullptr)
        {
            return ReparentResult::MissingTransform;
        }

        if (parent.IsValid())
        {
            const SimulationWorldTransform* parentWorld =
                m_registry->TryGet<SimulationWorldTransform>(parent);
            if (parentWorld == nullptr ||
                !TryMakeKeepWorldLocalTransform(parentWorld->matrix,
                                                childWorld->matrix,
                                                keepWorldLocal))
            {
                return ReparentResult::NonInvertibleParent;
            }
        }
        else
        {
            DecomposeLocalTransformMatrix(childWorld->matrix, keepWorldLocal);
        }
    }

    bool topologyChanged = false;
    if (parent.IsValid())
    {
        if (m_registry->Has<ParentRelation>(child))
        {
            topologyChanged = m_registry->Write<ParentRelation>(
                child,
                [parent](ParentRelation& relation)
                {
                    relation.parent = parent;
                });
        }
        else
        {
            topologyChanged = m_registry->Add<ParentRelation>(child, {.parent = parent});
        }
    }
    else if (m_registry->Has<ParentRelation>(child))
    {
        topologyChanged = m_registry->Remove<ParentRelation>(child);
    }
    else
    {
        topologyChanged = true;
    }

    if (!topologyChanged)
    {
        return ReparentResult::MutationRejected;
    }
    if (mode == ReparentMode::KeepWorld && !SetLocalTransform(child, keepWorldLocal))
    {
        return ReparentResult::MutationRejected;
    }

    ResolveSimulationTransforms();
    return ReparentResult::Applied;
}

ReparentResult TransformHierarchy::Detach(ECS::EntityHandle child, ReparentMode mode)
{
    return Reparent(child, ECS::EntityHandle::Invalid(), mode);
}

TransformResolveStats TransformHierarchy::BeginSimulationFrame()
{
    TransformResolveStats stats = ResolveSimulationTransforms();
    if (m_registry == nullptr)
    {
        return stats;
    }

    const std::vector<ECS::EntityHandle> entities =
        CollectEnabledEntities<SimulationWorldTransform>(*m_registry);
    for (ECS::EntityHandle entity : entities)
    {
        const SimulationWorldTransform* simulation =
            m_registry->TryGet<SimulationWorldTransform>(entity);
        const PreviousSimulationWorldTransform* previous =
            m_registry->TryGet<PreviousSimulationWorldTransform>(entity);
        if (simulation == nullptr || previous == nullptr ||
            previous->sourceRevision == simulation->revision)
        {
            continue;
        }

        m_registry->Write<PreviousSimulationWorldTransform>(
            entity,
            [simulation](PreviousSimulationWorldTransform& target)
            {
                target.matrix = simulation->matrix;
                target.sourceRevision = simulation->revision;
            });
    }
    return stats;
}

TransformResolveStats TransformHierarchy::ResolveSimulationTransforms()
{
    TransformResolveStats stats;
    if (m_registry == nullptr)
    {
        return stats;
    }

    const std::vector<ECS::EntityHandle> entities =
        CollectEnabledEntities<LocalTransform>(*m_registry);
    stats.entityCount = static_cast<uint32>(entities.size());

    std::unordered_map<ECS::EntityHandle, ResolveState> states;
    states.reserve(entities.size());
    for (ECS::EntityHandle entity : entities)
    {
        std::vector<ECS::EntityHandle> stack;
        ResolveOne(entity, stack, states, stats);
    }
    return stats;
}

uint32 TransformHierarchy::SynchronizeRenderWorldTransforms()
{
    if (m_registry == nullptr)
    {
        return 0;
    }

    uint32 synchronizedCount = 0;
    const std::vector<ECS::EntityHandle> entities =
        CollectEnabledEntities<SimulationWorldTransform>(*m_registry);
    for (ECS::EntityHandle entity : entities)
    {
        const SimulationWorldTransform* simulation =
            m_registry->TryGet<SimulationWorldTransform>(entity);
        const RenderWorldTransform* render =
            m_registry->TryGet<RenderWorldTransform>(entity);
        if (simulation == nullptr || render == nullptr ||
            render->sourceRevision == simulation->revision)
        {
            continue;
        }

        if (m_registry->Write<RenderWorldTransform>(
                entity,
                [simulation](RenderWorldTransform& target)
                {
                    target.matrix = simulation->matrix;
                    target.sourceRevision = simulation->revision;
                }))
        {
            ++synchronizedCount;
        }
    }
    return synchronizedCount;
}

bool TransformHierarchy::WouldCreateCycle(ECS::EntityHandle child,
                                          ECS::EntityHandle parent) const
{
    if (m_registry == nullptr)
    {
        return false;
    }

    std::unordered_set<ECS::EntityHandle> visited;
    ECS::EntityHandle current = parent;
    while (current.IsValid())
    {
        if (current == child || !visited.insert(current).second)
        {
            return true;
        }
        if (!m_registry->IsAlive(current))
        {
            return false;
        }

        const ParentRelation* relation = m_registry->TryGet<ParentRelation>(current);
        current = relation != nullptr ? relation->parent : ECS::EntityHandle::Invalid();
    }
    return false;
}

TransformHierarchy::ResolveState TransformHierarchy::ResolveOne(
    ECS::EntityHandle entity,
    std::vector<ECS::EntityHandle>& stack,
    std::unordered_map<ECS::EntityHandle, ResolveState>& states,
    TransformResolveStats& stats)
{
    const auto existing = states.find(entity);
    if (existing != states.end())
    {
        if (existing->second == ResolveState::Visiting)
        {
            ++stats.cycleCount;
            return ResolveState::Failed;
        }
        return existing->second;
    }
    if (m_registry == nullptr || !m_registry->IsAlive(entity))
    {
        ++stats.invalidParentCount;
        return ResolveState::Failed;
    }

    states.emplace(entity, ResolveState::Visiting);
    stack.push_back(entity);

    const LocalTransform* local = m_registry->TryGet<LocalTransform>(entity);
    const SimulationWorldTransform* currentWorld =
        m_registry->TryGet<SimulationWorldTransform>(entity);
    if (local == nullptr || currentWorld == nullptr)
    {
        states[entity] = ResolveState::Failed;
        stack.pop_back();
        return ResolveState::Failed;
    }

    const ParentRelation* relation = m_registry->TryGet<ParentRelation>(entity);
    const ECS::EntityHandle parent =
        relation != nullptr ? relation->parent : ECS::EntityHandle::Invalid();
    const uint64 parentWriteVersion =
        m_registry->GetFragmentWriteVersion<ParentRelation>(entity);

    Mat4 worldMatrix = MakeLocalTransformMatrix(*local);
    uint64 parentWorldRevision = 0;
    if (parent.IsValid())
    {
        if (!m_registry->IsAlive(parent) ||
            ResolveOne(parent, stack, states, stats) != ResolveState::Resolved)
        {
            ++stats.invalidParentCount;
            states[entity] = ResolveState::Failed;
            stack.pop_back();
            return ResolveState::Failed;
        }

        const SimulationWorldTransform* parentWorld =
            m_registry->TryGet<SimulationWorldTransform>(parent);
        if (parentWorld == nullptr)
        {
            ++stats.invalidParentCount;
            states[entity] = ResolveState::Failed;
            stack.pop_back();
            return ResolveState::Failed;
        }
        worldMatrix = parentWorld->matrix * worldMatrix;
        parentWorldRevision = parentWorld->revision;
    }

    const uint64 localWriteVersion =
        m_registry->GetFragmentWriteVersion<LocalTransform>(entity);
    const bool requiresUpdate =
        currentWorld->revision == 0 ||
        currentWorld->sourceLocalRevision != local->revision ||
        currentWorld->sourceLocalWriteVersion != localWriteVersion ||
        currentWorld->sourceParentWriteVersion != parentWriteVersion ||
        currentWorld->sourceParentWorldRevision != parentWorldRevision ||
        currentWorld->resolvedParent != parent;

    if (requiresUpdate)
    {
        const uint64 worldRevision = m_nextSimulationWorldRevision++;
        if (!m_registry->Write<SimulationWorldTransform>(
                entity,
                [&worldMatrix,
                 localRevision = local->revision,
                 localWriteVersion,
                 parentWriteVersion,
                 parentWorldRevision,
                 parent,
                 worldRevision](SimulationWorldTransform& target)
                {
                    target.matrix = worldMatrix;
                    target.revision = worldRevision;
                    target.sourceLocalRevision = localRevision;
                    target.sourceLocalWriteVersion = localWriteVersion;
                    target.sourceParentWriteVersion = parentWriteVersion;
                    target.sourceParentWorldRevision = parentWorldRevision;
                    target.resolvedParent = parent;
                }))
        {
            states[entity] = ResolveState::Failed;
            stack.pop_back();
            return ResolveState::Failed;
        }
        ++stats.resolvedCount;
    }
    else
    {
        ++stats.unchangedCount;
    }

    states[entity] = ResolveState::Resolved;
    stack.pop_back();
    return ResolveState::Resolved;
}
} // namespace RVX::SceneECS
