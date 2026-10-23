#include "PhysicsSceneAdapters/ECS/PhysicsEcsBridge.h"

#include "ECS/Query.h"
#include "Physics/Shapes/CollisionShape.h"
#include "Scene/ECS/AnimationFragments.h"
#include "Scene/ECS/PhysicsFragments.h"
#include "Scene/ECS/TransformMath.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RVX::PhysicsSceneAdapters
{
namespace
{
    constexpr float TransformInvertibilityEpsilon = 0.000001f;

    [[nodiscard]] bool IsFinite(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    [[nodiscard]] bool IsFinite(const Quat& value)
    {
        return std::isfinite(value.w) && std::isfinite(value.x) &&
               std::isfinite(value.y) && std::isfinite(value.z) &&
               glm::dot(value, value) > 0.000001f;
    }

    [[nodiscard]] bool IsExactRootMotionIntent(const SceneECS::RootMotionIntent& left,
                                                const SceneECS::RootMotionIntent& right)
    {
        return left.sceneRuntimeIdValue == right.sceneRuntimeIdValue &&
               left.targetEntity == right.targetEntity &&
               left.physicsBodyHandlePacked == right.physicsBodyHandlePacked &&
               left.sourcePoseSequence == right.sourcePoseSequence &&
               left.rootMotionSequence == right.rootMotionSequence &&
               left.fixedStepSequence == right.fixedStepSequence &&
               left.translationDelta.x == right.translationDelta.x &&
               left.translationDelta.y == right.translationDelta.y &&
               left.translationDelta.z == right.translationDelta.z &&
               left.rotationDelta.w == right.rotationDelta.w &&
               left.rotationDelta.x == right.rotationDelta.x &&
               left.rotationDelta.y == right.rotationDelta.y &&
               left.rotationDelta.z == right.rotationDelta.z &&
               left.pending == right.pending && left.sequenceRejected == right.sequenceRejected;
    }

    [[nodiscard]] bool IsFinite(const Mat4& matrix)
    {
        for (uint32 column = 0; column < 4u; ++column)
        {
            for (uint32 row = 0; row < 4u; ++row)
            {
                if (!std::isfinite(matrix[column][row]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool IsValidMotionType(SceneECS::RigidBodyMotionType type)
    {
        switch (type)
        {
        case SceneECS::RigidBodyMotionType::Static:
        case SceneECS::RigidBodyMotionType::Kinematic:
        case SceneECS::RigidBodyMotionType::Dynamic:
            return true;
        }
        return false;
    }

    [[nodiscard]] bool IsValidMotionQuality(SceneECS::RigidBodyMotionQuality quality)
    {
        switch (quality)
        {
        case SceneECS::RigidBodyMotionQuality::Discrete:
        case SceneECS::RigidBodyMotionQuality::LinearCast:
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<Physics::BodyType> ToPhysicsBodyType(
        SceneECS::RigidBodyMotionType type)
    {
        switch (type)
        {
        case SceneECS::RigidBodyMotionType::Static: return Physics::BodyType::Static;
        case SceneECS::RigidBodyMotionType::Kinematic: return Physics::BodyType::Kinematic;
        case SceneECS::RigidBodyMotionType::Dynamic: return Physics::BodyType::Dynamic;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Physics::MotionQuality> ToPhysicsMotionQuality(
        SceneECS::RigidBodyMotionQuality quality)
    {
        switch (quality)
        {
        case SceneECS::RigidBodyMotionQuality::Discrete: return Physics::MotionQuality::Discrete;
        case SceneECS::RigidBodyMotionQuality::LinearCast: return Physics::MotionQuality::LinearCast;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool IsBuiltInBackend(const Physics::PhysicsWorld& physicsWorld)
    {
        return physicsWorld.IsInitialized() &&
               physicsWorld.GetActiveBackendType() == Physics::PhysicsBackendType::BuiltIn &&
               !physicsWorld.IsBackendFallbackActive() &&
               (physicsWorld.GetRequestedBackendType() == Physics::PhysicsBackendType::Auto ||
                physicsWorld.GetRequestedBackendType() == Physics::PhysicsBackendType::BuiltIn);
    }

    struct ShapeBuildResult
    {
        std::shared_ptr<Physics::CollisionShape> shape;
        SceneECS::PhysicsBodyBindingStatus failure =
            SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
    };

    [[nodiscard]] std::optional<SceneECS::PhysicsBodyBindingStatus> ValidateCollider(
        const SceneECS::Collider& collider)
    {
        if (!IsFinite(collider.localOffset) || !IsFinite(collider.localRotation) ||
            !std::isfinite(collider.friction) || !std::isfinite(collider.restitution) ||
            !std::isfinite(collider.density) || collider.friction < 0.0f ||
            collider.restitution < 0.0f || collider.density <= 0.0f)
        {
            return SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
        }

        switch (collider.shape)
        {
        case SceneECS::ColliderShapeType::Box:
            if (!IsFinite(collider.boxHalfExtents) || collider.boxHalfExtents.x <= 0.0f ||
                collider.boxHalfExtents.y <= 0.0f || collider.boxHalfExtents.z <= 0.0f)
            {
                return SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
            }
            break;
        case SceneECS::ColliderShapeType::Sphere:
            if (!std::isfinite(collider.sphereRadius) || collider.sphereRadius <= 0.0f)
            {
                return SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
            }
            break;
        case SceneECS::ColliderShapeType::Capsule:
            if (!std::isfinite(collider.capsuleRadius) ||
                !std::isfinite(collider.capsuleHalfHeight) || collider.capsuleRadius <= 0.0f ||
                collider.capsuleHalfHeight < 0.0f)
            {
                return SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
            }
            break;
        case SceneECS::ColliderShapeType::Convex:
        case SceneECS::ColliderShapeType::Mesh:
            return SceneECS::PhysicsBodyBindingStatus::UnsupportedCollider;
        default:
            return SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
        }

        return std::nullopt;
    }

    [[nodiscard]] ShapeBuildResult BuildShape(const SceneECS::Collider& collider)
    {
        if (const auto failure = ValidateCollider(collider); failure.has_value())
        {
            return {.failure = *failure};
        }

        std::shared_ptr<Physics::CollisionShape> shape;
        try
        {
            switch (collider.shape)
            {
            case SceneECS::ColliderShapeType::Box:
                shape = Physics::BoxShape::Create(collider.boxHalfExtents);
                break;
            case SceneECS::ColliderShapeType::Sphere:
                shape = Physics::SphereShape::Create(collider.sphereRadius);
                break;
            case SceneECS::ColliderShapeType::Capsule:
                shape = Physics::CapsuleShape::Create(collider.capsuleRadius, collider.capsuleHalfHeight);
                break;
            case SceneECS::ColliderShapeType::Convex:
            case SceneECS::ColliderShapeType::Mesh:
                return {.failure = SceneECS::PhysicsBodyBindingStatus::UnsupportedCollider};
            default:
                return {.failure = SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration};
            }
        }
        catch (...)
        {
            return {};
        }

        if (shape != nullptr)
        {
            shape->SetMaterial({
                .friction = collider.friction,
                .restitution = collider.restitution,
                .density = collider.density,
            });
        }
        return {.shape = std::move(shape)};
    }

    [[nodiscard]] bool IsValidRigidBody(const SceneECS::RigidBody& rigidBody)
    {
        return IsValidMotionType(rigidBody.motionType) &&
               IsValidMotionQuality(rigidBody.motionQuality) &&
               std::isfinite(rigidBody.mass) && std::isfinite(rigidBody.linearDamping) &&
               std::isfinite(rigidBody.angularDamping) && std::isfinite(rigidBody.gravityScale) &&
               rigidBody.linearDamping >= 0.0f && rigidBody.angularDamping >= 0.0f &&
               (rigidBody.motionType != SceneECS::RigidBodyMotionType::Dynamic ||
                rigidBody.mass > 0.0f);
    }

    [[nodiscard]] std::optional<Physics::BodyConfiguration> MakeConfiguration(
        const SceneECS::RigidBody& rigidBody)
    {
        if (!IsValidRigidBody(rigidBody))
        {
            return std::nullopt;
        }

        const std::optional<Physics::BodyType> type = ToPhysicsBodyType(rigidBody.motionType);
        const std::optional<Physics::MotionQuality> quality =
            ToPhysicsMotionQuality(rigidBody.motionQuality);
        if (!type.has_value() || !quality.has_value())
        {
            return std::nullopt;
        }

        return Physics::BodyConfiguration{
            .type = *type,
            .motionQuality = *quality,
            .mass = rigidBody.mass,
            .linearDamping = rigidBody.linearDamping,
            .angularDamping = rigidBody.angularDamping,
            .gravityScale = rigidBody.gravityScale,
            .positionConstraints = rigidBody.positionConstraints,
            .rotationConstraints = rigidBody.rotationConstraints,
            .layer = rigidBody.collisionLayer,
            .collisionMask = rigidBody.collisionMask,
            .group = {.groupId = rigidBody.collisionGroup, .subGroupId = rigidBody.collisionSubGroup},
            .allowSleep = rigidBody.allowSleep,
            .isTrigger = rigidBody.isTrigger,
        };
    }

    [[nodiscard]] std::optional<Physics::BodyPose> MakePose(
        const SceneECS::SimulationWorldTransform& transform)
    {
        if (!IsFinite(transform.matrix))
        {
            return std::nullopt;
        }

        SceneECS::LocalTransform worldTransform;
        SceneECS::DecomposeLocalTransformMatrix(transform.matrix, worldTransform);
        if (!IsFinite(worldTransform.translation) || !IsFinite(worldTransform.rotation) ||
            !IsFinite(worldTransform.scale))
        {
            return std::nullopt;
        }
        return Physics::BodyPose{
            .position = worldTransform.translation,
            .rotation = worldTransform.rotation,
        };
    }

    [[nodiscard]] bool IsFinite(const Physics::BodyState& state)
    {
        return IsFinite(state.pose.position) && IsFinite(state.pose.rotation) &&
               IsFinite(state.linearVelocity) && IsFinite(state.angularVelocity);
    }

    [[nodiscard]] std::optional<Physics::RigidBodyDesc> MakeBodyDesc(
        const SceneECS::RigidBody& rigidBody,
        const SceneECS::PhysicsBodyState* state,
        const SceneECS::SimulationWorldTransform& transform)
    {
        const std::optional<Physics::BodyConfiguration> configuration =
            MakeConfiguration(rigidBody);
        const std::optional<Physics::BodyPose> pose = MakePose(transform);
        if (!configuration.has_value() || !pose.has_value() ||
            (state != nullptr && (!IsFinite(state->linearVelocity) ||
                                  !IsFinite(state->angularVelocity))))
        {
            return std::nullopt;
        }
        return Physics::RigidBodyDesc{
            .type = configuration->type,
            .motionQuality = configuration->motionQuality,
            .position = pose->position,
            .rotation = pose->rotation,
            .linearVelocity = state != nullptr ? state->linearVelocity : Vec3(0.0f),
            .angularVelocity = state != nullptr ? state->angularVelocity : Vec3(0.0f),
            .mass = configuration->mass,
            .linearDamping = configuration->linearDamping,
            .angularDamping = configuration->angularDamping,
            .gravityScale = configuration->gravityScale,
            .positionConstraints = configuration->positionConstraints,
            .rotationConstraints = configuration->rotationConstraints,
            .layer = configuration->layer,
            .collisionMask = configuration->collisionMask,
            .group = configuration->group,
            .allowSleep = configuration->allowSleep,
            .startAsleep = rigidBody.startAsleep,
            .isTrigger = configuration->isTrigger,
        };
    }

    /**
     * @brief Verify that a dynamic body's world-pose writes can be converted to local space.
     *
     * TransformHierarchy performs the final conversion, but checking the complete
     * authoritative ParentRelation chain before binding prevents a dynamic body
     * from entering simulation when the later writeback is known to be impossible.
     */
    [[nodiscard]] bool HasInvertibleParentAncestry(ECS::Registry& registry,
                                                    ECS::EntityHandle entity)
    {
        std::unordered_set<ECS::EntityHandle> visited;
        visited.reserve(8u);
        ECS::EntityHandle current = entity;
        while (current.IsValid())
        {
            if (!visited.emplace(current).second)
            {
                return false;
            }

            const SceneECS::ParentRelation* relation =
                registry.TryGet<SceneECS::ParentRelation>(current);
            if (relation == nullptr || !relation->parent.IsValid())
            {
                return true;
            }

            current = relation->parent;
            if (!registry.IsAlive(current))
            {
                return false;
            }
            const SceneECS::SimulationWorldTransform* parentWorld =
                registry.TryGet<SceneECS::SimulationWorldTransform>(current);
            if (parentWorld == nullptr || !IsFinite(parentWorld->matrix))
            {
                return false;
            }
            const float determinant = glm::determinant(parentWorld->matrix);
            if (!std::isfinite(determinant) ||
                std::abs(determinant) <= TransformInvertibilityEpsilon)
            {
                return false;
            }
        }
        return false;
    }
} // namespace

struct PhysicsEcsBridge::State
{
    struct Binding
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        Physics::BodyHandle body = Physics::BodyHandle::Invalid();
        uint64 rigidBodyWriteVersion = 0;
        uint64 colliderWriteVersion = 0;
        uint64 lastConsumedRootMotionSequence = 0;
        uint64 lastConsumedRootMotionFixedStep = 0;
        uint64 pendingBaselineRootMotionSequence = 0;
        // A failed first application retains this exact value-only record. It
        // is the sole exception to the normal same-fixed-step provenance rule;
        // later source values cannot replace or skip this retry.
        SceneECS::RootMotionIntent pendingRootMotionRetry;
        // Root-motion stream sequences are evaluator/bridge-owned and therefore
        // do not have to start at one. The exact generation-safe body handle is
        // the consumer epoch: its first well-formed intent is locked, its first
        // successful apply completes the baseline, and replay/gap checks are
        // strict throughout either state. sourcePoseSequence is provenance only.
        bool rootMotionSequenceBaselinePending = true;
        bool rootMotionRetryPending = false;
        SceneECS::PhysicsBodyBindingStatus status = SceneECS::PhysicsBodyBindingStatus::Unbound;
        bool awaitingCleanupAcknowledgement = false;
        bool configurationDirty = false;
        bool colliderDirty = false;
    };

    State(SceneECS::SceneEcsRuntime& runtimeIn, Physics::PhysicsWorld& physicsWorldIn)
        : runtime(&runtimeIn)
        , physicsWorld(&physicsWorldIn)
        , sceneRuntimeId(runtimeIn.GetSceneRuntimeId())
    {
    }

    ~State()
    {
        // Bridge destruction is a terminal ownership boundary. The processor
        // callbacks retain only a weak State, so no later tick can revive these
        // bodies after this deterministic handle-only release.
        if (physicsWorld == nullptr)
        {
            return;
        }

        std::vector<Physics::BodyHandle> bodies;
        bodies.reserve(bindings.size());
        for (const Binding& binding : bindings)
        {
            if (binding.body.IsValid())
            {
                bodies.push_back(binding.body);
            }
        }
        std::sort(bodies.begin(), bodies.end());
        for (const Physics::BodyHandle body : bodies)
        {
            static_cast<void>(physicsWorld->DestroyBodyValue(body));
        }
    }

    [[nodiscard]] uint32 FindBindingIndex(ECS::EntityHandle entity) const
    {
        const auto found = entityToBinding.find(entity);
        return found != entityToBinding.end() ? found->second : RVX_INVALID_INDEX;
    }

    void WriteState(ECS::Registry& registry,
                    ECS::EntityHandle entity,
                    SceneECS::PhysicsBodyBindingStatus status,
                    const Physics::BodyState* bodyState = nullptr,
                    uint64 fixedStep = 0)
    {
        if (!registry.Has<SceneECS::PhysicsBodyState>(entity))
        {
            return;
        }

        registry.Write<SceneECS::PhysicsBodyState>(
            entity,
            [status, bodyState, fixedStep](SceneECS::PhysicsBodyState& target)
            {
                target.status = status;
                if (bodyState != nullptr)
                {
                    target.linearVelocity = bodyState->linearVelocity;
                    target.angularVelocity = bodyState->angularVelocity;
                }
                if (fixedStep != 0)
                {
                    target.lastSynchronizedFixedStep = fixedStep;
                }
                ++target.bindingRevision;
            });
    }

    [[nodiscard]] bool HasPhysicsCleanupRequirement(const SceneECS::EntityLifecycleState* lifecycle) const
    {
        return lifecycle != nullptr &&
               (lifecycle->requiredCleanupDomains &
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics)) != 0;
    }

    void EraseBinding(uint32 index)
    {
        if (index >= bindings.size())
        {
            return;
        }

        const Binding removed = bindings[index];
        entityToBinding.erase(removed.entity);
        if (removed.body.IsValid())
        {
            bodyToBinding.erase(removed.body.GetPackedValue());
        }

        const uint32 lastIndex = static_cast<uint32>(bindings.size() - 1u);
        if (index != lastIndex)
        {
            bindings[index] = std::move(bindings[lastIndex]);
            entityToBinding[bindings[index].entity] = index;
            if (bindings[index].body.IsValid())
            {
                bodyToBinding[bindings[index].body.GetPackedValue()] = index;
            }
        }
        bindings.pop_back();
    }

    [[nodiscard]] bool AddBinding(ECS::EntityHandle entity,
                                  Physics::BodyHandle body,
                                  uint64 rigidBodyWriteVersion,
                                  uint64 colliderWriteVersion)
    {
        try
        {
            bindings.reserve(bindings.size() + 1u);
            entityToBinding.reserve(entityToBinding.size() + 1u);
            bodyToBinding.reserve(bodyToBinding.size() + 1u);
            const uint32 index = static_cast<uint32>(bindings.size());
            bindings.push_back({
                .entity = entity,
                .body = body,
                .rigidBodyWriteVersion = rigidBodyWriteVersion,
                .colliderWriteVersion = colliderWriteVersion,
                .status = SceneECS::PhysicsBodyBindingStatus::Active,
            });
            entityToBinding.emplace(entity, index);
            bodyToBinding.emplace(body.GetPackedValue(), index);
            return true;
        }
        catch (...)
        {
            const auto found = entityToBinding.find(entity);
            if (found != entityToBinding.end())
            {
                entityToBinding.erase(found);
            }
            bodyToBinding.erase(body.GetPackedValue());
            if (!bindings.empty() && bindings.back().entity == entity)
            {
                bindings.pop_back();
            }
            return false;
        }
    }

    [[nodiscard]] bool Detach(ECS::Registry& registry,
                              ECS::EntityHandle entity,
                              bool retainCleanupEvidence)
    {
        const uint32 index = FindBindingIndex(entity);
        if (index == RVX_INVALID_INDEX)
        {
            return true;
        }

        Binding& binding = bindings[index];
        if (binding.body.IsValid())
        {
            const Physics::BodyDestroyStatus result = physicsWorld->DestroyBodyValue(binding.body);
            if (result == Physics::BodyDestroyStatus::ReleaseFailed)
            {
                return false;
            }
            bodyToBinding.erase(binding.body.GetPackedValue());
            binding.body = Physics::BodyHandle::Invalid();
        }

        if (retainCleanupEvidence)
        {
            binding.awaitingCleanupAcknowledgement = true;
            binding.status = SceneECS::PhysicsBodyBindingStatus::PendingDestroy;
            WriteState(registry, entity, binding.status);
            return true;
        }

        WriteState(registry, entity, SceneECS::PhysicsBodyBindingStatus::Unbound);
        EraseBinding(index);
        return true;
    }

    [[nodiscard]] bool IsEligible(ECS::Registry& registry,
                                  ECS::EntityHandle entity,
                                  const SceneECS::RigidBody*& rigidBody,
                                  const SceneECS::Collider*& collider,
                                  const SceneECS::SimulationWorldTransform*& transform,
                                  const SceneECS::EntityLifecycleState*& lifecycle,
                                  SceneECS::PhysicsBodyBindingStatus& failure) const
    {
        rigidBody = nullptr;
        collider = nullptr;
        transform = nullptr;
        lifecycle = registry.TryGet<SceneECS::EntityLifecycleState>(entity);
        failure = SceneECS::PhysicsBodyBindingStatus::Unbound;
        if (!registry.IsAlive(entity) || lifecycle == nullptr ||
            lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive ||
            !registry.IsEnabled(entity) || !registry.Has<SceneECS::RigidBody>(entity) ||
            !registry.Has<SceneECS::Collider>(entity) ||
            !registry.Has<SceneECS::SimulationWorldTransform>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::RigidBody>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::Collider>(entity))
        {
            return false;
        }

        rigidBody = registry.TryGet<SceneECS::RigidBody>(entity);
        collider = registry.TryGet<SceneECS::Collider>(entity);
        transform = registry.TryGet<SceneECS::SimulationWorldTransform>(entity);
        if (rigidBody == nullptr || collider == nullptr || transform == nullptr ||
            !IsValidRigidBody(*rigidBody))
        {
            failure = SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
            return false;
        }

        if (const auto colliderFailure = ValidateCollider(*collider); colliderFailure.has_value())
        {
            failure = *colliderFailure;
            return false;
        }
        if (!MakePose(*transform).has_value())
        {
            failure = SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
            return false;
        }
        if (rigidBody->motionType == SceneECS::RigidBodyMotionType::Dynamic &&
            !HasInvertibleParentAncestry(registry, entity))
        {
            failure = SceneECS::PhysicsBodyBindingStatus::InvalidHierarchy;
            return false;
        }
        return true;
    }

    void Reconcile(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        const ECS::StructuralJournalRead read = registry.ReadStructuralChanges(structuralCursor);
        const bool continuityLost = read.continuity == ECS::StructuralJournalContinuity::Lost;
        if (continuityLost)
        {
            ++structuralContinuityLossCount;
        }
        if (!initialReconcileComplete || continuityLost)
        {
            ++authoritativeReconcileCount;
        }
        initialReconcileComplete = true;

        if (continuityLost)
        {
            for (Binding& binding : bindings)
            {
                binding.configurationDirty = true;
                binding.colliderDirty = true;
            }
        }
        else
        {
            for (const ECS::StructuralChange& change : read.changes)
            {
                const uint32 index = FindBindingIndex(change.entity);
                if (index == RVX_INVALID_INDEX)
                {
                    continue;
                }

                Binding& binding = bindings[index];
                if (change.fragmentType == std::type_index(typeid(SceneECS::RigidBody)))
                {
                    binding.configurationDirty = true;
                }
                if (change.fragmentType == std::type_index(typeid(SceneECS::Collider)))
                {
                    binding.colliderDirty = true;
                }
            }
        }

        // The reconciliation itself is deliberately include-disabled and
        // authoritative every barrier. Structural entries tell us when a
        // cursor was lost; the scan also covers lifecycle writes, which are
        // intentionally non-structural.
        std::vector<ECS::EntityHandle> existingBindings;
        existingBindings.reserve(bindings.size());
        for (const Binding& binding : bindings)
        {
            existingBindings.push_back(binding.entity);
        }
        std::sort(existingBindings.begin(), existingBindings.end());
        for (const ECS::EntityHandle entity : existingBindings)
        {
            const SceneECS::EntityLifecycleState* lifecycle =
                registry.TryGet<SceneECS::EntityLifecycleState>(entity);
            const bool retain = HasPhysicsCleanupRequirement(lifecycle) &&
                                lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive;
            const SceneECS::RigidBody* rigidBody = nullptr;
            const SceneECS::Collider* collider = nullptr;
            const SceneECS::SimulationWorldTransform* transform = nullptr;
            SceneECS::PhysicsBodyBindingStatus failure;
            if (!IsEligible(registry, entity, rigidBody, collider, transform, lifecycle, failure))
            {
                if (!Detach(registry, entity, retain))
                {
                    context.ReportFailure("Physics body release failed during ECS reconciliation.");
                    return;
                }
                if (!retain && registry.IsAlive(entity))
                {
                    WriteState(registry, entity, failure);
                }
            }
        }

        std::vector<ECS::EntityHandle> entities;
        registry.Query<ECS::Read<SceneECS::RigidBody>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity, const SceneECS::RigidBody&)
            {
                entities.push_back(entity);
            });
        std::sort(entities.begin(), entities.end());

        for (const ECS::EntityHandle entity : entities)
        {
            const SceneECS::RigidBody* rigidBody = nullptr;
            const SceneECS::Collider* collider = nullptr;
            const SceneECS::SimulationWorldTransform* transform = nullptr;
            const SceneECS::EntityLifecycleState* lifecycle = nullptr;
            SceneECS::PhysicsBodyBindingStatus failure;
            if (!IsEligible(registry, entity, rigidBody, collider, transform, lifecycle, failure))
            {
                if (failure == SceneECS::PhysicsBodyBindingStatus::UnsupportedCollider)
                {
                    ++unsupportedColliderCount;
                }
                WriteState(registry, entity, failure);
                continue;
            }

            if (FindBindingIndex(entity) != RVX_INVALID_INDEX)
            {
                continue;
            }

            const ShapeBuildResult shape = BuildShape(*collider);
            if (shape.shape == nullptr)
            {
                if (shape.failure == SceneECS::PhysicsBodyBindingStatus::UnsupportedCollider)
                {
                    ++unsupportedColliderCount;
                }
                WriteState(registry, entity, shape.failure);
                continue;
            }
            const SceneECS::PhysicsBodyState* state =
                registry.TryGet<SceneECS::PhysicsBodyState>(entity);
            const std::optional<Physics::RigidBodyDesc> bodyDesc =
                MakeBodyDesc(*rigidBody, state, *transform);
            if (!bodyDesc.has_value())
            {
                WriteState(registry, entity, SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration);
                continue;
            }
            const Physics::BodyCreateResult created =
                physicsWorld->CreateBodyValue(*bodyDesc);
            if (!created.Succeeded())
            {
                ++createFailureCount;
                WriteState(registry, entity, SceneECS::PhysicsBodyBindingStatus::CreateFailed);
                continue;
            }
            if (!physicsWorld->ReplaceBodyCollider(created.handle,
                                                   shape.shape,
                                                   collider->localOffset,
                                                   collider->localRotation,
                                                   rigidBody->isTrigger,
                                                   false) ||
                !AddBinding(entity,
                            created.handle,
                            registry.GetFragmentWriteVersion<SceneECS::RigidBody>(entity),
                            registry.GetFragmentWriteVersion<SceneECS::Collider>(entity)))
            {
                static_cast<void>(physicsWorld->DestroyBodyValue(created.handle));
                ++createFailureCount;
                WriteState(registry, entity, SceneECS::PhysicsBodyBindingStatus::CreateFailed);
                continue;
            }
            WriteState(registry, entity, SceneECS::PhysicsBodyBindingStatus::Active);
        }
    }

    enum class RootMotionApplyResult : uint8
    {
        None = 0,
        Applied,
        Rejected,
    };

    [[nodiscard]] bool RestoreRootMotionIntent(ECS::Registry& registry,
                                               ECS::EntityHandle entity,
                                               const SceneECS::RootMotionIntent& original,
                                               ECS::ProcessorExecutionContext& context)
    {
        if (!registry.Write<SceneECS::RootMotionIntent>(
                entity,
                [&original](SceneECS::RootMotionIntent& target)
                {
                    target = original;
                }))
        {
            context.ReportFailure("Root-motion intent rollback was rejected.");
            return false;
        }
        return true;
    }

    [[nodiscard]] RootMotionApplyResult ApplyPendingRootMotion(
        ECS::ProcessorExecutionContext& context,
        Binding& binding,
        const SceneECS::RigidBody& rigidBody,
        const SceneECS::SimulationWorldTransform& transform)
    {
        ECS::Registry& registry = context.registry;
        if (!registry.Has<SceneECS::RootMotionIntent>(binding.entity) ||
            !registry.IsFragmentEnabled<SceneECS::RootMotionIntent>(binding.entity))
        {
            return RootMotionApplyResult::None;
        }

        const SceneECS::RootMotionIntent* source =
            registry.TryGet<SceneECS::RootMotionIntent>(binding.entity);
        if (source == nullptr)
        {
            return RootMotionApplyResult::None;
        }
        const SceneECS::RootMotionIntent intent = *source;

        const auto reject = [this]()
        {
            ++rootMotionRejectedCount;
            return RootMotionApplyResult::Rejected;
        };

        if (binding.rootMotionRetryPending)
        {
            // Do not relax fixed-step provenance globally. Only the exact
            // record captured before the failed apply may cross a later fixed
            // step, and it must remain value-for-value identical while pending.
            if (!IsExactRootMotionIntent(intent, binding.pendingRootMotionRetry))
            {
                if (intent.rootMotionSequence >
                    binding.pendingRootMotionRetry.rootMotionSequence)
                {
                    ++rootMotionGapCount;
                }
                else if (intent.rootMotionSequence <= binding.lastConsumedRootMotionSequence)
                {
                    ++rootMotionReplayCount;
                }
                static_cast<void>(RestoreRootMotionIntent(
                    registry, binding.entity, binding.pendingRootMotionRetry, context));
                return reject();
            }
            if (context.fixedStepSequence <= intent.fixedStepSequence)
            {
                return reject();
            }
        }
        else if (!intent.pending)
        {
            return RootMotionApplyResult::None;
        }

        if (!binding.rootMotionRetryPending &&
            (intent.sequenceRejected || intent.sourcePoseSequence == 0 ||
             intent.rootMotionSequence == 0 ||
             intent.sceneRuntimeIdValue != sceneRuntimeId.GetValue() ||
             intent.targetEntity != binding.entity ||
             intent.physicsBodyHandlePacked != binding.body.GetPackedValue() ||
             intent.fixedStepSequence != context.fixedStepSequence ||
             !IsFinite(intent.translationDelta) || !IsFinite(intent.rotationDelta)))
        {
            return reject();
        }
        if (rigidBody.motionType != SceneECS::RigidBodyMotionType::Kinematic)
        {
            return reject();
        }
        if (binding.rootMotionSequenceBaselinePending)
        {
            if (binding.pendingBaselineRootMotionSequence == 0)
            {
                // Lock the first well-formed intent for this exact body epoch
                // before any fallible pose/body write.  A failed apply may be
                // retried with the same value, but a later root-motion stream
                // sequence can never silently skip it.
                binding.pendingBaselineRootMotionSequence = intent.rootMotionSequence;
            }
            else if (intent.rootMotionSequence < binding.pendingBaselineRootMotionSequence)
            {
                ++rootMotionReplayCount;
                return reject();
            }
            else if (intent.rootMotionSequence > binding.pendingBaselineRootMotionSequence)
            {
                ++rootMotionGapCount;
                return reject();
            }
        }
        else if (intent.rootMotionSequence <= binding.lastConsumedRootMotionSequence)
        {
            ++rootMotionReplayCount;
            return reject();
        }
        else if (binding.lastConsumedRootMotionSequence == std::numeric_limits<uint64>::max() ||
                 intent.rootMotionSequence != binding.lastConsumedRootMotionSequence + 1u)
        {
            ++rootMotionGapCount;
            return reject();
        }

        // Lock the exact value before the first fallible body/pose
        // preparation. A later root-motion stream sequence cannot leapfrog
        // this retry, while an unchanged record may be applied in a later
        // fixed step after the transient preparation or write failure clears.
        if (!binding.rootMotionRetryPending)
        {
            binding.pendingRootMotionRetry = intent;
            binding.rootMotionRetryPending = true;
        }

        const std::optional<Physics::BodyState> bodyState =
            physicsWorld->ReadBodyState(binding.body);
        const std::optional<Physics::BodyPose> currentPose = MakePose(transform);
        if (!bodyState.has_value() || !currentPose.has_value() ||
            bodyState->configuration.type != Physics::BodyType::Kinematic)
        {
            return reject();
        }

        Physics::BodyPose desiredPose;
        desiredPose.position = currentPose->position +
                               currentPose->rotation * intent.translationDelta;
        desiredPose.rotation = glm::normalize(
            currentPose->rotation * intent.rotationDelta);
        if (!IsFinite(desiredPose.position) || !IsFinite(desiredPose.rotation))
        {
            return reject();
        }

        // Claim first so a later successful body write cannot be replayed by a
        // second processor invocation. All later failures restore the locked
        // record and deliberately leave Binding's consumed sequence intact.
        if (!registry.Write<SceneECS::RootMotionIntent>(
                binding.entity,
                [](SceneECS::RootMotionIntent& target)
                {
                    target.pending = false;
                }))
        {
            return reject();
        }

        if (runtime->SetSimulationWorldPose(
                binding.entity, desiredPose.position, desiredPose.rotation) !=
            SceneECS::SetWorldPoseResult::Applied)
        {
            static_cast<void>(RestoreRootMotionIntent(
                registry, binding.entity, binding.pendingRootMotionRetry, context));
            return reject();
        }
        if (!physicsWorld->WriteBodyPose(binding.body, desiredPose))
        {
            const SceneECS::SetWorldPoseResult rollback = runtime->SetSimulationWorldPose(
                binding.entity, currentPose->position, currentPose->rotation);
            static_cast<void>(RestoreRootMotionIntent(
                registry, binding.entity, binding.pendingRootMotionRetry, context));
            if (rollback != SceneECS::SetWorldPoseResult::Applied)
            {
                context.ReportFailure("Root-motion transform rollback was rejected.");
                return RootMotionApplyResult::Rejected;
            }
            return reject();
        }

        binding.lastConsumedRootMotionSequence = intent.rootMotionSequence;
        binding.lastConsumedRootMotionFixedStep = intent.fixedStepSequence;
        binding.pendingBaselineRootMotionSequence = 0;
        binding.rootMotionSequenceBaselinePending = false;
        binding.rootMotionRetryPending = false;
        ++rootMotionAppliedCount;
        return RootMotionApplyResult::Applied;
    }

    void PushSceneToPhysics(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        std::vector<ECS::EntityHandle> entities;
        entities.reserve(bindings.size());
        for (const Binding& binding : bindings)
        {
            if (binding.body.IsValid())
            {
                entities.push_back(binding.entity);
            }
        }
        std::sort(entities.begin(), entities.end());

        for (const ECS::EntityHandle entity : entities)
        {
            const uint32 index = FindBindingIndex(entity);
            if (index == RVX_INVALID_INDEX)
            {
                continue;
            }
            Binding& binding = bindings[index];
            const SceneECS::RigidBody* rigidBody = nullptr;
            const SceneECS::Collider* collider = nullptr;
            const SceneECS::SimulationWorldTransform* transform = nullptr;
            const SceneECS::EntityLifecycleState* lifecycle = nullptr;
            SceneECS::PhysicsBodyBindingStatus failure;
            if (!IsEligible(registry, entity, rigidBody, collider, transform, lifecycle, failure))
            {
                const bool retain = HasPhysicsCleanupRequirement(lifecycle) &&
                                    lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive;
                if (!Detach(registry, entity, retain))
                {
                    context.ReportFailure("Physics body release failed before fixed simulation.");
                }
                else if (!retain && registry.IsAlive(entity))
                {
                    WriteState(registry, entity, failure);
                }
                continue;
            }

            const uint64 rigidBodyVersion =
                registry.GetFragmentWriteVersion<SceneECS::RigidBody>(entity);
            if (binding.configurationDirty || rigidBodyVersion != binding.rigidBodyWriteVersion)
            {
                const std::optional<Physics::BodyConfiguration> configuration =
                    MakeConfiguration(*rigidBody);
                if (!configuration.has_value() ||
                    !physicsWorld->WriteBodyConfiguration(binding.body, *configuration))
                {
                    const bool detached = Detach(registry, entity, false);
                    WriteState(registry, entity,
                               SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration);
                    if (!detached)
                    {
                        context.ReportFailure(
                            "Physics body release failed after configuration rejection.");
                    }
                    continue;
                }
                binding.rigidBodyWriteVersion = rigidBodyVersion;
                binding.configurationDirty = false;
            }

            const uint64 colliderVersion = registry.GetFragmentWriteVersion<SceneECS::Collider>(entity);
            if (binding.colliderDirty || colliderVersion != binding.colliderWriteVersion)
            {
                const ShapeBuildResult shape = BuildShape(*collider);
                if (shape.shape == nullptr)
                {
                    if (shape.failure == SceneECS::PhysicsBodyBindingStatus::UnsupportedCollider)
                    {
                        ++unsupportedColliderCount;
                    }
                    const bool detached = Detach(registry, entity, false);
                    WriteState(registry, entity, shape.failure);
                    if (!detached)
                    {
                        context.ReportFailure("Physics body release failed after collider replacement rejection.");
                    }
                    continue;
                }
                if (!physicsWorld->ReplaceBodyCollider(binding.body,
                                                       shape.shape,
                                                       collider->localOffset,
                                                       collider->localRotation,
                                                       rigidBody->isTrigger))
                {
                    binding.status = SceneECS::PhysicsBodyBindingStatus::CreateFailed;
                    WriteState(registry, entity, binding.status);
                    continue;
                }
                binding.colliderWriteVersion = colliderVersion;
                binding.colliderDirty = false;
            }

            const RootMotionApplyResult rootMotion = ApplyPendingRootMotion(
                context, binding, *rigidBody, *transform);
            if (context.HasReportedFailure())
            {
                return;
            }
            if (rootMotion == RootMotionApplyResult::Applied)
            {
                // Root motion already wrote the exact kinematic target.  Do
                // not overwrite it with the pre-root resolved transform.
                continue;
            }

            if (rigidBody->motionType == SceneECS::RigidBodyMotionType::Static ||
                rigidBody->motionType == SceneECS::RigidBodyMotionType::Kinematic)
            {
                const std::optional<Physics::BodyPose> pose = MakePose(*transform);
                if (!pose.has_value())
                {
                    const bool detached = Detach(registry, entity, false);
                    WriteState(registry, entity,
                               SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration);
                    if (!detached)
                    {
                        context.ReportFailure("Physics body release failed after invalid scene pose.");
                    }
                    continue;
                }
                if (!physicsWorld->WriteBodyPose(
                        binding.body, *pose))
                {
                    const bool detached = Detach(registry, entity, false);
                    WriteState(registry, entity,
                               SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration);
                    if (!detached)
                    {
                        context.ReportFailure("Scene-to-physics pose push rejected a live body.");
                    }
                    continue;
                }
                ++sceneToPhysicsPushCount;
            }
        }
    }

    void StepPhysics(ECS::ProcessorExecutionContext& context)
    {
        if (!IsBuiltInBackend(*physicsWorld))
        {
            context.ReportFailure("PhysicsEcsBridge supports the Built-in backend only.");
            return;
        }
        physicsWorld->StepFixed(static_cast<float>(context.deltaSeconds));
    }

    void PullPhysicsToScene(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        std::vector<ECS::EntityHandle> entities;
        entities.reserve(bindings.size());
        for (const Binding& binding : bindings)
        {
            if (binding.body.IsValid())
            {
                entities.push_back(binding.entity);
            }
        }
        std::sort(entities.begin(), entities.end());

        for (const ECS::EntityHandle entity : entities)
        {
            const uint32 index = FindBindingIndex(entity);
            if (index == RVX_INVALID_INDEX)
            {
                continue;
            }
            Binding& binding = bindings[index];
            const SceneECS::RigidBody* rigidBody = nullptr;
            const SceneECS::Collider* collider = nullptr;
            const SceneECS::SimulationWorldTransform* transform = nullptr;
            const SceneECS::EntityLifecycleState* lifecycle = nullptr;
            SceneECS::PhysicsBodyBindingStatus eligibilityFailure;
            const bool eligible = IsEligible(registry, entity, rigidBody, collider, transform,
                                             lifecycle, eligibilityFailure);
            const std::optional<Physics::BodyState> bodyState =
                physicsWorld->ReadBodyState(binding.body);
            if (!eligible || !bodyState.has_value() || !IsFinite(*bodyState))
            {
                const bool retain = HasPhysicsCleanupRequirement(lifecycle) &&
                                    lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive;
                const bool detached = Detach(registry, entity, retain);
                if (!retain && registry.IsAlive(entity))
                {
                    WriteState(registry, entity,
                               eligible ? SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration :
                                          eligibilityFailure);
                }
                if (!detached)
                {
                    context.ReportFailure("Stale Physics body could not be released from the ECS binding.");
                }
                continue;
            }

            if (rigidBody->motionType == SceneECS::RigidBodyMotionType::Dynamic)
            {
                const SceneECS::SetWorldPoseResult poseResult = runtime->SetSimulationWorldPose(
                    entity, bodyState->pose.position, bodyState->pose.rotation);
                if (poseResult != SceneECS::SetWorldPoseResult::Applied)
                {
                    const SceneECS::PhysicsBodyBindingStatus failure =
                        poseResult == SceneECS::SetWorldPoseResult::NonInvertibleParent ?
                            SceneECS::PhysicsBodyBindingStatus::InvalidHierarchy :
                            SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration;
                    const bool detached = Detach(registry, entity, false);
                    WriteState(registry, entity, failure, &*bodyState, context.fixedStepSequence);
                    if (!detached)
                    {
                        context.ReportFailure(
                            "Physics body release failed after world-pose conversion rejection.");
                        return;
                    }
                    context.ReportFailure("Physics-to-scene world pose could not be converted to local space.");
                    return;
                }
                ++physicsToScenePullCount;
            }
            binding.status = SceneECS::PhysicsBodyBindingStatus::Active;
            WriteState(registry, entity, binding.status, &*bodyState, context.fixedStepSequence);
        }
    }

    void AcknowledgeRetainedCleanup(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        std::vector<ECS::EntityHandle> entities;
        // Lifecycle is the durable cleanup evidence. Scanning it every cleanup
        // phase ensures an acknowledgement failure remains retryable even after
        // the cleanup journal cursor has advanced, including bodies that were
        // never created because their data was unsupported or invalid.
        registry.Query<ECS::Read<SceneECS::EntityLifecycleState>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity,
                        const SceneECS::EntityLifecycleState& lifecycle)
            {
                if (lifecycle.phase == SceneECS::EntityLifecyclePhase::CleanupRequired &&
                    (lifecycle.requiredCleanupDomains &
                     SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics)) != 0)
                {
                    entities.push_back(entity);
                }
            });
        std::sort(entities.begin(), entities.end());
        for (const ECS::EntityHandle entity : entities)
        {
            const uint32 index = FindBindingIndex(entity);
            const SceneECS::EntityLifecycleState* lifecycle =
                registry.TryGet<SceneECS::EntityLifecycleState>(entity);
            if (!HasPhysicsCleanupRequirement(lifecycle) ||
                lifecycle->phase != SceneECS::EntityLifecyclePhase::CleanupRequired)
            {
                continue;
            }
            if (index != RVX_INVALID_INDEX && bindings[index].body.IsValid() &&
                !Detach(registry, entity, true))
            {
                context.ReportFailure("Physics body release failed while retrying cleanup acknowledgement.");
                return;
            }
            if (!runtime->AcknowledgeCleanup(
                    entity,
                    SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics)))
            {
                context.ReportFailure("Physics cleanup acknowledgement was rejected.");
                return;
            }
            EraseBinding(index);
        }
    }

    void Cleanup(ECS::ProcessorExecutionContext& context)
    {
        // Retry durable cleanup work before consuming new journal records. This
        // preserves evidence across a transient destroy/acknowledge failure.
        AcknowledgeRetainedCleanup(context);
        if (context.HasReportedFailure())
        {
            return;
        }

        const SceneECS::CleanupRecordRead read = runtime->ReadCleanupRecords(cleanupCursor);
        if (read.continuity == SceneECS::CleanupRecordContinuity::Lost)
        {
            ++cleanupContinuityLossCount;
            Reconcile(context);
            if (context.HasReportedFailure())
            {
                return;
            }
            AcknowledgeRetainedCleanup(context);
            return;
        }

        for (const SceneECS::CleanupRecord& record : read.records)
        {
            if (record.sceneRuntimeId != sceneRuntimeId ||
                (record.requiredCleanupDomains &
                 SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics)) == 0)
            {
                continue;
            }

            if (!Detach(context.registry, record.entity, true))
            {
                context.ReportFailure("Physics body release failed during cleanup.");
                return;
            }
        }
        AcknowledgeRetainedCleanup(context);
    }

    SceneECS::SceneEcsRuntime* runtime = nullptr;
    Physics::PhysicsWorld* physicsWorld = nullptr;
    ECS::SceneRuntimeId sceneRuntimeId;
    ECS::StructuralJournalCursor structuralCursor;
    SceneECS::CleanupRecordCursor cleanupCursor;
    std::vector<Binding> bindings;
    std::unordered_map<ECS::EntityHandle, uint32> entityToBinding;
    std::unordered_map<uint64, uint32> bodyToBinding;
    PhysicsEcsBridgeRegistrationResult registration =
        PhysicsEcsBridgeRegistrationResult::InvalidRuntime;
    uint64 structuralContinuityLossCount = 0;
    uint64 cleanupContinuityLossCount = 0;
    uint64 authoritativeReconcileCount = 0;
    uint64 createFailureCount = 0;
    uint64 unsupportedColliderCount = 0;
    uint64 sceneToPhysicsPushCount = 0;
    uint64 physicsToScenePullCount = 0;
    uint64 rootMotionAppliedCount = 0;
    uint64 rootMotionRejectedCount = 0;
    uint64 rootMotionReplayCount = 0;
    uint64 rootMotionGapCount = 0;
    bool initialReconcileComplete = false;
    bool processorsEnabled = false;
};

PhysicsEcsBridge::PhysicsEcsBridge(SceneECS::SceneEcsRuntime& runtime, Physics::PhysicsWorld& physicsWorld)
    : m_state(std::make_shared<State>(runtime, physicsWorld))
{
}

PhysicsEcsBridge::~PhysicsEcsBridge() = default;

PhysicsEcsBridgeRegistrationResult PhysicsEcsBridge::RegisterProcessors()
{
    if (m_state == nullptr || m_state->runtime == nullptr || m_state->physicsWorld == nullptr ||
        !m_state->sceneRuntimeId.IsValid())
    {
        return PhysicsEcsBridgeRegistrationResult::InvalidRuntime;
    }
    if (m_state->registration == PhysicsEcsBridgeRegistrationResult::Registered)
    {
        return PhysicsEcsBridgeRegistrationResult::AlreadyRegistered;
    }
    if (!m_state->physicsWorld->IsInitialized())
    {
        m_state->registration = PhysicsEcsBridgeRegistrationResult::PhysicsWorldUnavailable;
        return m_state->registration;
    }
    if (!IsBuiltInBackend(*m_state->physicsWorld))
    {
        m_state->registration = PhysicsEcsBridgeRegistrationResult::UnsupportedBackend;
        return m_state->registration;
    }

    const std::string prefix = "PhysicsEcsBridge." +
                               std::to_string(m_state->sceneRuntimeId.GetValue()) + ".";
    const std::weak_ptr<State> weakState = m_state;
    const auto buildProcessorDescriptor = [weakState, prefix](
                                            std::string suffix,
                                            ECS::ProcessorPhase phase,
                                            std::vector<std::type_index> reads,
                                            std::vector<std::type_index> writes,
                                            std::function<void(State&, ECS::ProcessorExecutionContext&)> run)
    {
        ECS::ProcessorDescriptor descriptor;
        descriptor.name = prefix + std::move(suffix);
        descriptor.phase = phase;
        descriptor.stepMode = phase == ECS::ProcessorPhase::BeginSimulation ||
                                      phase == ECS::ProcessorPhase::EndFrameCleanup ?
                                  ECS::ProcessorStepMode::Variable :
                                  ECS::ProcessorStepMode::Fixed;
        descriptor.access.reads = std::move(reads);
        descriptor.access.writes = std::move(writes);
        descriptor.access.resourceWrites = {std::type_index(typeid(Physics::PhysicsWorld))};
        descriptor.runWithContext = [weakState, run = std::move(run)](
                                        ECS::ProcessorExecutionContext& context)
        {
            if (const std::shared_ptr<State> state = weakState.lock())
            {
                if (state->processorsEnabled)
                {
                    run(*state, context);
                }
            }
        };
        return descriptor;
    };

    const std::vector<std::type_index> reconcileReads = {
        std::type_index(typeid(SceneECS::RigidBody)),
        std::type_index(typeid(SceneECS::Collider)),
        std::type_index(typeid(SceneECS::PhysicsBodyState)),
        std::type_index(typeid(SceneECS::EntityLifecycleState)),
        std::type_index(typeid(SceneECS::SimulationWorldTransform)),
        std::type_index(typeid(SceneECS::ParentRelation)),
    };
    const std::vector<std::type_index> reconcileWrites = {
        std::type_index(typeid(SceneECS::PhysicsBodyState)),
    };

    // Build the entire batch before mutating runtime state. SceneEcsRuntime
    // validates and publishes this batch atomically, so a naming or phase
    // conflict cannot leave an inert subset behind and callers may retry.
    std::vector<ECS::ProcessorDescriptor> descriptors;
    try
    {
        descriptors.reserve(6u);
        descriptors.push_back(buildProcessorDescriptor(
            "BeginSimulationReconcile", ECS::ProcessorPhase::BeginSimulation,
            reconcileReads, reconcileWrites,
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Reconcile(context);
            }));
        descriptors.push_back(buildProcessorDescriptor(
            "BeforeFixedStepReconcile", ECS::ProcessorPhase::BeforeFixedStep,
            reconcileReads, reconcileWrites,
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Reconcile(context);
            }));
        descriptors.push_back(buildProcessorDescriptor(
            "SceneToPhysics", ECS::ProcessorPhase::SceneToPhysics,
            {std::type_index(typeid(SceneECS::RigidBody)),
             std::type_index(typeid(SceneECS::Collider)),
             std::type_index(typeid(SceneECS::PhysicsBodyState)),
             std::type_index(typeid(SceneECS::EntityLifecycleState)),
             std::type_index(typeid(SceneECS::LocalTransform)),
             std::type_index(typeid(SceneECS::SimulationWorldTransform)),
             std::type_index(typeid(SceneECS::ParentRelation)),
             std::type_index(typeid(SceneECS::RootMotionIntent))},
            {std::type_index(typeid(SceneECS::LocalTransform)),
             std::type_index(typeid(SceneECS::SimulationWorldTransform)),
             std::type_index(typeid(SceneECS::PhysicsBodyState)),
             std::type_index(typeid(SceneECS::RootMotionIntent))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.PushSceneToPhysics(context);
            }));
        descriptors.push_back(buildProcessorDescriptor(
            "PhysicsSimulation", ECS::ProcessorPhase::PhysicsSimulation,
            {}, {},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.StepPhysics(context);
            }));
        descriptors.push_back(buildProcessorDescriptor(
            "PhysicsToScene", ECS::ProcessorPhase::PhysicsToScene,
            {std::type_index(typeid(SceneECS::RigidBody)),
             std::type_index(typeid(SceneECS::Collider)),
             std::type_index(typeid(SceneECS::PhysicsBodyState)),
             std::type_index(typeid(SceneECS::EntityLifecycleState)),
             std::type_index(typeid(SceneECS::SimulationWorldTransform)),
             std::type_index(typeid(SceneECS::ParentRelation))},
            {std::type_index(typeid(SceneECS::LocalTransform)),
             std::type_index(typeid(SceneECS::SimulationWorldTransform)),
             std::type_index(typeid(SceneECS::PhysicsBodyState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.PullPhysicsToScene(context);
            }));
        descriptors.push_back(buildProcessorDescriptor(
            "EndFrameCleanup", ECS::ProcessorPhase::EndFrameCleanup,
            {std::type_index(typeid(SceneECS::EntityLifecycleState))},
            {std::type_index(typeid(SceneECS::EntityLifecycleState)),
             std::type_index(typeid(SceneECS::PhysicsBodyState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Cleanup(context);
            }));
    }
    catch (...)
    {
        m_state->registration = PhysicsEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
        return m_state->registration;
    }

    const bool registered = m_state->runtime->RegisterProcessors(std::move(descriptors));

    m_state->processorsEnabled = registered;
    m_state->registration = registered ? PhysicsEcsBridgeRegistrationResult::Registered :
                                         PhysicsEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
    return m_state->registration;
}

bool PhysicsEcsBridge::IsRegistered() const
{
    return m_state != nullptr &&
           m_state->registration == PhysicsEcsBridgeRegistrationResult::Registered;
}

ECS::SceneRuntimeId PhysicsEcsBridge::GetSceneRuntimeId() const
{
    return m_state != nullptr ? m_state->sceneRuntimeId : ECS::SceneRuntimeId{};
}

Physics::BodyHandle PhysicsEcsBridge::FindBody(ECS::SceneRuntimeId sceneRuntimeId,
                                               ECS::EntityHandle entity) const
{
    if (m_state == nullptr || sceneRuntimeId != m_state->sceneRuntimeId)
    {
        return Physics::BodyHandle::Invalid();
    }
    const uint32 index = m_state->FindBindingIndex(entity);
    return index != RVX_INVALID_INDEX ? m_state->bindings[index].body :
                                        Physics::BodyHandle::Invalid();
}

PhysicsEcsBridgeDiagnosticsSnapshot PhysicsEcsBridge::GetDiagnosticsSnapshot() const
{
    PhysicsEcsBridgeDiagnosticsSnapshot snapshot;
    if (m_state == nullptr)
    {
        return snapshot;
    }

    snapshot.sceneRuntimeId = m_state->sceneRuntimeId;
    snapshot.registration = m_state->registration;
    snapshot.structuralContinuityLossCount = m_state->structuralContinuityLossCount;
    snapshot.cleanupContinuityLossCount = m_state->cleanupContinuityLossCount;
    snapshot.authoritativeReconcileCount = m_state->authoritativeReconcileCount;
    snapshot.createFailureCount = m_state->createFailureCount;
    snapshot.unsupportedColliderCount = m_state->unsupportedColliderCount;
    snapshot.sceneToPhysicsPushCount = m_state->sceneToPhysicsPushCount;
    snapshot.physicsToScenePullCount = m_state->physicsToScenePullCount;
    snapshot.rootMotionAppliedCount = m_state->rootMotionAppliedCount;
    snapshot.rootMotionRejectedCount = m_state->rootMotionRejectedCount;
    snapshot.rootMotionReplayCount = m_state->rootMotionReplayCount;
    snapshot.rootMotionGapCount = m_state->rootMotionGapCount;
    snapshot.bindings.reserve(m_state->bindings.size());
    for (const State::Binding& binding : m_state->bindings)
    {
        snapshot.bindings.push_back({
            .entity = binding.entity,
            .body = binding.body,
            .status = binding.status,
            .lastConsumedRootMotionSequence = binding.lastConsumedRootMotionSequence,
            .lastConsumedRootMotionFixedStep = binding.lastConsumedRootMotionFixedStep,
            .awaitingCleanupAcknowledgement = binding.awaitingCleanupAcknowledgement,
        });
        if (binding.body.IsValid())
        {
            ++snapshot.activeBodyCount;
        }
        if (binding.awaitingCleanupAcknowledgement)
        {
            ++snapshot.pendingCleanupCount;
        }
    }
    std::sort(snapshot.bindings.begin(), snapshot.bindings.end(),
              [](const PhysicsEcsBodyBindingDiagnostic& lhs,
                 const PhysicsEcsBodyBindingDiagnostic& rhs)
              {
                  return lhs.entity < rhs.entity;
              });
    return snapshot;
}
} // namespace RVX::PhysicsSceneAdapters
