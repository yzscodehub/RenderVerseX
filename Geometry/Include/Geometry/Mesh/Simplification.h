/**
 * @file Simplification.h
 * @brief Mesh simplification using Quadric Error Metrics (QEM)
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Mesh/HalfEdgeMesh.h"
#include "Geometry/Constants.h"
#include <vector>
#include <queue>
#include <set>

namespace RVX::Geometry
{

/**
 * @brief Options for mesh simplification
 */
struct SimplificationOptions
{
    float targetRatio = 0.5f;       ///< Target triangle count ratio (0.5 = half)
    uint32_t targetTriangles = 0;   ///< Target triangle count (0 = use ratio)
    float maxError = 0.01f;         ///< Maximum allowed error per collapse
    bool preserveBoundary = true;   ///< Preserve boundary edges
    bool preserveUVSeams = false;   ///< Preserve UV discontinuities
    float boundaryWeight = 100.0f;  ///< Weight for boundary preservation
};

/**
 * @brief Quadric error matrix for vertex
 */
struct QuadricMatrix
{
    // Symmetric 4x4 matrix stored as upper triangle (10 values)
    double a[10] = {0};

    QuadricMatrix() = default;

    QuadricMatrix(const Vec3& n, float d)
    {
        // Q = n * n^T (outer product) scaled by d
        double nx = n.x, ny = n.y, nz = n.z;
        a[0] = nx * nx; a[1] = nx * ny; a[2] = nx * nz; a[3] = nx * d;
        a[4] = ny * ny; a[5] = ny * nz; a[6] = ny * d;
        a[7] = nz * nz; a[8] = nz * d;
        a[9] = d * d;
    }

    QuadricMatrix& operator+=(const QuadricMatrix& other)
    {
        for (int i = 0; i < 10; ++i) a[i] += other.a[i];
        return *this;
    }

    QuadricMatrix operator+(const QuadricMatrix& other) const
    {
        QuadricMatrix result = *this;
        result += other;
        return result;
    }

    /// Evaluate error for a point
    double Evaluate(const Vec3& v) const
    {
        double x = v.x, y = v.y, z = v.z;
        return a[0]*x*x + 2*a[1]*x*y + 2*a[2]*x*z + 2*a[3]*x
             + a[4]*y*y + 2*a[5]*y*z + 2*a[6]*y
             + a[7]*z*z + 2*a[8]*z
             + a[9];
    }

    /// Find optimal position (minimizing error)
    bool FindOptimalPosition(Vec3& outPos) const
    {
        // Solve 3x3 linear system
        // | a0  a1  a2 |   | x |   | -a3 |
        // | a1  a4  a5 | * | y | = | -a6 |
        // | a2  a5  a7 |   | z |   | -a8 |

        double det =
            a[0] * (a[4] * a[7] - a[5] * a[5]) -
            a[1] * (a[1] * a[7] - a[5] * a[2]) +
            a[2] * (a[1] * a[5] - a[4] * a[2]);

        if (std::abs(det) < 1e-10) return false;

        double invDet = 1.0 / det;

        outPos.x = static_cast<float>(invDet * (
            -a[3] * (a[4] * a[7] - a[5] * a[5]) +
            a[6] * (a[1] * a[7] - a[2] * a[5]) -
            a[8] * (a[1] * a[5] - a[2] * a[4])));

        outPos.y = static_cast<float>(invDet * (
            a[3] * (a[1] * a[7] - a[2] * a[5]) -
            a[6] * (a[0] * a[7] - a[2] * a[2]) +
            a[8] * (a[0] * a[5] - a[1] * a[2])));

        outPos.z = static_cast<float>(invDet * (
            -a[3] * (a[1] * a[5] - a[4] * a[2]) +
            a[6] * (a[0] * a[5] - a[1] * a[2]) -
            a[8] * (a[0] * a[4] - a[1] * a[1])));

        return true;
    }
};

/**
 * @brief Edge collapse candidate
 */
struct CollapseCandidate
{
    uint32_t v0, v1;       ///< Edge vertices
    Vec3 targetPos;         ///< Optimal position after collapse
    float cost;             ///< Error cost
    uint32_t timestamp;     ///< For lazy deletion

    bool operator>(const CollapseCandidate& other) const
    {
        return cost > other.cost;
    }
};

/**
 * @brief Mesh simplification using Quadric Error Metrics
 */
class MeshSimplifier
{
public:
    /**
     * @brief Simplify a mesh
     * 
     * @param vertices Input/output vertex positions
     * @param indices Input/output triangle indices
     * @param options Simplification options
     */
    static void Simplify(
        std::vector<Vec3>& vertices,
        std::vector<uint32_t>& indices,
        const SimplificationOptions& options = {})
    {
        if (indices.size() < 3) return;

        size_t numTriangles = indices.size() / 3;
        uint32_t targetTris = options.targetTriangles > 0
            ? options.targetTriangles
            : static_cast<uint32_t>(numTriangles * options.targetRatio);

        if (targetTris >= numTriangles) return;

        // Build half-edge mesh
        HalfEdgeMesh mesh;
        mesh.Build(vertices, indices);

        // Compute initial quadrics
        std::vector<QuadricMatrix> quadrics(vertices.size());
        ComputeInitialQuadrics(mesh, quadrics, options);

        // Build priority queue of edge collapses
        std::priority_queue<
            CollapseCandidate,
            std::vector<CollapseCandidate>,
            std::greater<CollapseCandidate>> queue;

        std::vector<uint32_t> timestamps(vertices.size(), 0);

        // Initialize all edge collapses
        for (size_t he = 0; he < mesh.GetHalfEdgeCount(); ++he)
        {
            const HalfEdge& edge = mesh.GetHalfEdge(static_cast<uint32_t>(he));
            if (edge.twin != HalfEdge::INVALID && he > edge.twin)
                continue;  // Skip duplicate edges

            uint32_t v0 = mesh.GetHalfEdge(edge.prev).vertex;
            uint32_t v1 = edge.vertex;

            CollapseCandidate candidate;
            ComputeCollapseCost(mesh, quadrics, v0, v1, candidate, options);
            candidate.timestamp = 0;
            queue.push(candidate);
        }

        // Collapse edges
        std::vector<bool> removed(vertices.size(), false);
        size_t currentTris = numTriangles;

        while (currentTris > targetTris && !queue.empty())
        {
            CollapseCandidate best = queue.top();
            queue.pop();

            // Check if this edge is still valid
            if (removed[best.v0] || removed[best.v1]) continue;
            if (timestamps[best.v0] != best.timestamp) continue;
            if (timestamps[best.v1] != best.timestamp) continue;
            if (best.cost > options.maxError) continue;

            // Check for mesh validity (avoid fold-overs, etc.)
            if (!IsCollapseValid(mesh, best.v0, best.v1, best.targetPos, removed))
                continue;

            // Perform collapse
            removed[best.v1] = true;
            vertices[best.v0] = best.targetPos;
            quadrics[best.v0] += quadrics[best.v1];
            ++timestamps[best.v0];

            // Update triangles
            currentTris -= CountCollapsedTriangles(mesh, best.v0, best.v1);

            // Re-add affected edges to queue
            // (Simplified: in full implementation, update all edges touching v0)
        }

        // Rebuild mesh from remaining triangles
        RebuildMesh(mesh, vertices, indices, removed);
    }

private:
    static void ComputeInitialQuadrics(
        const HalfEdgeMesh& mesh,
        std::vector<QuadricMatrix>& quadrics,
        const SimplificationOptions& options)
    {
        size_t numFaces = mesh.GetFaceCount();

        for (size_t f = 0; f < numFaces; ++f)
        {
            Vec3 normal = mesh.ComputeFaceNormal(static_cast<uint32_t>(f));
            auto verts = mesh.GetFaceVertices(static_cast<uint32_t>(f));
            if (verts.empty()) continue;

            Vec3 p = mesh.GetVertex(verts[0]);
            float d = -glm::dot(normal, p);

            QuadricMatrix Q(normal, d);

            for (uint32_t v : verts)
            {
                quadrics[v] += Q;
            }
        }

        // Add boundary penalty
        if (options.preserveBoundary)
        {
            for (size_t v = 0; v < mesh.GetVertexCount(); ++v)
            {
                if (mesh.IsBoundaryVertex(static_cast<uint32_t>(v)))
                {
                    QuadricMatrix penalty;
                    for (int i = 0; i < 10; ++i)
                        penalty.a[i] = quadrics[v].a[i] * options.boundaryWeight;
                    quadrics[v] += penalty;
                }
            }
        }
    }

    static void ComputeCollapseCost(
        const HalfEdgeMesh& mesh,
        const std::vector<QuadricMatrix>& quadrics,
        uint32_t v0, uint32_t v1,
        CollapseCandidate& out,
        const SimplificationOptions& /*options*/)
    {
        out.v0 = v0;
        out.v1 = v1;

        QuadricMatrix Q = quadrics[v0] + quadrics[v1];

        // Try to find optimal position
        Vec3 optimal;
        if (!Q.FindOptimalPosition(optimal))
        {
            // Fallback to midpoint
            optimal = (mesh.GetVertex(v0) + mesh.GetVertex(v1)) * 0.5f;
        }

        // Clamp to edge segment if optimal is too far
        Vec3 p0 = mesh.GetVertex(v0);
        Vec3 p1 = mesh.GetVertex(v1);
        Vec3 edge = p1 - p0;
        float edgeLen = glm::length(edge);

        if (edgeLen > EPSILON)
        {
            Vec3 toOptimal = optimal - p0;
            float t = glm::dot(toOptimal, edge) / (edgeLen * edgeLen);

            if (t < -0.5f || t > 1.5f)
            {
                optimal = (p0 + p1) * 0.5f;
            }
        }

        out.targetPos = optimal;
        out.cost = static_cast<float>(Q.Evaluate(optimal));
    }

    static bool IsCollapseValid(
        const HalfEdgeMesh& mesh,
        uint32_t v0,
        uint32_t v1,
        const Vec3& targetPos,
        const std::vector<bool>& removed)
    {
        // Check 1: Get neighbors of both vertices
        auto neighbors0 = mesh.GetVertexNeighbors(v0);
        auto neighbors1 = mesh.GetVertexNeighbors(v1);

        // Count shared neighbors (should be exactly 2 for manifold edge)
        int sharedCount = 0;
        for (uint32_t n0 : neighbors0)
        {
            if (removed[n0]) continue;
            for (uint32_t n1 : neighbors1)
            {
                if (removed[n1]) continue;
                if (n0 == n1) ++sharedCount;
            }
        }

        // For interior edge, should have exactly 2 shared neighbors
        // For boundary edge, should have exactly 1
        bool isBoundary = mesh.IsBoundaryVertex(v0) || mesh.IsBoundaryVertex(v1);
        if (!isBoundary && sharedCount != 2) return false;
        if (isBoundary && sharedCount > 2) return false;

        // Check 2: Verify no face normal flipping after collapse
        auto faces0 = mesh.GetVertexFaces(v0);
        auto faces1 = mesh.GetVertexFaces(v1);

        for (uint32_t faceIdx : faces0)
        {
            auto faceVerts = mesh.GetFaceVertices(faceIdx);
            if (faceVerts.size() < 3) continue;

            // Skip faces that will be removed (contain both v0 and v1)
            bool hasV0 = false, hasV1 = false;
            for (uint32_t v : faceVerts)
            {
                if (v == v0) hasV0 = true;
                if (v == v1) hasV1 = true;
            }
            if (hasV0 && hasV1) continue;

            // Compute original normal
            Vec3 origNormal = mesh.ComputeFaceNormal(faceIdx);

            // Compute new normal with v0 moved to targetPos
            std::vector<Vec3> newPositions;
            for (uint32_t v : faceVerts)
            {
                if (v == v0)
                    newPositions.push_back(targetPos);
                else if (removed[v])
                    return false;  // Adjacent vertex was removed
                else
                    newPositions.push_back(mesh.GetVertex(v));
            }

            if (newPositions.size() < 3) continue;

            Vec3 e1 = newPositions[1] - newPositions[0];
            Vec3 e2 = newPositions[2] - newPositions[0];
            Vec3 newNormal = glm::cross(e1, e2);
            float len = glm::length(newNormal);

            // Check for degenerate triangle
            if (len < DEGENERATE_TOLERANCE) return false;

            newNormal /= len;

            // Check if normal flipped (dot product should be positive)
            if (glm::dot(origNormal, newNormal) < 0.0f)
                return false;
        }

        // Check 3: Valence check - don't create vertices with too low valence
        int newValence = 0;
        std::set<uint32_t> allNeighbors;
        for (uint32_t n : neighbors0)
        {
            if (!removed[n] && n != v1) allNeighbors.insert(n);
        }
        for (uint32_t n : neighbors1)
        {
            if (!removed[n] && n != v0) allNeighbors.insert(n);
        }
        newValence = static_cast<int>(allNeighbors.size());

        // Vertex should have at least 3 neighbors after collapse
        if (newValence < 3 && !isBoundary) return false;

        return true;
    }

    static int CountCollapsedTriangles(
        const HalfEdgeMesh& /*mesh*/,
        uint32_t /*v0*/,
        uint32_t /*v1*/)
    {
        // Simplified - typically removes 2 triangles per collapse
        return 2;
    }

    static void RebuildMesh(
        const HalfEdgeMesh& mesh,
        std::vector<Vec3>& vertices,
        std::vector<uint32_t>& indices,
        const std::vector<bool>& removed)
    {
        // Build vertex remapping
        std::vector<uint32_t> remap(vertices.size(), HalfEdge::INVALID);
        std::vector<Vec3> newVerts;

        for (size_t i = 0; i < vertices.size(); ++i)
        {
            if (!removed[i])
            {
                remap[i] = static_cast<uint32_t>(newVerts.size());
                newVerts.push_back(vertices[i]);
            }
        }

        // Rebuild triangles
        std::vector<uint32_t> newIndices;
        for (size_t f = 0; f < mesh.GetFaceCount(); ++f)
        {
            auto verts = mesh.GetFaceVertices(static_cast<uint32_t>(f));
            if (verts.size() < 3) continue;

            bool valid = true;
            for (uint32_t v : verts)
            {
                if (removed[v])
                {
                    valid = false;
                    break;
                }
            }

            if (valid)
            {
                newIndices.push_back(remap[verts[0]]);
                newIndices.push_back(remap[verts[1]]);
                newIndices.push_back(remap[verts[2]]);
            }
        }

        vertices = std::move(newVerts);
        indices = std::move(newIndices);
    }
};

} // namespace RVX::Geometry
