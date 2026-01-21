/**
 * @file HalfEdgeMesh.h
 * @brief Half-edge mesh data structure for mesh processing
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Geometry/Constants.h"
#include <vector>
#include <unordered_map>
#include <span>
#include <cstdint>

namespace RVX::Geometry
{

/**
 * @brief Half-edge structure
 * 
 * Each edge in the mesh is represented by two half-edges pointing in
 * opposite directions. This enables efficient traversal of mesh topology.
 */
struct HalfEdge
{
    uint32_t vertex = INVALID;    ///< Target vertex index
    uint32_t face = INVALID;      ///< Face this half-edge belongs to
    uint32_t next = INVALID;      ///< Next half-edge in the face loop
    uint32_t prev = INVALID;      ///< Previous half-edge in the face loop
    uint32_t twin = INVALID;      ///< Opposite half-edge (same edge, opposite direction)

    static constexpr uint32_t INVALID = ~0u;

    bool IsBoundary() const { return twin == INVALID; }
    bool IsValid() const { return vertex != INVALID; }
};

/**
 * @brief Half-edge mesh data structure
 * 
 * Provides efficient mesh topology queries and modifications.
 * Supports:
 * - Vertex/face/edge traversal
 * - Boundary detection
 * - Topological modifications (split, collapse, flip)
 */
class HalfEdgeMesh
{
public:
    HalfEdgeMesh() = default;

    // =========================================================================
    // Construction
    // =========================================================================

    /**
     * @brief Build from indexed triangle mesh
     * @param vertices Vertex positions
     * @param indices Triangle indices (3 per face)
     */
    void Build(std::span<const Vec3> vertices, std::span<const uint32_t> indices);

    /**
     * @brief Build from polygon soup
     * @param vertices Vertex positions
     * @param faces List of face vertex indices
     */
    void BuildFromPolygons(
        std::span<const Vec3> vertices,
        std::span<const std::vector<uint32_t>> faces);

    /**
     * @brief Clear all data
     */
    void Clear();

    // =========================================================================
    // Accessors
    // =========================================================================

    size_t GetVertexCount() const { return m_vertices.size(); }
    size_t GetFaceCount() const { return m_faceHalfEdge.size(); }
    size_t GetHalfEdgeCount() const { return m_halfEdges.size(); }
    size_t GetEdgeCount() const { return m_halfEdges.size() / 2; }

    const Vec3& GetVertex(uint32_t idx) const { return m_vertices[idx]; }
    Vec3& GetVertex(uint32_t idx) { return m_vertices[idx]; }

    const HalfEdge& GetHalfEdge(uint32_t idx) const { return m_halfEdges[idx]; }
    HalfEdge& GetHalfEdge(uint32_t idx) { return m_halfEdges[idx]; }

    /// Get one half-edge starting from a vertex
    uint32_t GetVertexHalfEdge(uint32_t vertexIdx) const
    {
        return m_vertexHalfEdge[vertexIdx];
    }

    /// Get one half-edge belonging to a face
    uint32_t GetFaceHalfEdge(uint32_t faceIdx) const
    {
        return m_faceHalfEdge[faceIdx];
    }

    // =========================================================================
    // Topology Queries
    // =========================================================================

    /**
     * @brief Get all vertices adjacent to a vertex
     */
    std::vector<uint32_t> GetVertexNeighbors(uint32_t vertexIdx) const;

    /**
     * @brief Get all faces adjacent to a vertex
     */
    std::vector<uint32_t> GetVertexFaces(uint32_t vertexIdx) const;

    /**
     * @brief Get all half-edges emanating from a vertex
     */
    std::vector<uint32_t> GetVertexHalfEdges(uint32_t vertexIdx) const;

    /**
     * @brief Get the vertices of a face
     */
    std::vector<uint32_t> GetFaceVertices(uint32_t faceIdx) const;

    /**
     * @brief Get the number of vertices in a face
     */
    int GetFaceValence(uint32_t faceIdx) const;

    /**
     * @brief Get the valence (degree) of a vertex
     */
    int GetVertexValence(uint32_t vertexIdx) const;

    /**
     * @brief Check if a vertex is on the boundary
     */
    bool IsBoundaryVertex(uint32_t vertexIdx) const;

    /**
     * @brief Check if a half-edge is on the boundary
     */
    bool IsBoundaryEdge(uint32_t halfEdgeIdx) const;

    /**
     * @brief Check if the mesh is manifold
     */
    bool IsManifold() const;

    /**
     * @brief Check if the mesh is closed (no boundary)
     */
    bool IsClosed() const;

    // =========================================================================
    // Geometry Queries
    // =========================================================================

    /**
     * @brief Compute face normal
     */
    Vec3 ComputeFaceNormal(uint32_t faceIdx) const;

    /**
     * @brief Compute vertex normal (area-weighted average of adjacent faces)
     */
    Vec3 ComputeVertexNormal(uint32_t vertexIdx) const;

    /**
     * @brief Compute face area
     */
    float ComputeFaceArea(uint32_t faceIdx) const;

    /**
     * @brief Compute face centroid
     */
    Vec3 ComputeFaceCentroid(uint32_t faceIdx) const;

    /**
     * @brief Get mesh bounding box
     */
    AABB GetBoundingBox() const;

    // =========================================================================
    // Topology Operations
    // =========================================================================

    /**
     * @brief Split an edge at parameter t
     * @return Index of the new vertex
     */
    uint32_t SplitEdge(uint32_t halfEdgeIdx, float t = 0.5f);

    /**
     * @brief Collapse an edge to its midpoint
     * @return Index of the remaining vertex, or INVALID if collapse failed
     */
    uint32_t CollapseEdge(uint32_t halfEdgeIdx);

    /**
     * @brief Flip an edge (requires both adjacent faces to be triangles)
     * @return true if flip succeeded
     */
    bool FlipEdge(uint32_t halfEdgeIdx);

    // =========================================================================
    // Conversion
    // =========================================================================

    /**
     * @brief Convert to indexed triangle mesh
     */
    void ToIndexedMesh(
        std::vector<Vec3>& outVertices,
        std::vector<uint32_t>& outIndices) const;

    /**
     * @brief Triangulate all faces (in-place)
     */
    void Triangulate();

    /**
     * @brief Compute vertex normals for all vertices
     */
    void ComputeAllVertexNormals(std::vector<Vec3>& outNormals) const;

private:
    std::vector<Vec3> m_vertices;
    std::vector<HalfEdge> m_halfEdges;
    std::vector<uint32_t> m_vertexHalfEdge;  // One outgoing half-edge per vertex
    std::vector<uint32_t> m_faceHalfEdge;    // One half-edge per face

    // Helper for finding half-edge by vertex pair
    struct EdgeKey
    {
        uint32_t v0, v1;

        bool operator==(const EdgeKey& other) const
        {
            return v0 == other.v0 && v1 == other.v1;
        }
    };

    struct EdgeKeyHash
    {
        size_t operator()(const EdgeKey& key) const
        {
            return std::hash<uint64_t>{}(
                (static_cast<uint64_t>(key.v0) << 32) | key.v1);
        }
    };

    uint32_t AddVertex(const Vec3& pos);
    uint32_t AddHalfEdge();
    void LinkTwin(uint32_t he1, uint32_t he2);
};

// ============================================================================
// Implementation
// ============================================================================

inline void HalfEdgeMesh::Build(
    std::span<const Vec3> vertices,
    std::span<const uint32_t> indices)
{
    Clear();

    // Copy vertices
    m_vertices.reserve(vertices.size());
    for (const auto& v : vertices)
    {
        m_vertices.push_back(v);
    }
    m_vertexHalfEdge.resize(m_vertices.size(), HalfEdge::INVALID);

    if (indices.size() < 3) return;

    // Build half-edges from triangles
    std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> edgeMap;

    size_t numFaces = indices.size() / 3;
    m_faceHalfEdge.reserve(numFaces);
    m_halfEdges.reserve(indices.size());

    for (size_t f = 0; f < numFaces; ++f)
    {
        uint32_t faceStart = static_cast<uint32_t>(m_halfEdges.size());
        uint32_t faceIdx = static_cast<uint32_t>(m_faceHalfEdge.size());

        // Create 3 half-edges for this triangle
        for (int i = 0; i < 3; ++i)
        {
            uint32_t heIdx = AddHalfEdge();
            HalfEdge& he = m_halfEdges[heIdx];

            uint32_t v0 = indices[f * 3 + i];
            uint32_t v1 = indices[f * 3 + (i + 1) % 3];

            he.vertex = v1;
            he.face = faceIdx;
            he.next = faceStart + (i + 1) % 3;
            he.prev = faceStart + (i + 2) % 3;

            // Update vertex half-edge
            if (m_vertexHalfEdge[v0] == HalfEdge::INVALID)
            {
                m_vertexHalfEdge[v0] = heIdx;
            }

            // Look for twin
            EdgeKey key = {v1, v0};
            auto it = edgeMap.find(key);
            if (it != edgeMap.end())
            {
                LinkTwin(heIdx, it->second);
                edgeMap.erase(it);
            }
            else
            {
                edgeMap[{v0, v1}] = heIdx;
            }
        }

        m_faceHalfEdge.push_back(faceStart);
    }
}

inline void HalfEdgeMesh::Clear()
{
    m_vertices.clear();
    m_halfEdges.clear();
    m_vertexHalfEdge.clear();
    m_faceHalfEdge.clear();
}

inline std::vector<uint32_t> HalfEdgeMesh::GetVertexNeighbors(uint32_t vertexIdx) const
{
    std::vector<uint32_t> neighbors;
    uint32_t start = m_vertexHalfEdge[vertexIdx];
    if (start == HalfEdge::INVALID) return neighbors;

    uint32_t he = start;
    do
    {
        neighbors.push_back(m_halfEdges[he].vertex);
        uint32_t twin = m_halfEdges[he].twin;
        if (twin == HalfEdge::INVALID) break;
        he = m_halfEdges[twin].next;
    } while (he != start);

    return neighbors;
}

inline std::vector<uint32_t> HalfEdgeMesh::GetVertexFaces(uint32_t vertexIdx) const
{
    std::vector<uint32_t> faces;
    uint32_t start = m_vertexHalfEdge[vertexIdx];
    if (start == HalfEdge::INVALID) return faces;

    uint32_t he = start;
    do
    {
        if (m_halfEdges[he].face != HalfEdge::INVALID)
        {
            faces.push_back(m_halfEdges[he].face);
        }
        uint32_t twin = m_halfEdges[he].twin;
        if (twin == HalfEdge::INVALID) break;
        he = m_halfEdges[twin].next;
    } while (he != start);

    return faces;
}

inline std::vector<uint32_t> HalfEdgeMesh::GetFaceVertices(uint32_t faceIdx) const
{
    std::vector<uint32_t> vertices;
    uint32_t start = m_faceHalfEdge[faceIdx];
    uint32_t he = start;

    do
    {
        vertices.push_back(m_halfEdges[m_halfEdges[he].prev].vertex);
        he = m_halfEdges[he].next;
    } while (he != start);

    return vertices;
}

inline bool HalfEdgeMesh::IsBoundaryVertex(uint32_t vertexIdx) const
{
    uint32_t start = m_vertexHalfEdge[vertexIdx];
    if (start == HalfEdge::INVALID) return true;

    uint32_t he = start;
    do
    {
        if (m_halfEdges[he].twin == HalfEdge::INVALID)
            return true;
        he = m_halfEdges[m_halfEdges[he].twin].next;
    } while (he != start);

    return false;
}

inline bool HalfEdgeMesh::IsBoundaryEdge(uint32_t halfEdgeIdx) const
{
    return m_halfEdges[halfEdgeIdx].twin == HalfEdge::INVALID;
}

inline Vec3 HalfEdgeMesh::ComputeFaceNormal(uint32_t faceIdx) const
{
    auto verts = GetFaceVertices(faceIdx);
    if (verts.size() < 3) return Vec3(0, 1, 0);

    Vec3 edge1 = m_vertices[verts[1]] - m_vertices[verts[0]];
    Vec3 edge2 = m_vertices[verts[2]] - m_vertices[verts[0]];
    Vec3 n = glm::cross(edge1, edge2);
    float len = glm::length(n);
    return len > EPSILON ? n / len : Vec3(0, 1, 0);
}

inline Vec3 HalfEdgeMesh::ComputeVertexNormal(uint32_t vertexIdx) const
{
    auto faces = GetVertexFaces(vertexIdx);
    Vec3 normal(0);

    for (uint32_t f : faces)
    {
        normal += ComputeFaceNormal(f) * ComputeFaceArea(f);
    }

    float len = glm::length(normal);
    return len > EPSILON ? normal / len : Vec3(0, 1, 0);
}

inline float HalfEdgeMesh::ComputeFaceArea(uint32_t faceIdx) const
{
    auto verts = GetFaceVertices(faceIdx);
    if (verts.size() < 3) return 0.0f;

    Vec3 edge1 = m_vertices[verts[1]] - m_vertices[verts[0]];
    Vec3 edge2 = m_vertices[verts[2]] - m_vertices[verts[0]];
    return 0.5f * glm::length(glm::cross(edge1, edge2));
}

inline AABB HalfEdgeMesh::GetBoundingBox() const
{
    AABB box;
    for (const auto& v : m_vertices)
    {
        box.Expand(v);
    }
    return box;
}

inline void HalfEdgeMesh::ToIndexedMesh(
    std::vector<Vec3>& outVertices,
    std::vector<uint32_t>& outIndices) const
{
    outVertices = m_vertices;
    outIndices.clear();

    for (size_t f = 0; f < m_faceHalfEdge.size(); ++f)
    {
        auto verts = GetFaceVertices(static_cast<uint32_t>(f));

        // Triangulate if needed
        for (size_t i = 1; i + 1 < verts.size(); ++i)
        {
            outIndices.push_back(verts[0]);
            outIndices.push_back(verts[i]);
            outIndices.push_back(verts[i + 1]);
        }
    }
}

inline void HalfEdgeMesh::ComputeAllVertexNormals(std::vector<Vec3>& outNormals) const
{
    outNormals.resize(m_vertices.size());
    for (size_t i = 0; i < m_vertices.size(); ++i)
    {
        outNormals[i] = ComputeVertexNormal(static_cast<uint32_t>(i));
    }
}

inline uint32_t HalfEdgeMesh::AddVertex(const Vec3& pos)
{
    uint32_t idx = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back(pos);
    m_vertexHalfEdge.push_back(HalfEdge::INVALID);
    return idx;
}

inline uint32_t HalfEdgeMesh::AddHalfEdge()
{
    uint32_t idx = static_cast<uint32_t>(m_halfEdges.size());
    m_halfEdges.emplace_back();
    return idx;
}

inline void HalfEdgeMesh::LinkTwin(uint32_t he1, uint32_t he2)
{
    m_halfEdges[he1].twin = he2;
    m_halfEdges[he2].twin = he1;
}

inline void HalfEdgeMesh::BuildFromPolygons(
    std::span<const Vec3> vertices,
    std::span<const std::vector<uint32_t>> faces)
{
    Clear();

    // Copy vertices
    m_vertices.reserve(vertices.size());
    for (const auto& v : vertices)
    {
        m_vertices.push_back(v);
    }
    m_vertexHalfEdge.resize(m_vertices.size(), HalfEdge::INVALID);

    if (faces.empty()) return;

    // Build half-edges from polygons
    std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> edgeMap;

    for (size_t f = 0; f < faces.size(); ++f)
    {
        const auto& faceVerts = faces[f];
        if (faceVerts.size() < 3) continue;

        uint32_t faceStart = static_cast<uint32_t>(m_halfEdges.size());
        uint32_t faceIdx = static_cast<uint32_t>(m_faceHalfEdge.size());
        size_t n = faceVerts.size();

        // Create half-edges for this face
        for (size_t i = 0; i < n; ++i)
        {
            uint32_t heIdx = AddHalfEdge();
            HalfEdge& he = m_halfEdges[heIdx];

            uint32_t v0 = faceVerts[i];
            uint32_t v1 = faceVerts[(i + 1) % n];

            he.vertex = v1;
            he.face = faceIdx;
            he.next = faceStart + static_cast<uint32_t>((i + 1) % n);
            he.prev = faceStart + static_cast<uint32_t>((i + n - 1) % n);

            if (m_vertexHalfEdge[v0] == HalfEdge::INVALID)
            {
                m_vertexHalfEdge[v0] = heIdx;
            }

            // Look for twin
            EdgeKey key = {v1, v0};
            auto it = edgeMap.find(key);
            if (it != edgeMap.end())
            {
                LinkTwin(heIdx, it->second);
                edgeMap.erase(it);
            }
            else
            {
                edgeMap[{v0, v1}] = heIdx;
            }
        }

        m_faceHalfEdge.push_back(faceStart);
    }
}

inline std::vector<uint32_t> HalfEdgeMesh::GetVertexHalfEdges(uint32_t vertexIdx) const
{
    std::vector<uint32_t> edges;
    uint32_t start = m_vertexHalfEdge[vertexIdx];
    if (start == HalfEdge::INVALID) return edges;

    uint32_t he = start;
    do
    {
        edges.push_back(he);
        uint32_t twin = m_halfEdges[he].twin;
        if (twin == HalfEdge::INVALID) break;
        he = m_halfEdges[twin].next;
    } while (he != start);

    return edges;
}

inline int HalfEdgeMesh::GetFaceValence(uint32_t faceIdx) const
{
    int count = 0;
    uint32_t start = m_faceHalfEdge[faceIdx];
    uint32_t he = start;

    do
    {
        ++count;
        he = m_halfEdges[he].next;
    } while (he != start);

    return count;
}

inline int HalfEdgeMesh::GetVertexValence(uint32_t vertexIdx) const
{
    int count = 0;
    uint32_t start = m_vertexHalfEdge[vertexIdx];
    if (start == HalfEdge::INVALID) return 0;

    uint32_t he = start;
    do
    {
        ++count;
        uint32_t twin = m_halfEdges[he].twin;
        if (twin == HalfEdge::INVALID) break;
        he = m_halfEdges[twin].next;
    } while (he != start);

    return count;
}

inline bool HalfEdgeMesh::IsManifold() const
{
    // Check that each edge has at most 2 adjacent faces
    // In half-edge representation, this means each half-edge has at most one twin
    // Also check that vertex fans are complete (no gaps except at boundary)

    for (size_t v = 0; v < m_vertices.size(); ++v)
    {
        uint32_t start = m_vertexHalfEdge[v];
        if (start == HalfEdge::INVALID) continue;

        uint32_t he = start;
        int boundaryCount = 0;
        int iterations = 0;
        const int maxIterations = static_cast<int>(m_halfEdges.size());

        do
        {
            if (++iterations > maxIterations) return false;  // Infinite loop = non-manifold
            
            if (m_halfEdges[he].twin == HalfEdge::INVALID)
            {
                ++boundaryCount;
                if (boundaryCount > 1) return false;  // Multiple boundary edges = non-manifold
                break;
            }
            he = m_halfEdges[m_halfEdges[he].twin].next;
        } while (he != start);
    }

    return true;
}

inline bool HalfEdgeMesh::IsClosed() const
{
    for (const auto& he : m_halfEdges)
    {
        if (he.twin == HalfEdge::INVALID)
            return false;
    }
    return true;
}

inline Vec3 HalfEdgeMesh::ComputeFaceCentroid(uint32_t faceIdx) const
{
    auto verts = GetFaceVertices(faceIdx);
    if (verts.empty()) return Vec3(0);

    Vec3 sum(0);
    for (uint32_t v : verts)
    {
        sum += m_vertices[v];
    }
    return sum / static_cast<float>(verts.size());
}

inline uint32_t HalfEdgeMesh::SplitEdge(uint32_t halfEdgeIdx, float t)
{
    if (halfEdgeIdx >= m_halfEdges.size()) return HalfEdge::INVALID;

    HalfEdge& he = m_halfEdges[halfEdgeIdx];
    uint32_t v0 = m_halfEdges[he.prev].vertex;
    uint32_t v1 = he.vertex;

    // Create new vertex at interpolated position
    Vec3 newPos = glm::mix(m_vertices[v0], m_vertices[v1], t);
    uint32_t newVert = AddVertex(newPos);

    // Get twin info before modification
    uint32_t twin = he.twin;

    // Split the half-edge: create new half-edge from newVert to v1
    uint32_t newHe = AddHalfEdge();
    m_halfEdges[newHe].vertex = v1;
    m_halfEdges[newHe].face = he.face;
    m_halfEdges[newHe].next = he.next;
    m_halfEdges[newHe].prev = halfEdgeIdx;

    // Update the next half-edge's prev pointer to point to new half-edge
    uint32_t origNext = he.next;
    m_halfEdges[origNext].prev = newHe;
    
    // Update the original half-edge to point to new vertex
    he.vertex = newVert;
    he.next = newHe;

    // Update vertex half-edge reference
    m_vertexHalfEdge[newVert] = newHe;

    // Handle twin if exists
    if (twin != HalfEdge::INVALID)
    {
        HalfEdge& twinHe = m_halfEdges[twin];
        
        // Create new twin half-edge
        uint32_t newTwin = AddHalfEdge();
        m_halfEdges[newTwin].vertex = v0;
        m_halfEdges[newTwin].face = twinHe.face;
        m_halfEdges[newTwin].next = twinHe.next;
        m_halfEdges[newTwin].prev = twin;

        // Update the next half-edge's prev pointer
        uint32_t origTwinNext = twinHe.next;
        m_halfEdges[origTwinNext].prev = newTwin;
        
        // Update twin to point to new vertex
        twinHe.vertex = newVert;
        twinHe.next = newTwin;

        // Link twins
        LinkTwin(newHe, twin);
        LinkTwin(halfEdgeIdx, newTwin);
    }

    return newVert;
}

inline uint32_t HalfEdgeMesh::CollapseEdge(uint32_t halfEdgeIdx)
{
    if (halfEdgeIdx >= m_halfEdges.size()) return HalfEdge::INVALID;

    const HalfEdge& he = m_halfEdges[halfEdgeIdx];
    uint32_t v0 = m_halfEdges[he.prev].vertex;  // Source vertex
    uint32_t v1 = he.vertex;                      // Target vertex
    uint32_t twin = he.twin;

    // Check if collapse is valid (simple check - not comprehensive)
    if (v0 == v1) return HalfEdge::INVALID;

    // Move v0 to midpoint
    m_vertices[v0] = (m_vertices[v0] + m_vertices[v1]) * 0.5f;

    // Redirect all half-edges pointing to v1 to point to v0
    for (auto& edge : m_halfEdges)
    {
        if (edge.vertex == v1)
        {
            edge.vertex = v0;
        }
    }

    // Update vertex references
    if (m_vertexHalfEdge[v1] != HalfEdge::INVALID)
    {
        // Find a valid half-edge for v0 if needed
        uint32_t heStart = m_vertexHalfEdge[v1];
        uint32_t heCurrent = heStart;
        do
        {
            // Update source vertex reference
            uint32_t prevHe = m_halfEdges[heCurrent].prev;
            if (m_halfEdges[prevHe].vertex == v1)
            {
                m_halfEdges[prevHe].vertex = v0;
            }

            uint32_t twinHe = m_halfEdges[heCurrent].twin;
            if (twinHe == HalfEdge::INVALID) break;
            heCurrent = m_halfEdges[twinHe].next;
        } while (heCurrent != heStart);
    }

    // Mark v1 as removed (we don't actually remove it to keep indices valid)
    m_vertexHalfEdge[v1] = HalfEdge::INVALID;

    // Remove degenerate faces (faces where the collapsed edge was)
    // Mark the half-edges of the collapsed edge as invalid
    m_halfEdges[halfEdgeIdx].vertex = HalfEdge::INVALID;
    if (twin != HalfEdge::INVALID)
    {
        m_halfEdges[twin].vertex = HalfEdge::INVALID;
    }

    return v0;
}

inline bool HalfEdgeMesh::FlipEdge(uint32_t halfEdgeIdx)
{
    if (halfEdgeIdx >= m_halfEdges.size()) return false;

    HalfEdge& he = m_halfEdges[halfEdgeIdx];
    if (he.twin == HalfEdge::INVALID) return false;  // Can't flip boundary edge

    HalfEdge& twin = m_halfEdges[he.twin];

    // Both faces must be triangles
    if (GetFaceValence(he.face) != 3 || GetFaceValence(twin.face) != 3)
        return false;

    // Get the 4 vertices of the two triangles
    //     v2
    //    /  \
    //  v0----v1  (he goes v0->v1, twin goes v1->v0)
    //    \  /
    //     v3
    uint32_t v0 = m_halfEdges[he.prev].vertex;
    uint32_t v1 = he.vertex;
    uint32_t v2 = m_halfEdges[he.next].vertex;
    uint32_t v3 = m_halfEdges[twin.next].vertex;

    // Get half-edges
    uint32_t heNext = he.next;
    uint32_t hePrev = he.prev;
    uint32_t twinNext = twin.next;
    uint32_t twinPrev = twin.prev;

    // Flip: edge now connects v2-v3 instead of v0-v1
    he.vertex = v3;
    twin.vertex = v2;

    // Reconnect half-edges
    // Face 1: v0 -> v2 -> v3 -> v0
    he.next = twinPrev;
    he.prev = heNext;
    m_halfEdges[heNext].next = halfEdgeIdx;
    m_halfEdges[heNext].prev = twinPrev;
    m_halfEdges[twinPrev].next = heNext;
    m_halfEdges[twinPrev].prev = halfEdgeIdx;
    m_halfEdges[twinPrev].face = he.face;

    // Face 2: v1 -> v3 -> v2 -> v1
    twin.next = hePrev;
    twin.prev = twinNext;
    m_halfEdges[twinNext].next = he.twin;
    m_halfEdges[twinNext].prev = hePrev;
    m_halfEdges[hePrev].next = twinNext;
    m_halfEdges[hePrev].prev = he.twin;
    m_halfEdges[hePrev].face = twin.face;

    // Update face half-edge pointers
    m_faceHalfEdge[he.face] = halfEdgeIdx;
    m_faceHalfEdge[twin.face] = he.twin;

    // Update vertex half-edge pointers
    m_vertexHalfEdge[v0] = heNext;
    m_vertexHalfEdge[v1] = twinNext;
    m_vertexHalfEdge[v2] = he.twin;
    m_vertexHalfEdge[v3] = halfEdgeIdx;

    return true;
}

inline void HalfEdgeMesh::Triangulate()
{
    std::vector<uint32_t> newFaceHalfEdges;
    std::vector<HalfEdge> newHalfEdges;
    
    for (size_t f = 0; f < m_faceHalfEdge.size(); ++f)
    {
        int valence = GetFaceValence(static_cast<uint32_t>(f));
        
        if (valence <= 3)
        {
            // Already a triangle, keep as-is
            newFaceHalfEdges.push_back(m_faceHalfEdge[f]);
            continue;
        }

        // Fan triangulation from first vertex
        auto verts = GetFaceVertices(static_cast<uint32_t>(f));
        uint32_t v0 = verts[0];

        for (size_t i = 1; i + 1 < verts.size(); ++i)
        {
            uint32_t v1 = verts[i];
            uint32_t v2 = verts[i + 1];

            uint32_t faceStart = static_cast<uint32_t>(m_halfEdges.size() + newHalfEdges.size());
            uint32_t newFaceIdx = static_cast<uint32_t>(m_faceHalfEdge.size() + newFaceHalfEdges.size());

            // Create 3 half-edges
            for (int j = 0; j < 3; ++j)
            {
                HalfEdge newHe;
                newHe.face = newFaceIdx;
                newHe.next = faceStart + (j + 1) % 3;
                newHe.prev = faceStart + (j + 2) % 3;

                switch (j)
                {
                    case 0: newHe.vertex = v1; break;
                    case 1: newHe.vertex = v2; break;
                    case 2: newHe.vertex = v0; break;
                }

                newHalfEdges.push_back(newHe);
            }

            newFaceHalfEdges.push_back(faceStart);
        }
    }

    // Rebuild with new faces
    if (!newHalfEdges.empty())
    {
        m_halfEdges.insert(m_halfEdges.end(), newHalfEdges.begin(), newHalfEdges.end());
    }
    if (!newFaceHalfEdges.empty())
    {
        m_faceHalfEdge = std::move(newFaceHalfEdges);
    }

    // Note: Twin pointers would need to be rebuilt for a complete implementation
}

} // namespace RVX::Geometry
