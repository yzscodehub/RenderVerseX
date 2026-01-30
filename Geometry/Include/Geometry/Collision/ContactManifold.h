/**
 * @file ContactManifold.h
 * @brief Contact manifold for physics simulation
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Constants.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace RVX::Geometry
{

/**
 * @brief Feature ID for persistent contact identification
 * 
 * Used to match contacts across frames for warm-starting.
 * Encodes which features of two shapes are in contact.
 */
struct ContactFeatureID
{
    uint8_t indexA = 0;     ///< Feature index on shape A (vertex/edge/face)
    uint8_t indexB = 0;     ///< Feature index on shape B (vertex/edge/face)
    uint8_t typeA = 0;      ///< Feature type on A: 0=vertex, 1=edge, 2=face
    uint8_t typeB = 0;      ///< Feature type on B: 0=vertex, 1=edge, 2=face

    ContactFeatureID() = default;

    ContactFeatureID(uint8_t idxA, uint8_t idxB, uint8_t tA = 0, uint8_t tB = 0)
        : indexA(idxA), indexB(idxB), typeA(tA), typeB(tB) {}

    bool operator==(const ContactFeatureID& other) const
    {
        return indexA == other.indexA && indexB == other.indexB &&
               typeA == other.typeA && typeB == other.typeB;
    }

    bool operator!=(const ContactFeatureID& other) const
    {
        return !(*this == other);
    }

    /// Generate a hash value for use in containers
    uint32_t Hash() const
    {
        return static_cast<uint32_t>(indexA) |
               (static_cast<uint32_t>(indexB) << 8) |
               (static_cast<uint32_t>(typeA) << 16) |
               (static_cast<uint32_t>(typeB) << 24);
    }

    /// Invalid/unset feature ID
    static ContactFeatureID Invalid()
    {
        return ContactFeatureID(0xFF, 0xFF, 0xFF, 0xFF);
    }

    bool IsValid() const
    {
        return typeA != 0xFF;
    }
};

/**
 * @brief Single contact point
 */
struct ContactPoint
{
    Vec3 pointA{0};          ///< Contact point on shape A (world space)
    Vec3 pointB{0};          ///< Contact point on shape B (world space)
    Vec3 normal{0, 1, 0};    ///< Contact normal (from A to B)
    float depth = 0.0f;      ///< Penetration depth (positive = penetrating)
    float normalImpulse = 0.0f;   ///< Accumulated normal impulse (for warm starting)
    float tangentImpulse1 = 0.0f; ///< Accumulated tangent impulse 1
    float tangentImpulse2 = 0.0f; ///< Accumulated tangent impulse 2
    ContactFeatureID featureID;   ///< Feature ID for persistent matching
    uint32_t lifespan = 0;        ///< Number of frames this contact has existed

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

    /// Reset accumulated impulses
    void ResetImpulses()
    {
        normalImpulse = 0.0f;
        tangentImpulse1 = 0.0f;
        tangentImpulse2 = 0.0f;
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

    /// Add a contact with feature ID for persistent tracking
    void Add(const Vec3& pointOnA, const Vec3& pointOnB, const Vec3& norm, 
             float penetration, const ContactFeatureID& featureId)
    {
        ContactPoint cp;
        cp.pointA = pointOnA;
        cp.pointB = pointOnB;
        cp.normal = norm;
        cp.depth = penetration;
        cp.featureID = featureId;
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

    /// Update contacts for persistent manifold (legacy version)
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
            
            contacts[i].lifespan++;
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

    /**
     * @brief Merge with a new manifold, preserving impulses for matching contacts
     * 
     * This is the main entry point for persistent contact caching.
     * Matches contacts by feature ID or position proximity.
     * 
     * @param newManifold The newly computed manifold
     * @param positionThreshold Distance threshold for position-based matching
     */
    void MergeWith(const ContactManifold& newManifold, float positionThreshold = 0.01f)
    {
        float thresholdSq = positionThreshold * positionThreshold;

        // For each new contact, try to find a matching old contact
        ContactPoint mergedContacts[MAX_CONTACTS];
        int mergedCount = 0;

        for (int newIdx = 0; newIdx < newManifold.count && mergedCount < MAX_CONTACTS; ++newIdx)
        {
            const ContactPoint& newContact = newManifold.contacts[newIdx];
            int matchIdx = FindMatchingContact(newContact, thresholdSq);

            if (matchIdx >= 0)
            {
                // Found a match - copy new positions but preserve impulses
                mergedContacts[mergedCount] = newContact;
                mergedContacts[mergedCount].normalImpulse = contacts[matchIdx].normalImpulse;
                mergedContacts[mergedCount].tangentImpulse1 = contacts[matchIdx].tangentImpulse1;
                mergedContacts[mergedCount].tangentImpulse2 = contacts[matchIdx].tangentImpulse2;
                mergedContacts[mergedCount].lifespan = contacts[matchIdx].lifespan + 1;
            }
            else
            {
                // New contact
                mergedContacts[mergedCount] = newContact;
                mergedContacts[mergedCount].lifespan = 0;
            }
            mergedCount++;
        }

        // Update this manifold with merged contacts
        for (int i = 0; i < mergedCount; ++i)
        {
            contacts[i] = mergedContacts[i];
        }
        count = mergedCount;
        normal = newManifold.normal;
    }

    /**
     * @brief Find a contact matching by feature ID or position
     * @return Index of matching contact, or -1 if not found
     */
    int FindMatchingContact(const ContactPoint& query, float posThresholdSq) const
    {
        // First try to match by feature ID
        if (query.featureID.IsValid())
        {
            for (int i = 0; i < count; ++i)
            {
                if (contacts[i].featureID == query.featureID)
                {
                    return i;
                }
            }
        }

        // Fall back to position-based matching
        int bestMatch = -1;
        float bestDistSq = posThresholdSq;

        for (int i = 0; i < count; ++i)
        {
            Vec3 diff = contacts[i].pointA - query.pointA;
            float distSq = glm::dot(diff, diff);
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestMatch = i;
            }
        }

        return bestMatch;
    }

    /**
     * @brief Age out old contacts and remove stale ones
     * @param maxAge Maximum lifespan before removal
     */
    void PruneStaleContacts(uint32_t maxAge = 60)
    {
        for (int i = count - 1; i >= 0; --i)
        {
            if (contacts[i].lifespan > maxAge)
            {
                contacts[i] = contacts[--count];
            }
        }
    }

    /**
     * @brief Validate contacts against current transforms
     * 
     * Removes contacts that are no longer valid (separated or drifted too far).
     * 
     * @param transformA Current transform of shape A
     * @param transformB Current transform of shape B
     * @param breakingThreshold Separation distance to remove contact
     * @param driftThreshold Tangential drift distance to remove contact
     */
    void ValidateContacts(
        const Mat4& transformA,
        const Mat4& transformB,
        float breakingThreshold = 0.02f,
        float driftThreshold = 0.05f)
    {
        float driftThresholdSq = driftThreshold * driftThreshold;

        for (int i = count - 1; i >= 0; --i)
        {
            ContactPoint& cp = contacts[i];

            // Transform local contact points to world space
            Vec3 worldA = Vec3(transformA * Vec4(cp.pointA, 1.0f));
            Vec3 worldB = Vec3(transformB * Vec4(cp.pointB, 1.0f));

            // Check separation
            Vec3 diff = worldB - worldA;
            float separation = glm::dot(diff, cp.normal);

            if (separation < -breakingThreshold)
            {
                // Contact is separating
                contacts[i] = contacts[--count];
                continue;
            }

            // Check tangential drift
            Vec3 tangent = diff - cp.normal * separation;
            float driftSq = glm::dot(tangent, tangent);

            if (driftSq > driftThresholdSq)
            {
                // Contact has drifted too far
                contacts[i] = contacts[--count];
                continue;
            }

            // Update contact positions
            cp.pointA = worldA;
            cp.pointB = worldB;
            cp.depth = -separation;  // Positive depth = penetrating
        }
    }

    /**
     * @brief Get the total accumulated normal impulse
     */
    float GetTotalNormalImpulse() const
    {
        float total = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            total += contacts[i].normalImpulse;
        }
        return total;
    }

    /**
     * @brief Reset all accumulated impulses (call when manifold is invalidated)
     */
    void ResetImpulses()
    {
        for (int i = 0; i < count; ++i)
        {
            contacts[i].ResetImpulses();
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
