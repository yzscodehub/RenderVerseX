/**
 * @file PhysicsWorld.cpp
 * @brief PhysicsWorld implementation
 */

#include "Physics/PhysicsWorld.h"
#include "Physics/Shapes/CollisionShape.h"
#include "Physics/Constraints/IConstraint.h"
#include "Core/Math/Intersection.h"
#include "Core/Math/Ray.h"
#include "Core/Math/AABB.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace RVX::Physics
{
namespace
{
    struct AABBContact
    {
        Vec3 normal{0.0f, 1.0f, 0.0f};
        Vec3 point{0.0f};
        float depth = 0.0f;
    };

    struct LinearCastHit
    {
        Vec3 normal{0.0f, 1.0f, 0.0f};
        float fraction = 1.0f;
    };

    struct QueryBroadphasePrimitive
    {
        AABB bounds;
        size_t bodyIndex = 0;
    };

    struct QueryBroadphaseNode
    {
        AABB bounds;
        uint32 firstPrimitive = 0;
        uint32 primitiveCount = 0;
        uint32 leftChild = std::numeric_limits<uint32>::max();
        uint32 rightChild = std::numeric_limits<uint32>::max();

        bool IsLeaf() const
        {
            return primitiveCount > 0;
        }
    };

    constexpr uint32 kQueryBroadphaseLeafSize = 4;

    PhysicsBackendConfig ToBackendConfig(const PhysicsWorldConfig& config)
    {
        PhysicsBackendConfig backendConfig;
        backendConfig.gravity = config.gravity;
        backendConfig.maxBodies = config.maxBodies;
        backendConfig.maxBodyPairs = config.maxBodyPairs;
        backendConfig.maxContactConstraints = config.maxContactConstraints;
        backendConfig.velocitySteps = config.velocitySteps;
        backendConfig.positionSteps = config.positionSteps;
        backendConfig.enableSleeping = config.sleepTimeThreshold > 0.0f;
        backendConfig.enableCCD = true;
        return backendConfig;
    }

    PhysicsBackendType ResolveBackendType(PhysicsBackendType requested)
    {
        if (requested == PhysicsBackendType::Auto)
        {
            return PhysicsBackendFactory::IsAvailable(PhysicsBackendType::Jolt)
                ? PhysicsBackendType::Jolt
                : PhysicsBackendType::BuiltIn;
        }

        return PhysicsBackendFactory::IsAvailable(requested)
            ? requested
            : PhysicsBackendType::BuiltIn;
    }

    uint32 LayerToMask(CollisionLayer layer)
    {
        const uint32 layerValue = static_cast<uint32>(layer);
        if (layerValue >= 32u)
        {
            return layerValue;
        }
        return 1u << layerValue;
    }

    bool MaskContainsLayer(uint32 mask, CollisionLayer layer)
    {
        const uint32 layerValue = static_cast<uint32>(layer);
        const uint32 layerBit = LayerToMask(layer);
        return (mask & layerBit) != 0u || (layerValue != 0u && (mask & layerValue) != 0u);
    }

    bool QueryCanHitBody(const RigidBody& body, uint32 layerMask)
    {
        return MaskContainsLayer(layerMask, body.GetLayer()) &&
               (body.GetCollisionMask() & layerMask) != 0u;
    }

    bool CollisionGroupsAllowInteraction(const CollisionGroup& groupA, const CollisionGroup& groupB)
    {
        if (groupA.groupId == 0u || groupB.groupId == 0u)
        {
            return true;
        }

        return groupA.groupId != groupB.groupId || groupA.subGroupId != groupB.subGroupId;
    }

    bool BodiesCanCollide(const RigidBody& bodyA, const RigidBody& bodyB)
    {
        if (bodyA.IsStatic() && bodyB.IsStatic())
        {
            return false;
        }

        if (!CollisionGroupsAllowInteraction(bodyA.GetGroup(), bodyB.GetGroup()))
        {
            return false;
        }

        return MaskContainsLayer(bodyA.GetCollisionMask(), bodyB.GetLayer()) &&
               MaskContainsLayer(bodyB.GetCollisionMask(), bodyA.GetLayer());
    }

    bool BodyAABBsOverlap(const RigidBody& bodyA, const RigidBody& bodyB)
    {
        const RigidBody::AABB aabbA = bodyA.GetAABB();
        const RigidBody::AABB aabbB = bodyB.GetAABB();
        return AABB(aabbA.min, aabbA.max).Overlaps(AABB(aabbB.min, aabbB.max));
    }

    bool TrySweepAABBAgainstAABB(const RigidBody::AABB& movingAABB,
                                 const Vec3& displacement,
                                 const RigidBody::AABB& targetAABB,
                                 LinearCastHit& hit)
    {
        if (dot(displacement, displacement) <= 0.000001f)
        {
            return false;
        }

        const AABB movingBounds(movingAABB.min, movingAABB.max);
        const AABB targetBounds(targetAABB.min, targetAABB.max);
        if (movingBounds.Overlaps(targetBounds))
        {
            return false;
        }

        const Vec3 movingCenter = (movingAABB.min + movingAABB.max) * 0.5f;
        const Vec3 movingHalfExtents = (movingAABB.max - movingAABB.min) * 0.5f;
        const Vec3 expandedMin = targetAABB.min - movingHalfExtents;
        const Vec3 expandedMax = targetAABB.max + movingHalfExtents;

        float entryFraction = 0.0f;
        float exitFraction = 1.0f;
        Vec3 entryNormal{0.0f};

        auto testAxis = [&](float origin,
                            float delta,
                            float slabMin,
                            float slabMax,
                            const Vec3& negativeNormal,
                            const Vec3& positiveNormal) {
            if (std::abs(delta) <= 0.000001f)
            {
                return origin >= slabMin && origin <= slabMax;
            }

            float axisEntry = 0.0f;
            float axisExit = 0.0f;
            Vec3 axisNormal{0.0f};
            if (delta > 0.0f)
            {
                axisEntry = (slabMin - origin) / delta;
                axisExit = (slabMax - origin) / delta;
                axisNormal = negativeNormal;
            }
            else
            {
                axisEntry = (slabMax - origin) / delta;
                axisExit = (slabMin - origin) / delta;
                axisNormal = positiveNormal;
            }

            if (axisEntry > entryFraction)
            {
                entryFraction = axisEntry;
                entryNormal = axisNormal;
            }
            exitFraction = std::min(exitFraction, axisExit);
            return entryFraction <= exitFraction;
        };

        if (!testAxis(movingCenter.x, displacement.x, expandedMin.x, expandedMax.x,
                      Vec3(-1.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)) ||
            !testAxis(movingCenter.y, displacement.y, expandedMin.y, expandedMax.y,
                      Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)) ||
            !testAxis(movingCenter.z, displacement.z, expandedMin.z, expandedMax.z,
                      Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, 1.0f)))
        {
            return false;
        }

        if (entryFraction < 0.0f || entryFraction > 1.0f)
        {
            return false;
        }

        hit.fraction = entryFraction;
        hit.normal = entryNormal;
        return true;
    }

    bool TryBuildAABBContact(const RigidBody& bodyA, const RigidBody& bodyB, AABBContact& contact)
    {
        const RigidBody::AABB aabbA = bodyA.GetAABB();
        const RigidBody::AABB aabbB = bodyB.GetAABB();
        const Vec3 overlapMin = glm::max(aabbA.min, aabbB.min);
        const Vec3 overlapMax = glm::min(aabbA.max, aabbB.max);

        if (overlapMin.x > overlapMax.x ||
            overlapMin.y > overlapMax.y ||
            overlapMin.z > overlapMax.z)
        {
            return false;
        }

        const Vec3 overlapSize = overlapMax - overlapMin;
        contact.depth = overlapSize.x;
        contact.normal = bodyA.GetPosition().x <= bodyB.GetPosition().x
            ? Vec3(1.0f, 0.0f, 0.0f)
            : Vec3(-1.0f, 0.0f, 0.0f);

        if (overlapSize.y < contact.depth)
        {
            contact.depth = overlapSize.y;
            contact.normal = bodyA.GetPosition().y <= bodyB.GetPosition().y
                ? Vec3(0.0f, 1.0f, 0.0f)
                : Vec3(0.0f, -1.0f, 0.0f);
        }

        if (overlapSize.z < contact.depth)
        {
            contact.depth = overlapSize.z;
            contact.normal = bodyA.GetPosition().z <= bodyB.GetPosition().z
                ? Vec3(0.0f, 0.0f, 1.0f)
                : Vec3(0.0f, 0.0f, -1.0f);
        }

        contact.point = (overlapMin + overlapMax) * 0.5f;
        return contact.depth > 0.0f;
    }

    uint8 ConstraintMaskForAxis(const Vec3& axis)
    {
        const Vec3 absAxis = glm::abs(axis);
        if (absAxis.x >= absAxis.y && absAxis.x >= absAxis.z)
        {
            return 1u;
        }
        if (absAxis.y >= absAxis.z)
        {
            return 2u;
        }
        return 4u;
    }

    float GetContactInverseMass(const RigidBody& body, const Vec3& contactNormal)
    {
        if (!body.IsDynamic())
        {
            return 0.0f;
        }

        const uint8 axisMask = ConstraintMaskForAxis(contactNormal);
        if ((body.GetPositionConstraints() & axisMask) != 0u)
        {
            return 0.0f;
        }

        return body.GetInverseMass();
    }

    void ResolveAABBContact(RigidBody& bodyA, RigidBody& bodyB, const AABBContact& contact)
    {
        if (bodyA.IsTrigger() || bodyB.IsTrigger())
        {
            return;
        }

        const float invMassA = GetContactInverseMass(bodyA, contact.normal);
        const float invMassB = GetContactInverseMass(bodyB, contact.normal);
        const float totalInvMass = invMassA + invMassB;
        if (totalInvMass <= 0.0f)
        {
            return;
        }

        const Vec3 correction = contact.normal * (contact.depth / totalInvMass);
        if (invMassA > 0.0f)
        {
            bodyA.SetPosition(bodyA.GetPosition() - correction * invMassA);
        }
        if (invMassB > 0.0f)
        {
            bodyB.SetPosition(bodyB.GetPosition() + correction * invMassB);
        }

        const Vec3 relativeVelocity = bodyB.GetLinearVelocity() - bodyA.GetLinearVelocity();
        const float relativeNormalVelocity = dot(relativeVelocity, contact.normal);
        if (relativeNormalVelocity >= 0.0f)
        {
            return;
        }

        const float impulseMagnitude = -relativeNormalVelocity / totalInvMass;
        const Vec3 impulse = contact.normal * impulseMagnitude;
        if (invMassA > 0.0f)
        {
            bodyA.SetLinearVelocity(bodyA.GetLinearVelocity() - impulse * invMassA);
        }
        if (invMassB > 0.0f)
        {
            bodyB.SetLinearVelocity(bodyB.GetLinearVelocity() + impulse * invMassB);
        }
    }

    Vec4 GetDebugBodyColor(const RigidBody& body)
    {
        if (body.IsTrigger())
        {
            return Vec4(1.0f, 0.7f, 0.1f, 1.0f);
        }
        if (body.IsStatic())
        {
            return Vec4(0.55f, 0.6f, 0.65f, 1.0f);
        }
        if (body.IsKinematic())
        {
            return Vec4(0.25f, 0.75f, 1.0f, 1.0f);
        }
        return Vec4(0.25f, 0.9f, 0.45f, 1.0f);
    }

    Vec4 GetDebugBroadphaseColor()
    {
        return Vec4(0.75f, 0.35f, 1.0f, 1.0f);
    }

    Vec4 GetDebugConstraintColor(const Constraint& constraint)
    {
        switch (constraint.GetType())
        {
            case ConstraintType::Fixed:
                return Vec4(0.2f, 0.85f, 1.0f, 1.0f);
            case ConstraintType::Hinge:
                return Vec4(0.25f, 0.65f, 1.0f, 1.0f);
            case ConstraintType::Slider:
                return Vec4(0.2f, 1.0f, 0.75f, 1.0f);
            case ConstraintType::Spring:
                return Vec4(1.0f, 0.55f, 0.2f, 1.0f);
            case ConstraintType::Distance:
                return Vec4(1.0f, 0.9f, 0.25f, 1.0f);
            default:
                return Vec4(0.6f, 0.85f, 1.0f, 1.0f);
        }
    }

    Vec3 GetConstraintAnchorWorld(const RigidBody* body, const Vec3& localOrWorldAnchor)
    {
        if (!body)
        {
            return localOrWorldAnchor;
        }

        return Vec3(body->GetTransform() * Vec4(localOrWorldAnchor, 1.0f));
    }

    void AppendDebugLine(const Vec3& start,
                         const Vec3& end,
                         const Vec4& color,
                         std::vector<Vec3>& lines,
                         std::vector<Vec4>& colors)
    {
        lines.push_back(start);
        lines.push_back(end);
        colors.push_back(color);
        colors.push_back(color);
    }

    void AppendAABBDebugLines(const AABB& aabb,
                              const Vec4& color,
                              std::vector<Vec3>& lines,
                              std::vector<Vec4>& colors);

    void AppendAABBDebugLines(const RigidBody::AABB& aabb,
                              const Vec4& color,
                              std::vector<Vec3>& lines,
                              std::vector<Vec4>& colors)
    {
        AppendAABBDebugLines(AABB(aabb.min, aabb.max), color, lines, colors);
    }

    void AppendAABBDebugLines(const AABB& aabb,
                              const Vec4& color,
                              std::vector<Vec3>& lines,
                              std::vector<Vec4>& colors)
    {
        const Vec3 corners[8] = {
            Vec3(aabb.GetMin().x, aabb.GetMin().y, aabb.GetMin().z),
            Vec3(aabb.GetMax().x, aabb.GetMin().y, aabb.GetMin().z),
            Vec3(aabb.GetMax().x, aabb.GetMax().y, aabb.GetMin().z),
            Vec3(aabb.GetMin().x, aabb.GetMax().y, aabb.GetMin().z),
            Vec3(aabb.GetMin().x, aabb.GetMin().y, aabb.GetMax().z),
            Vec3(aabb.GetMax().x, aabb.GetMin().y, aabb.GetMax().z),
            Vec3(aabb.GetMax().x, aabb.GetMax().y, aabb.GetMax().z),
            Vec3(aabb.GetMin().x, aabb.GetMax().y, aabb.GetMax().z)};

        constexpr uint32 edges[12][2] = {
            {0u, 1u}, {1u, 2u}, {2u, 3u}, {3u, 0u},
            {4u, 5u}, {5u, 6u}, {6u, 7u}, {7u, 4u},
            {0u, 4u}, {1u, 5u}, {2u, 6u}, {3u, 7u}};

        for (const auto& edge : edges)
        {
            AppendDebugLine(corners[edge[0]], corners[edge[1]], color, lines, colors);
        }
    }

    Vec4 GetDebugContactColor(const CollisionEvent& event)
    {
        return event.isTrigger
            ? Vec4(1.0f, 0.75f, 0.15f, 1.0f)
            : Vec4(1.0f, 0.2f, 0.2f, 1.0f);
    }

    CollisionEvent MakeCollisionEvent(const RigidBody& bodyA, const RigidBody& bodyB)
    {
        CollisionEvent event;
        event.bodyIdA = bodyA.GetId();
        event.bodyIdB = bodyB.GetId();
        event.isTrigger = bodyA.IsTrigger() || bodyB.IsTrigger();

        Vec3 normal = bodyB.GetPosition() - bodyA.GetPosition();
        const float normalLengthSq = dot(normal, normal);
        if (normalLengthSq > 0.000001f)
        {
            normal /= std::sqrt(normalLengthSq);
        }
        else
        {
            normal = Vec3(0.0f, 1.0f, 0.0f);
        }

        const RigidBody::AABB aabbA = bodyA.GetAABB();
        const RigidBody::AABB aabbB = bodyB.GetAABB();
        const Vec3 overlapMin = glm::max(aabbA.min, aabbB.min);
        const Vec3 overlapMax = glm::min(aabbA.max, aabbB.max);
        if (overlapMin.x <= overlapMax.x &&
            overlapMin.y <= overlapMax.y &&
            overlapMin.z <= overlapMax.z)
        {
            event.contactPoint = (overlapMin + overlapMax) * 0.5f;
        }
        else
        {
            event.contactPoint = (bodyA.GetPosition() + bodyB.GetPosition()) * 0.5f;
        }

        event.contactNormal = normal;
        event.impulse = 0.0f;
        return event;
    }

    bool IsFiniteVec3(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool TryNormalizeQueryDirection(const Vec3& origin,
                                    const Vec3& direction,
                                    float maxDistance,
                                    Vec3& normalizedDirection)
    {
        if (!IsFiniteVec3(origin) || !IsFiniteVec3(direction) ||
            !std::isfinite(maxDistance) || maxDistance <= 0.0f)
        {
            return false;
        }

        const float directionLengthSq = dot(direction, direction);
        if (!std::isfinite(directionLengthSq) || directionLengthSq <= 0.000001f)
        {
            return false;
        }

        normalizedDirection = direction / std::sqrt(directionLengthSq);
        return IsFiniteVec3(normalizedDirection);
    }

    bool IsValidHalfExtents(const Vec3& halfExtents)
    {
        return IsFiniteVec3(halfExtents) &&
               halfExtents.x >= 0.0f &&
               halfExtents.y >= 0.0f &&
               halfExtents.z >= 0.0f;
    }

    AABB ToCoreAABB(const RigidBody::AABB& bounds)
    {
        return AABB(bounds.min, bounds.max);
    }

    bool IsFiniteAABB(const AABB& bounds)
    {
        return bounds.IsValid() &&
               IsFiniteVec3(bounds.GetMin()) &&
               IsFiniteVec3(bounds.GetMax());
    }

    int GetLongestAxis(const AABB& bounds)
    {
        const Vec3 size = bounds.GetSize();
        if (size.y > size.x && size.y > size.z)
        {
            return 1;
        }
        if (size.z > size.x)
        {
            return 2;
        }
        return 0;
    }

    uint32 BuildQueryBroadphaseRecursive(std::vector<QueryBroadphasePrimitive>& primitives,
                                         std::vector<QueryBroadphaseNode>& nodes,
                                         uint32 start,
                                         uint32 end)
    {
        const uint32 nodeIndex = static_cast<uint32>(nodes.size());
        nodes.emplace_back();

        AABB bounds;
        for (uint32 primitiveIndex = start; primitiveIndex < end; ++primitiveIndex)
        {
            bounds.Expand(primitives[primitiveIndex].bounds);
        }

        const uint32 primitiveCount = end - start;
        if (primitiveCount <= kQueryBroadphaseLeafSize)
        {
            QueryBroadphaseNode& node = nodes[nodeIndex];
            node.bounds = bounds;
            node.firstPrimitive = start;
            node.primitiveCount = primitiveCount;
            return nodeIndex;
        }

        const int splitAxis = GetLongestAxis(bounds);
        const uint32 mid = start + primitiveCount / 2u;
        std::nth_element(primitives.begin() + start,
                         primitives.begin() + mid,
                         primitives.begin() + end,
                         [splitAxis](const QueryBroadphasePrimitive& lhs,
                                     const QueryBroadphasePrimitive& rhs) {
                             return lhs.bounds.GetCenter()[splitAxis] < rhs.bounds.GetCenter()[splitAxis];
                         });

        const uint32 leftChild = BuildQueryBroadphaseRecursive(primitives, nodes, start, mid);
        const uint32 rightChild = BuildQueryBroadphaseRecursive(primitives, nodes, mid, end);

        QueryBroadphaseNode& node = nodes[nodeIndex];
        node.bounds = bounds;
        node.leftChild = leftChild;
        node.rightChild = rightChild;
        return nodeIndex;
    }

    void BuildQueryBroadphase(const std::vector<std::shared_ptr<RigidBody>>& bodies,
                              std::vector<QueryBroadphasePrimitive>& primitives,
                              std::vector<QueryBroadphaseNode>& nodes)
    {
        primitives.clear();
        nodes.clear();
        primitives.reserve(bodies.size());

        for (size_t bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex)
        {
            const auto& body = bodies[bodyIndex];
            if (!body)
            {
                continue;
            }

            const AABB bounds = ToCoreAABB(body->GetAABB());
            if (!IsFiniteAABB(bounds))
            {
                continue;
            }

            QueryBroadphasePrimitive primitive;
            primitive.bounds = bounds;
            primitive.bodyIndex = bodyIndex;
            primitives.push_back(primitive);
        }

        if (!primitives.empty())
        {
            nodes.reserve(primitives.size() * 2u);
            BuildQueryBroadphaseRecursive(primitives, nodes, 0u, static_cast<uint32>(primitives.size()));
        }
    }

    void CollectRayBroadphaseCandidates(const std::vector<QueryBroadphasePrimitive>& primitives,
                                        const std::vector<QueryBroadphaseNode>& nodes,
                                        const Ray& ray,
                                        std::vector<size_t>& bodyIndices,
                                        PhysicsWorld::QueryStats& stats)
    {
        bodyIndices.clear();
        if (nodes.empty())
        {
            return;
        }

        std::vector<uint32> stack;
        stack.push_back(0u);

        while (!stack.empty())
        {
            const uint32 nodeIndex = stack.back();
            stack.pop_back();

            const QueryBroadphaseNode& node = nodes[nodeIndex];
            ++stats.broadphaseNodeVisits;

            float nodeTMin = 0.0f;
            float nodeTMax = 0.0f;
            if (!RayAABBIntersect(ray, node.bounds, nodeTMin, nodeTMax))
            {
                continue;
            }

            if (node.IsLeaf())
            {
                const uint32 end = node.firstPrimitive + node.primitiveCount;
                for (uint32 primitiveIndex = node.firstPrimitive; primitiveIndex < end; ++primitiveIndex)
                {
                    float primitiveTMin = 0.0f;
                    float primitiveTMax = 0.0f;
                    if (RayAABBIntersect(ray, primitives[primitiveIndex].bounds, primitiveTMin, primitiveTMax))
                    {
                        bodyIndices.push_back(primitives[primitiveIndex].bodyIndex);
                        ++stats.broadphaseCandidateCount;
                    }
                }
                continue;
            }

            stack.push_back(node.leftChild);
            stack.push_back(node.rightChild);
        }
    }

    void CollectAABBBroadphaseCandidates(const std::vector<QueryBroadphasePrimitive>& primitives,
                                         const std::vector<QueryBroadphaseNode>& nodes,
                                         const AABB& queryBounds,
                                         std::vector<size_t>& bodyIndices,
                                         PhysicsWorld::QueryStats& stats)
    {
        bodyIndices.clear();
        if (nodes.empty() || !queryBounds.IsValid())
        {
            return;
        }

        std::vector<uint32> stack;
        stack.push_back(0u);

        while (!stack.empty())
        {
            const uint32 nodeIndex = stack.back();
            stack.pop_back();

            const QueryBroadphaseNode& node = nodes[nodeIndex];
            ++stats.broadphaseNodeVisits;

            if (!node.bounds.Overlaps(queryBounds))
            {
                continue;
            }

            if (node.IsLeaf())
            {
                const uint32 end = node.firstPrimitive + node.primitiveCount;
                for (uint32 primitiveIndex = node.firstPrimitive; primitiveIndex < end; ++primitiveIndex)
                {
                    if (primitives[primitiveIndex].bounds.Overlaps(queryBounds))
                    {
                        bodyIndices.push_back(primitives[primitiveIndex].bodyIndex);
                        ++stats.broadphaseCandidateCount;
                    }
                }
                continue;
            }

            stack.push_back(node.leftChild);
            stack.push_back(node.rightChild);
        }
    }

    bool TryGetRotationAxes(const Quat& rotation, Vec3 (&axes)[3])
    {
        if (!std::isfinite(rotation.w) ||
            !std::isfinite(rotation.x) ||
            !std::isfinite(rotation.y) ||
            !std::isfinite(rotation.z))
        {
            return false;
        }

        const float rotationLengthSq = rotation.w * rotation.w +
                                       rotation.x * rotation.x +
                                       rotation.y * rotation.y +
                                       rotation.z * rotation.z;
        if (rotationLengthSq <= 0.000001f)
        {
            return false;
        }

        const Mat3 rotationMatrix = mat3_cast(normalize(rotation));
        axes[0] = Vec3(rotationMatrix[0]);
        axes[1] = Vec3(rotationMatrix[1]);
        axes[2] = Vec3(rotationMatrix[2]);
        return IsFiniteVec3(axes[0]) && IsFiniteVec3(axes[1]) && IsFiniteVec3(axes[2]);
    }

    Vec3 CalculateRotatedHalfExtents(const Vec3& halfExtents, const Vec3 (&axes)[3])
    {
        return Vec3(
            std::abs(axes[0].x) * halfExtents.x +
                std::abs(axes[1].x) * halfExtents.y +
                std::abs(axes[2].x) * halfExtents.z,
            std::abs(axes[0].y) * halfExtents.x +
                std::abs(axes[1].y) * halfExtents.y +
                std::abs(axes[2].y) * halfExtents.z,
            std::abs(axes[0].z) * halfExtents.x +
                std::abs(axes[1].z) * halfExtents.y +
                std::abs(axes[2].z) * halfExtents.z);
    }

    bool OverlapsOnAxis(const Vec3& axis,
                        const Vec3& centerDelta,
                        const Vec3& aabbHalfExtents,
                        const Vec3& boxHalfExtents,
                        const Vec3 (&boxAxes)[3])
    {
        if (dot(axis, axis) <= 0.000001f)
        {
            return true;
        }

        const float distance = std::abs(dot(centerDelta, axis));
        const float aabbRadius = aabbHalfExtents.x * std::abs(axis.x) +
                                 aabbHalfExtents.y * std::abs(axis.y) +
                                 aabbHalfExtents.z * std::abs(axis.z);
        const float boxRadius = boxHalfExtents.x * std::abs(dot(axis, boxAxes[0])) +
                                boxHalfExtents.y * std::abs(dot(axis, boxAxes[1])) +
                                boxHalfExtents.z * std::abs(dot(axis, boxAxes[2]));
        return distance <= aabbRadius + boxRadius;
    }

    bool OrientedBoxOverlapsAABB(const Vec3& boxCenter,
                                 const Vec3& boxHalfExtents,
                                 const Vec3 (&boxAxes)[3],
                                 const AABB& aabb)
    {
        const Vec3 aabbCenter = aabb.GetCenter();
        const Vec3 aabbHalfExtents = aabb.GetExtent();
        const Vec3 centerDelta = boxCenter - aabbCenter;
        const Vec3 worldAxes[3] = {
            Vec3(1.0f, 0.0f, 0.0f),
            Vec3(0.0f, 1.0f, 0.0f),
            Vec3(0.0f, 0.0f, 1.0f)};

        for (const Vec3& axis : worldAxes)
        {
            if (!OverlapsOnAxis(axis, centerDelta, aabbHalfExtents, boxHalfExtents, boxAxes))
            {
                return false;
            }
        }

        for (const Vec3& axis : boxAxes)
        {
            if (!OverlapsOnAxis(axis, centerDelta, aabbHalfExtents, boxHalfExtents, boxAxes))
            {
                return false;
            }
        }

        for (const Vec3& worldAxis : worldAxes)
        {
            for (const Vec3& boxAxis : boxAxes)
            {
                if (!OverlapsOnAxis(cross(worldAxis, boxAxis),
                                    centerDelta,
                                    aabbHalfExtents,
                                    boxHalfExtents,
                                    boxAxes))
                {
                    return false;
                }
            }
        }

        return true;
    }

    Vec3 ApplyVelocityConstraints(Vec3 velocity, uint8 constraints)
    {
        if ((constraints & 1u) != 0u)
        {
            velocity.x = 0.0f;
        }
        if ((constraints & 2u) != 0u)
        {
            velocity.y = 0.0f;
        }
        if ((constraints & 4u) != 0u)
        {
            velocity.z = 0.0f;
        }
        return velocity;
    }

    Vec3 ApplyPositionConstraints(Vec3 previous, Vec3 next, uint8 constraints)
    {
        if ((constraints & 1u) != 0u)
        {
            next.x = previous.x;
        }
        if ((constraints & 2u) != 0u)
        {
            next.y = previous.y;
        }
        if ((constraints & 4u) != 0u)
        {
            next.z = previous.z;
        }
        return next;
    }
} // namespace

PhysicsWorld::~PhysicsWorld()
{
    Shutdown();
}

bool PhysicsWorld::Initialize(const PhysicsWorldConfig& config)
{
    if (m_initialized)
    {
        Shutdown();
    }

    m_config = config;
    if (!std::isfinite(m_config.fixedTimeStep) || m_config.fixedTimeStep <= 0.0f)
    {
        m_config.fixedTimeStep = 1.0f / 60.0f;
    }
    if (m_config.maxSubSteps == 0)
    {
        m_config.maxSubSteps = 1;
    }
    if (!std::isfinite(m_config.sleepVelocityThreshold) || m_config.sleepVelocityThreshold < 0.0f)
    {
        m_config.sleepVelocityThreshold = 0.1f;
    }
    if (!std::isfinite(m_config.sleepTimeThreshold) || m_config.sleepTimeThreshold < 0.0f)
    {
        m_config.sleepTimeThreshold = 0.5f;
    }
    m_accumulatedTime = 0.0f;
    m_lastStepCount = 0;

    m_requestedBackend = m_config.backend;
    m_activeBackend = ResolveBackendType(m_requestedBackend);
    m_backendFallbackActive = m_requestedBackend != PhysicsBackendType::Auto &&
                              m_requestedBackend != m_activeBackend;

    m_backend = PhysicsBackendFactory::Create(m_activeBackend);
    if (!m_backend)
    {
        m_backend = PhysicsBackendFactory::Create(PhysicsBackendType::BuiltIn);
        m_activeBackend = PhysicsBackendType::BuiltIn;
        m_backendFallbackActive = m_requestedBackend != PhysicsBackendType::Auto &&
                                  m_requestedBackend != PhysicsBackendType::BuiltIn;
    }

    if (!m_backend || !m_backend->Initialize(ToBackendConfig(m_config)))
    {
        m_backend.reset();
        m_initialized = false;
        return false;
    }

    m_activeBackend = m_backend->GetType();

    m_initialized = true;
    return true;
}

void PhysicsWorld::Shutdown()
{
    m_bodies.clear();
    m_bodyLookup.clear();
    m_constraints.clear();
    m_activeCollisionPairs.clear();
    m_lastQueryStats = QueryStats{};
    if (m_backend)
    {
        m_backend->Shutdown();
        m_backend.reset();
    }
    m_requestedBackend = PhysicsBackendType::Auto;
    m_activeBackend = PhysicsBackendType::BuiltIn;
    m_backendFallbackActive = false;
    m_initialized = false;
}

void PhysicsWorld::Step(float deltaTime)
{
    m_lastStepCount = 0;

    if (!m_initialized || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
    {
        return;
    }

    const float fixedTimeStep = m_config.fixedTimeStep;
    const float maxAccumulatedTime = fixedTimeStep * static_cast<float>(m_config.maxSubSteps);
    m_accumulatedTime = std::min(m_accumulatedTime + deltaTime, maxAccumulatedTime);

    // Fixed timestep simulation
    while (m_accumulatedTime >= fixedTimeStep && m_lastStepCount < m_config.maxSubSteps)
    {
        StepInternal(fixedTimeStep);
        m_accumulatedTime -= fixedTimeStep;
        ++m_lastStepCount;
    }
}

void PhysicsWorld::StepInternal(float dt)
{
    if (m_activeBackend != PhysicsBackendType::BuiltIn)
    {
        if (m_backend)
        {
            m_backend->Step(dt);
        }
        return;
    }

    // Built-in physics step
    
    // 1. Apply forces and integrate velocities
    for (auto& body : m_bodies)
    {
        if (!body || !body->IsDynamic() || body->IsSleeping())
            continue;

        // Apply gravity
        Vec3 gravity = m_config.gravity * body->GetGravityScale();
        Vec3 velocity = body->GetLinearVelocity();
        velocity += gravity * dt;
        velocity += body->GetAccumulatedForce() * body->GetInverseMass() * dt;
        // Apply damping
        float linearDamping = 1.0f - body->GetLinearDamping() * dt;
        float angularDamping = 1.0f - body->GetAngularDamping() * dt;
        linearDamping = std::max(0.0f, linearDamping);
        angularDamping = std::max(0.0f, angularDamping);
        velocity *= linearDamping;
        Vec3 angularVelocity = body->GetAngularVelocity();
        angularVelocity += body->GetAccumulatedTorque() * body->GetInverseMass() * dt;
        angularVelocity *= angularDamping;
        velocity = ApplyVelocityConstraints(velocity, body->GetPositionConstraints());
        angularVelocity = ApplyVelocityConstraints(angularVelocity, body->GetRotationConstraints());

        body->SetLinearVelocity(velocity);
        body->SetAngularVelocity(angularVelocity);
    }

    // 2. Solve constraints
    for (auto& constraint : m_constraints)
    {
        if (constraint && constraint->IsEnabled() && !constraint->IsBroken())
        {
            constraint->PreSolve(dt);
        }
    }

    for (int iter = 0; iter < m_config.velocitySteps; ++iter)
    {
        for (auto& constraint : m_constraints)
        {
            if (constraint && constraint->IsEnabled() && !constraint->IsBroken())
            {
                constraint->SolveVelocity(dt);
            }
        }
    }

    // 3. Integrate positions
    for (auto& body : m_bodies)
    {
        if (!body || !body->IsDynamic() || body->IsSleeping())
            continue;

        Vec3 position = body->GetPosition();
        Quat rotation = body->GetRotation();

        const Vec3 displacement = body->GetLinearVelocity() * dt;
        Vec3 nextPosition = position + displacement;
        if (body->GetMotionQuality() == MotionQuality::LinearCast)
        {
            float hitFraction = 1.0f;
            Vec3 hitNormal{0.0f, 1.0f, 0.0f};
            if (TryLinearCastBody(*body, displacement, hitFraction, hitNormal))
            {
                constexpr float linearCastSlop = 0.0001f;
                nextPosition = position + displacement * std::max(0.0f, hitFraction - linearCastSlop);

                const Vec3 velocity = body->GetLinearVelocity();
                const float normalVelocity = dot(velocity, hitNormal);
                if (normalVelocity < 0.0f)
                {
                    body->SetLinearVelocity(velocity - hitNormal * normalVelocity);
                }
            }
        }
        position = ApplyPositionConstraints(body->GetPosition(), nextPosition, body->GetPositionConstraints());

        Vec3 angularVel = body->GetAngularVelocity();
        if (length(angularVel) > 0.0001f)
        {
            Quat wQuat(0.0f, angularVel.x, angularVel.y, angularVel.z);
            Quat dq = wQuat * rotation * 0.5f * dt;
            rotation = normalize(rotation + dq);
        }
        
        body->SetPosition(position);
        body->SetRotation(rotation);
        body->ClearForces();
    }

    // 4. Resolve built-in contacts
    ResolveBodyCollisions();

    // 5. Solve position constraints
    for (int iter = 0; iter < m_config.positionSteps; ++iter)
    {
        for (auto& constraint : m_constraints)
        {
            if (constraint && constraint->IsEnabled() && !constraint->IsBroken())
            {
                constraint->SolvePosition(dt);
            }
        }
    }
    UpdateConstraintBreakage(dt);

    // 6. Check for sleeping bodies
    UpdateCollisionEvents();
    UpdateSleepStates(dt);
}

void PhysicsWorld::UpdateSleepStates(float deltaTime)
{
    for (auto& body : m_bodies)
    {
        if (!body || !body->IsDynamic())
        {
            continue;
        }

        if (!body->CanSleep())
        {
            body->ResetSleepTimer();
            continue;
        }

        float linearSpeed = length(body->GetLinearVelocity());
        float angularSpeed = length(body->GetAngularVelocity());

        if (linearSpeed < m_config.sleepVelocityThreshold &&
            angularSpeed < m_config.sleepVelocityThreshold)
        {
            body->AccumulateSleepTime(deltaTime);
            if (body->GetSleepTimer() >= m_config.sleepTimeThreshold)
            {
                body->SetSleeping(true);
            }
        }
        else
        {
            body->ResetSleepTimer();
        }
    }
}

void PhysicsWorld::ResolveBodyCollisions()
{
    uint32 processedBodyPairs = 0;
    uint32 resolvedContacts = 0;

    for (size_t i = 0; i < m_bodies.size(); ++i)
    {
        auto& bodyA = m_bodies[i];
        if (!bodyA)
        {
            continue;
        }

        for (size_t j = i + 1; j < m_bodies.size(); ++j)
        {
            auto& bodyB = m_bodies[j];
            if (!bodyB)
            {
                continue;
            }

            if (!BodiesCanCollide(*bodyA, *bodyB))
            {
                continue;
            }

            if (processedBodyPairs >= m_config.maxBodyPairs)
            {
                return;
            }
            ++processedBodyPairs;

            AABBContact contact;
            if (TryBuildAABBContact(*bodyA, *bodyB, contact))
            {
                if (resolvedContacts >= m_config.maxContactConstraints)
                {
                    return;
                }
                ResolveAABBContact(*bodyA, *bodyB, contact);
                ++resolvedContacts;
            }
        }
    }
}

bool PhysicsWorld::TryLinearCastBody(const RigidBody& movingBody,
                                     const Vec3& displacement,
                                     float& hitFraction,
                                     Vec3& hitNormal) const
{
    if (!movingBody.IsDynamic() ||
        movingBody.IsTrigger() ||
        movingBody.GetShapeCount() == 0 ||
        dot(displacement, displacement) <= 0.000001f)
    {
        return false;
    }

    bool foundHit = false;
    hitFraction = 1.0f;
    hitNormal = Vec3(0.0f, 1.0f, 0.0f);

    const RigidBody::AABB movingAABB = movingBody.GetAABB();
    for (const auto& otherBody : m_bodies)
    {
        if (!otherBody ||
            otherBody->GetId() == movingBody.GetId() ||
            otherBody->IsTrigger() ||
            !BodiesCanCollide(movingBody, *otherBody))
        {
            continue;
        }

        LinearCastHit candidateHit;
        if (TrySweepAABBAgainstAABB(movingAABB, displacement, otherBody->GetAABB(), candidateHit) &&
            candidateHit.fraction < hitFraction)
        {
            foundHit = true;
            hitFraction = candidateHit.fraction;
            hitNormal = candidateHit.normal;
        }
    }

    return foundHit;
}

void PhysicsWorld::UpdateConstraintBreakage(float deltaTime)
{
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
    {
        return;
    }

    for (auto& constraint : m_constraints)
    {
        if (!constraint || !constraint->IsEnabled() || constraint->IsBroken())
        {
            continue;
        }

        const float breakingForce = constraint->GetBreakingForce();
        if (!std::isfinite(breakingForce) || breakingForce <= 0.0f)
        {
            continue;
        }

        const float appliedImpulse = std::abs(constraint->GetAppliedImpulse());
        if (std::isfinite(appliedImpulse) && appliedImpulse / deltaTime > breakingForce)
        {
            constraint->Break();
        }
    }
}

void PhysicsWorld::SetGravity(const Vec3& gravity)
{
    m_config.gravity = gravity;
    if (m_backend)
    {
        m_backend->SetGravity(gravity);
    }
}

const char* PhysicsWorld::GetActiveBackendName() const
{
    return m_backend ? m_backend->GetName() : "None";
}

BodyHandle PhysicsWorld::CreateBody(const RigidBodyDesc& desc)
{
    if (!m_initialized || m_bodies.size() >= m_config.maxBodies)
    {
        return BodyHandle{};
    }

    auto body = std::make_shared<RigidBody>(desc);
    uint64 id = m_nextBodyId++;
    body->SetId(id);

    m_bodyLookup[id] = m_bodies.size();
    m_bodies.push_back(std::move(body));

    return BodyHandle(id);
}

void PhysicsWorld::DestroyBody(BodyHandle handle)
{
    auto it = m_bodyLookup.find(handle.GetId());
    if (it == m_bodyLookup.end()) return;

    size_t index = it->second;
    RemoveActivePairsForBody(handle.GetId());

    // Swap and pop
    if (index != m_bodies.size() - 1)
    {
        m_bodies[index] = std::move(m_bodies.back());
        m_bodyLookup[m_bodies[index]->GetId()] = index;
    }
    m_bodies.pop_back();
    m_bodyLookup.erase(it);
}

RigidBody* PhysicsWorld::GetBody(BodyHandle handle)
{
    auto it = m_bodyLookup.find(handle.GetId());
    if (it == m_bodyLookup.end()) return nullptr;
    return m_bodies[it->second].get();
}

const RigidBody* PhysicsWorld::GetBody(BodyHandle handle) const
{
    auto it = m_bodyLookup.find(handle.GetId());
    if (it == m_bodyLookup.end()) return nullptr;
    return m_bodies[it->second].get();
}

std::shared_ptr<RigidBody> PhysicsWorld::GetBodyRef(BodyHandle handle) const
{
    auto it = m_bodyLookup.find(handle.GetId());
    if (it == m_bodyLookup.end()) return {};
    return m_bodies[it->second];
}

void PhysicsWorld::SetBodyPosition(BodyHandle body, const Vec3& position)
{
    if (auto* b = GetBody(body)) b->SetPosition(position);
}

Vec3 PhysicsWorld::GetBodyPosition(BodyHandle body) const
{
    if (auto* b = GetBody(body)) return b->GetPosition();
    return Vec3(0.0f);
}

void PhysicsWorld::SetBodyRotation(BodyHandle body, const Quat& rotation)
{
    if (auto* b = GetBody(body)) b->SetRotation(rotation);
}

Quat PhysicsWorld::GetBodyRotation(BodyHandle body) const
{
    if (auto* b = GetBody(body)) return b->GetRotation();
    return Quat(1, 0, 0, 0);
}

void PhysicsWorld::SetBodyVelocity(BodyHandle body, const Vec3& velocity)
{
    if (auto* b = GetBody(body)) b->SetLinearVelocity(velocity);
}

Vec3 PhysicsWorld::GetBodyVelocity(BodyHandle body) const
{
    if (auto* b = GetBody(body)) return b->GetLinearVelocity();
    return Vec3(0.0f);
}

void PhysicsWorld::SetBodyAngularVelocity(BodyHandle body, const Vec3& angularVelocity)
{
    if (auto* b = GetBody(body)) b->SetAngularVelocity(angularVelocity);
}

Vec3 PhysicsWorld::GetBodyAngularVelocity(BodyHandle body) const
{
    if (auto* b = GetBody(body)) return b->GetAngularVelocity();
    return Vec3(0.0f);
}

void PhysicsWorld::ApplyForce(BodyHandle body, const Vec3& force)
{
    if (auto* b = GetBody(body)) b->ApplyForce(force);
}

void PhysicsWorld::ApplyImpulse(BodyHandle body, const Vec3& impulse)
{
    if (auto* b = GetBody(body)) b->ApplyImpulse(impulse);
}

void PhysicsWorld::ApplyTorque(BodyHandle body, const Vec3& torque)
{
    if (auto* b = GetBody(body)) b->ApplyTorque(torque);
}

void PhysicsWorld::AddShape(BodyHandle body, std::shared_ptr<CollisionShape> shape,
                            const Vec3& offset, const Quat& rotation)
{
    if (auto* b = GetBody(body))
    {
        b->AddShape(std::move(shape), offset, rotation);
    }
}

uint64 PhysicsWorld::CreateConstraint(std::shared_ptr<Constraint> constraint)
{
    if (!constraint)
    {
        return 0;
    }

    uint64 id = m_nextConstraintId++;
    constraint->SetId(id);
    m_constraints.push_back(std::move(constraint));
    return id;
}

void PhysicsWorld::DestroyConstraint(uint64 constraintId)
{
    if (constraintId == 0)
    {
        return;
    }

    auto removeBegin = std::remove_if(m_constraints.begin(), m_constraints.end(),
        [constraintId](const std::shared_ptr<Constraint>& constraint) {
            if (!constraint || constraint->GetId() != constraintId)
            {
                return false;
            }

            constraint->SetId(0);
            return true;
        });
    m_constraints.erase(removeBegin, m_constraints.end());
}

void PhysicsWorld::UpdateCollisionEvents()
{
    struct PendingEvent
    {
        CollisionEvent event;
        bool isTrigger = false;
        bool isEnter = false;
    };

    std::unordered_set<BodyPairKey, BodyPairKeyHash> currentPairs;
    std::vector<PendingEvent> pendingEvents;

    auto makeKey = [](uint64 bodyIdA, uint64 bodyIdB) {
        if (bodyIdA > bodyIdB)
        {
            std::swap(bodyIdA, bodyIdB);
        }
        return BodyPairKey{bodyIdA, bodyIdB};
    };

    uint32 processedBodyPairs = 0;

    for (size_t i = 0; i < m_bodies.size(); ++i)
    {
        const auto& bodyA = m_bodies[i];
        if (!bodyA)
        {
            continue;
        }

        for (size_t j = i + 1; j < m_bodies.size(); ++j)
        {
            const auto& bodyB = m_bodies[j];
            if (!bodyB)
            {
                continue;
            }

            if (!BodiesCanCollide(*bodyA, *bodyB) || !BodyAABBsOverlap(*bodyA, *bodyB))
            {
                continue;
            }

            if (processedBodyPairs >= m_config.maxBodyPairs)
            {
                break;
            }
            ++processedBodyPairs;

            const BodyPairKey key = makeKey(bodyA->GetId(), bodyB->GetId());
            currentPairs.insert(key);
            if (m_activeCollisionPairs.find(key) == m_activeCollisionPairs.end())
            {
                const CollisionEvent event = MakeCollisionEvent(*bodyA, *bodyB);
                pendingEvents.push_back({event, event.isTrigger, true});
            }
        }
    }

    for (const BodyPairKey& key : m_activeCollisionPairs)
    {
        if (currentPairs.find(key) != currentPairs.end())
        {
            continue;
        }

        const RigidBody* bodyA = GetBody(BodyHandle(key.bodyA));
        const RigidBody* bodyB = GetBody(BodyHandle(key.bodyB));
        if (!bodyA || !bodyB)
        {
            continue;
        }

        const CollisionEvent event = MakeCollisionEvent(*bodyA, *bodyB);
        pendingEvents.push_back({event, event.isTrigger, false});
    }

    m_activeCollisionPairs = std::move(currentPairs);

    for (const PendingEvent& pendingEvent : pendingEvents)
    {
        if (pendingEvent.isTrigger)
        {
            const CollisionCallback& callback = pendingEvent.isEnter ? m_onTriggerEnter : m_onTriggerExit;
            if (callback)
            {
                callback(pendingEvent.event);
            }
        }
        else
        {
            const CollisionCallback& callback = pendingEvent.isEnter ? m_onCollisionEnter : m_onCollisionExit;
            if (callback)
            {
                callback(pendingEvent.event);
            }
        }
    }
}

void PhysicsWorld::RemoveActivePairsForBody(uint64 bodyId)
{
    for (auto it = m_activeCollisionPairs.begin(); it != m_activeCollisionPairs.end();)
    {
        if (it->bodyA == bodyId || it->bodyB == bodyId)
        {
            it = m_activeCollisionPairs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool PhysicsWorld::Raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                           RaycastHit& hit, uint32 layerMask) const
{
    m_lastQueryStats = QueryStats{};
    hit = RaycastHit{};
    Vec3 dir(0.0f);
    if (!TryNormalizeQueryDirection(origin, direction, maxDistance, dir))
    {
        return false;
    }

    float closestDist = maxDistance;
    bool foundHit = false;
    const Ray ray(origin, dir, 0.0f, maxDistance);

    std::vector<QueryBroadphasePrimitive> broadphasePrimitives;
    std::vector<QueryBroadphaseNode> broadphaseNodes;
    std::vector<size_t> candidateBodyIndices;
    BuildQueryBroadphase(m_bodies, broadphasePrimitives, broadphaseNodes);
    CollectRayBroadphaseCandidates(broadphasePrimitives,
                                   broadphaseNodes,
                                   ray,
                                   candidateBodyIndices,
                                   m_lastQueryStats);

    for (size_t bodyIndex : candidateBodyIndices)
    {
        if (bodyIndex >= m_bodies.size()) continue;
        auto& body = m_bodies[bodyIndex];
        if (!body) continue;

        // Check layer mask
        if (!QueryCanHitBody(*body, layerMask)) continue;
        ++m_lastQueryStats.narrowphaseTestCount;

        // Get body AABB
        RigidBody::AABB aabb = body->GetAABB();
        AABB box(aabb.min, aabb.max);

        // Ray-AABB intersection test
        float tMin = 0.0f, tMax = 0.0f;
        if (RayAABBIntersect(ray, box, tMin, tMax))
        {
            if (tMin <= closestDist && tMin >= 0.0f)
            {
                closestDist = tMin;
                foundHit = true;
                hit.hit = true;
                hit.distance = tMin;
                hit.point = origin + dir * tMin;
                hit.bodyId = body->GetId();
                // Simplified normal calculation - point towards ray origin
                hit.normal = -dir;
            }
        }
    }

    return foundHit;
}

size_t PhysicsWorld::RaycastAll(const Vec3& origin, const Vec3& direction, float maxDistance,
                                std::vector<RaycastHit>& hits, uint32 layerMask) const
{
    m_lastQueryStats = QueryStats{};
    hits.clear();

    Vec3 dir(0.0f);
    if (!TryNormalizeQueryDirection(origin, direction, maxDistance, dir))
    {
        return 0;
    }

    const Ray ray(origin, dir, 0.0f, maxDistance);
    std::vector<QueryBroadphasePrimitive> broadphasePrimitives;
    std::vector<QueryBroadphaseNode> broadphaseNodes;
    std::vector<size_t> candidateBodyIndices;
    BuildQueryBroadphase(m_bodies, broadphasePrimitives, broadphaseNodes);
    CollectRayBroadphaseCandidates(broadphasePrimitives,
                                   broadphaseNodes,
                                   ray,
                                   candidateBodyIndices,
                                   m_lastQueryStats);

    for (size_t bodyIndex : candidateBodyIndices)
    {
        if (bodyIndex >= m_bodies.size()) continue;
        auto& body = m_bodies[bodyIndex];
        if (!body) continue;
        if (!QueryCanHitBody(*body, layerMask)) continue;
        ++m_lastQueryStats.narrowphaseTestCount;

        RigidBody::AABB aabb = body->GetAABB();
        AABB box(aabb.min, aabb.max);

        float tMin = 0.0f, tMax = 0.0f;
        if (RayAABBIntersect(ray, box, tMin, tMax))
        {
            if (tMin <= maxDistance && tMin >= 0.0f)
            {
                RaycastHit hit;
                hit.hit = true;
                hit.distance = tMin;
                hit.point = origin + dir * tMin;
                hit.bodyId = body->GetId();
                hit.normal = -dir;
                hits.push_back(hit);
            }
        }
    }

    // Sort by distance
    std::sort(hits.begin(), hits.end(), [](const RaycastHit& a, const RaycastHit& b) {
        return a.distance < b.distance;
    });

    return hits.size();
}

bool PhysicsWorld::SphereCast(const Vec3& origin, float radius, const Vec3& direction,
                              float maxDistance, ShapeCastHit& hit, uint32 layerMask) const
{
    m_lastQueryStats = QueryStats{};
    hit = ShapeCastHit{};
    Vec3 dir(0.0f);
    if (!std::isfinite(radius) || radius < 0.0f ||
        !TryNormalizeQueryDirection(origin, direction, maxDistance, dir))
    {
        return false;
    }

    float closestDist = maxDistance;
    bool foundHit = false;
    const Vec3 end = origin + dir * maxDistance;
    const AABB sweptBounds(glm::min(origin, end) - Vec3(radius),
                           glm::max(origin, end) + Vec3(radius));
    const Ray ray(origin, dir, 0.0f, maxDistance);

    std::vector<QueryBroadphasePrimitive> broadphasePrimitives;
    std::vector<QueryBroadphaseNode> broadphaseNodes;
    std::vector<size_t> candidateBodyIndices;
    BuildQueryBroadphase(m_bodies, broadphasePrimitives, broadphaseNodes);
    CollectAABBBroadphaseCandidates(broadphasePrimitives,
                                    broadphaseNodes,
                                    sweptBounds,
                                    candidateBodyIndices,
                                    m_lastQueryStats);

    for (size_t bodyIndex : candidateBodyIndices)
    {
        if (bodyIndex >= m_bodies.size()) continue;
        auto& body = m_bodies[bodyIndex];
        if (!body) continue;
        if (!QueryCanHitBody(*body, layerMask)) continue;
        ++m_lastQueryStats.narrowphaseTestCount;

        RigidBody::AABB bodyAABB = body->GetAABB();

        // Expand AABB by radius
        Vec3 expandedMin = bodyAABB.min - Vec3(radius);
        Vec3 expandedMax = bodyAABB.max + Vec3(radius);
        AABB expandedBox(expandedMin, expandedMax);

        float tMin = 0.0f, tMax = 0.0f;
        if (RayAABBIntersect(ray, expandedBox, tMin, tMax))
        {
            if (tMin <= closestDist && tMin >= 0.0f)
            {
                closestDist = tMin;
                foundHit = true;
                hit.hit = true;
                hit.fraction = tMin / maxDistance;
                hit.point = origin + dir * tMin;
                hit.bodyId = body->GetId();
                hit.normal = -dir;
            }
        }
    }

    return foundHit;
}

size_t PhysicsWorld::OverlapSphere(const Vec3& center, float radius,
                                   std::vector<BodyHandle>& bodies, uint32 layerMask) const
{
    m_lastQueryStats = QueryStats{};
    bodies.clear();
    if (!IsFiniteVec3(center) || !std::isfinite(radius) || radius < 0.0f)
    {
        return 0;
    }

    const AABB queryBounds(center - Vec3(radius), center + Vec3(radius));
    std::vector<QueryBroadphasePrimitive> broadphasePrimitives;
    std::vector<QueryBroadphaseNode> broadphaseNodes;
    std::vector<size_t> candidateBodyIndices;
    BuildQueryBroadphase(m_bodies, broadphasePrimitives, broadphaseNodes);
    CollectAABBBroadphaseCandidates(broadphasePrimitives,
                                    broadphaseNodes,
                                    queryBounds,
                                    candidateBodyIndices,
                                    m_lastQueryStats);

    for (size_t bodyIndex : candidateBodyIndices)
    {
        if (bodyIndex >= m_bodies.size()) continue;
        auto& body = m_bodies[bodyIndex];
        if (!body) continue;
        if (!QueryCanHitBody(*body, layerMask)) continue;
        ++m_lastQueryStats.narrowphaseTestCount;

        RigidBody::AABB aabb = body->GetAABB();

        // Check if sphere intersects AABB
        Vec3 closestPoint = glm::clamp(center, aabb.min, aabb.max);
        float dist = glm::length(closestPoint - center);

        if (dist <= radius)
        {
            bodies.push_back(body->GetHandle());
        }
    }

    return bodies.size();
}

size_t PhysicsWorld::OverlapBox(const Vec3& center, const Vec3& halfExtents,
                                std::vector<BodyHandle>& bodies, uint32 layerMask) const
{
    return OverlapBox(center, halfExtents, Quat(1, 0, 0, 0), bodies, layerMask);
}

size_t PhysicsWorld::OverlapBox(const Vec3& center, const Vec3& halfExtents, const Quat& rotation,
                                std::vector<BodyHandle>& bodies, uint32 layerMask) const
{
    m_lastQueryStats = QueryStats{};
    bodies.clear();
    if (!IsFiniteVec3(center) || !IsValidHalfExtents(halfExtents))
    {
        return 0;
    }

    Vec3 boxAxes[3];
    if (!TryGetRotationAxes(rotation, boxAxes))
    {
        return 0;
    }

    const Vec3 extents = CalculateRotatedHalfExtents(halfExtents, boxAxes);
    const AABB queryBounds(center - extents, center + extents);

    std::vector<QueryBroadphasePrimitive> broadphasePrimitives;
    std::vector<QueryBroadphaseNode> broadphaseNodes;
    std::vector<size_t> candidateBodyIndices;
    BuildQueryBroadphase(m_bodies, broadphasePrimitives, broadphaseNodes);
    CollectAABBBroadphaseCandidates(broadphasePrimitives,
                                    broadphaseNodes,
                                    queryBounds,
                                    candidateBodyIndices,
                                    m_lastQueryStats);

    for (size_t bodyIndex : candidateBodyIndices)
    {
        if (bodyIndex >= m_bodies.size()) continue;
        auto& body = m_bodies[bodyIndex];
        if (!body) continue;
        if (!QueryCanHitBody(*body, layerMask)) continue;
        ++m_lastQueryStats.narrowphaseTestCount;

        const RigidBody::AABB bodyAABB = body->GetAABB();
        const AABB bodyBounds(bodyAABB.min, bodyAABB.max);
        if (queryBounds.Overlaps(bodyBounds) &&
            OrientedBoxOverlapsAABB(center, halfExtents, boxAxes, bodyBounds))
        {
            bodies.push_back(body->GetHandle());
        }
    }

    return bodies.size();
}

size_t PhysicsWorld::OverlapCapsule(const Vec3& pointA, const Vec3& pointB, float radius,
                                    std::vector<BodyHandle>& bodies, uint32 layerMask) const
{
    m_lastQueryStats = QueryStats{};
    bodies.clear();
    if (!IsFiniteVec3(pointA) || !IsFiniteVec3(pointB) || !std::isfinite(radius) || radius < 0.0f)
    {
        return 0;
    }

    const AABB capsuleBounds(glm::min(pointA, pointB) - Vec3(radius),
                             glm::max(pointA, pointB) + Vec3(radius));

    std::vector<QueryBroadphasePrimitive> broadphasePrimitives;
    std::vector<QueryBroadphaseNode> broadphaseNodes;
    std::vector<size_t> candidateBodyIndices;
    BuildQueryBroadphase(m_bodies, broadphasePrimitives, broadphaseNodes);
    CollectAABBBroadphaseCandidates(broadphasePrimitives,
                                    broadphaseNodes,
                                    capsuleBounds,
                                    candidateBodyIndices,
                                    m_lastQueryStats);

    for (size_t bodyIndex : candidateBodyIndices)
    {
        if (bodyIndex >= m_bodies.size()) continue;
        auto& body = m_bodies[bodyIndex];
        if (!body) continue;
        if (!QueryCanHitBody(*body, layerMask)) continue;
        ++m_lastQueryStats.narrowphaseTestCount;

        const RigidBody::AABB bodyAABB = body->GetAABB();
        const AABB bodyBounds(bodyAABB.min, bodyAABB.max);
        if (capsuleBounds.Overlaps(bodyBounds))
        {
            bodies.push_back(body->GetHandle());
        }
    }

    return bodies.size();
}

void PhysicsWorld::GetDebugDrawData(std::vector<Vec3>& lines, std::vector<Vec4>& colors) const
{
    GetDebugDrawData(lines, colors, DebugDrawOptions{});
}

void PhysicsWorld::GetDebugDrawData(std::vector<Vec3>& lines, std::vector<Vec4>& colors,
                                    const DebugDrawOptions& options) const
{
    lines.clear();
    colors.clear();

    if (!options.drawBodies &&
        !options.drawShapes &&
        !options.drawContacts &&
        !options.drawConstraints &&
        !options.drawBroadphase)
    {
        return;
    }

    lines.reserve(m_bodies.size() * 24u + m_constraints.size() * 2u);
    colors.reserve(m_bodies.size() * 24u + m_constraints.size() * 2u);

    if (options.drawBodies || options.drawShapes)
    {
        for (const auto& body : m_bodies)
        {
            if (!body)
            {
                continue;
            }

            AppendAABBDebugLines(body->GetAABB(), GetDebugBodyColor(*body), lines, colors);
        }
    }

    if (options.drawBroadphase)
    {
        const Vec4 color = GetDebugBroadphaseColor();
        std::vector<QueryBroadphasePrimitive> broadphasePrimitives;
        std::vector<QueryBroadphaseNode> broadphaseNodes;
        BuildQueryBroadphase(m_bodies, broadphasePrimitives, broadphaseNodes);
        for (const QueryBroadphaseNode& node : broadphaseNodes)
        {
            AppendAABBDebugLines(node.bounds, color, lines, colors);
        }
    }

    if (options.drawConstraints)
    {
        for (const auto& constraint : m_constraints)
        {
            if (!constraint || !constraint->IsEnabled() || constraint->IsBroken())
            {
                continue;
            }

            const Vec3 anchorA = GetConstraintAnchorWorld(constraint->GetBodyA(), constraint->GetAnchorA());
            const Vec3 anchorB = GetConstraintAnchorWorld(constraint->GetBodyB(), constraint->GetAnchorB());
            AppendDebugLine(anchorA, anchorB, GetDebugConstraintColor(*constraint), lines, colors);
        }
    }

    if (!options.drawContacts)
    {
        return;
    }

    constexpr float contactNormalLength = 0.5f;
    for (const BodyPairKey& pair : m_activeCollisionPairs)
    {
        const RigidBody* bodyA = GetBody(BodyHandle(pair.bodyA));
        const RigidBody* bodyB = GetBody(BodyHandle(pair.bodyB));
        if (!bodyA || !bodyB)
        {
            continue;
        }

        const CollisionEvent event = MakeCollisionEvent(*bodyA, *bodyB);
        AppendDebugLine(event.contactPoint,
                        event.contactPoint + event.contactNormal * contactNormalLength,
                        GetDebugContactColor(event),
                        lines,
                        colors);
    }
}

} // namespace RVX::Physics
