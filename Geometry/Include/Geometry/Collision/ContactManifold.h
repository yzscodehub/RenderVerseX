/**
 * @file ContactManifold.h
 * @brief Contact manifold for physics simulation
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Constants.h"
#include <algorithm>
#include <cmath>

namespace RVX::Geometry
{

/**
 * @brief Single contact point
 */
struct ContactPoint
{
    Vec3 pointA{0};     ///< Contact point on shape A (world space)
    Vec3 pointB{0};     ///< Contact point on shape B (world space)
    Vec3 normal{0, 1, 0}; ///< Contact normal (from A to B)
    float depth = 0.0f;  ///< Penetration depth (positive = penetrating)
    float normalImpulse = 0.0f;  ///< Accumulated normal impulse (for warm starting)
    float tangentImpulse1 = 0.0f; ///< Accumulated tangent impulse 1
    float tangentImpulse2 = 0.0f; ///< Accumulated tangent impulse 2

    /// Get the contact point in world space (average of A and B)
    Vec3 GetWorldPoint() const
    {
        return (pointA + pointB) * 0.5f;
    }

    /// Get the relative position from center of A to contact
    Vec3 GetRelativeA(const Vec3& centerA) const
    {
        return pointA - centerA;
    }

    /// Get the relative position from center of B to contact
    Vec3 GetRelativeB(const Vec3& centerB) const
    {
        return pointB - centerB;
    }
};

/**
 * @brief Contact manifold holding multiple contact points
 * 
 * A manifold stores up to 4 contact points between two shapes.
 * For stable physics simulation, we keep the most representative
 * set of contact points.
 */
struct ContactManifold
{
    static constexpr int MAX_CONTACTS = 4;

    ContactPoint contacts[MAX_CONTACTS];
    int count = 0;

    /// Shape identifiers (for caching)
    uint64_t shapeA = 0;
    uint64_t shapeB = 0;

    /// Contact normal (shared by all contacts)
    Vec3 normal{0, 1, 0};

    /// Clear all contacts
    void Clear()
    {
        count = 0;
    }

    /// Add a contact point
    void Add(const ContactPoint& contact)
    {
        if (count < MAX_CONTACTS)
        {
            contacts[count++] = contact;
        }
        else
        {
            // Replace the point furthest from the new one
            int replaceIdx = FindFurthestPoint(contact.pointA);
            contacts[replaceIdx] = contact;
        }
    }

    /// Add a contact with basic info
    void Add(const Vec3& pointOnA, const Vec3& pointOnB, const Vec3& norm, float penetration)
    {
        ContactPoint cp;
        cp.pointA = pointOnA;
        cp.pointB = pointOnB;
        cp.normal = norm;
        cp.depth = penetration;
        Add(cp);
    }

    /// Reduce contacts to at most 4 (most representative)
    void Reduce()
    {
        if (count <= MAX_CONTACTS)
            return;

        // Keep the 4 points that form the largest area
        int best[4] = {0, 1, 2, 3};

        // Find point with deepest penetration
        float maxDepth = contacts[0].depth;
        best[0] = 0;
        for (int i = 1; i < count; ++i)
        {
            if (contacts[i].depth > maxDepth)
            {
                maxDepth = contacts[i].depth;
                best[0] = i;
            }
        }

        // Find point furthest from first
        float maxDistSq = 0;
        for (int i = 0; i < count; ++i)
        {
            if (i == best[0]) continue;
            float distSq = glm::dot(
                contacts[i].pointA - contacts[best[0]].pointA,
                contacts[i].pointA - contacts[best[0]].pointA);
            if (distSq > maxDistSq)
            {
                maxDistSq = distSq;
                best[1] = i;
            }
        }

        // Find point furthest from line (1-2)
        Vec3 line = contacts[best[1]].pointA - contacts[best[0]].pointA;
        float lineLen = glm::length(line);
        if (lineLen > EPSILON) line /= lineLen;

        maxDistSq = 0;
        for (int i = 0; i < count; ++i)
        {
            if (i == best[0] || i == best[1]) continue;
            Vec3 v = contacts[i].pointA - contacts[best[0]].pointA;
            Vec3 proj = v - line * glm::dot(v, line);
            float distSq = glm::dot(proj, proj);
            if (distSq > maxDistSq)
            {
                maxDistSq = distSq;
                best[2] = i;
            }
        }

        // Find point furthest from triangle plane
        Vec3 e1 = contacts[best[1]].pointA - contacts[best[0]].pointA;
        Vec3 e2 = contacts[best[2]].pointA - contacts[best[0]].pointA;
        Vec3 triNormal = glm::cross(e1, e2);
        float triNormalLen = glm::length(triNormal);
        if (triNormalLen > EPSILON) triNormal /= triNormalLen;

        float maxDist = 0;
        for (int i = 0; i < count; ++i)
        {
            if (i == best[0] || i == best[1] || i == best[2]) continue;
            float dist = std::abs(glm::dot(
                contacts[i].pointA - contacts[best[0]].pointA, triNormal));
            if (dist > maxDist)
            {
                maxDist = dist;
                best[3] = i;
            }
        }

        // Keep only the best 4
        ContactPoint kept[4];
        int keptCount = 0;
        for (int i = 0; i < 4 && keptCount < count; ++i)
        {
            kept[keptCount++] = contacts[best[i]];
        }

        for (int i = 0; i < keptCount; ++i)
        {
            contacts[i] = kept[i];
        }
        count = keptCount;
    }

    /// Update contacts for persistent manifold
    void Update(const Vec3& deltaA, const Vec3& deltaB, float breakingThreshold = 0.02f)
    {
        // Move contact points
        for (int i = 0; i < count; ++i)
        {
            contacts[i].pointA += deltaA;
            contacts[i].pointB += deltaB;

            // Recompute depth
            contacts[i].depth = glm::dot(
                contacts[i].pointB - contacts[i].pointA,
                contacts[i].normal);
        }

        // Remove separated contacts
        for (int i = count - 1; i >= 0; --i)
        {
            if (contacts[i].depth < -breakingThreshold)
            {
                // Remove by swapping with last
                contacts[i] = contacts[--count];
            }
        }
    }

    /// Get average contact point
    Vec3 GetAveragePoint() const
    {
        if (count == 0) return Vec3(0);

        Vec3 sum(0);
        for (int i = 0; i < count; ++i)
        {
            sum += contacts[i].GetWorldPoint();
        }
        return sum / static_cast<float>(count);
    }

    /// Get deepest penetration
    float GetDeepestPenetration() const
    {
        float maxDepth = 0;
        for (int i = 0; i < count; ++i)
        {
            maxDepth = std::max(maxDepth, contacts[i].depth);
        }
        return maxDepth;
    }

private:
    int FindFurthestPoint(const Vec3& point) const
    {
        int furthest = 0;
        float maxDistSq = 0;

        for (int i = 0; i < count; ++i)
        {
            Vec3 d = contacts[i].pointA - point;
            float distSq = glm::dot(d, d);
            if (distSq > maxDistSq)
            {
                maxDistSq = distSq;
                furthest = i;
            }
        }

        return furthest;
    }
};

} // namespace RVX::Geometry
