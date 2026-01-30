#pragma once

/**
 * @file Mesh.h
 * @brief Modern mesh class with flexible vertex attributes and submesh support
 * 
 * Migrated from found::model::Mesh
 */

#include "Scene/VertexAttribute.h"
#include "Core/Math/AABB.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace RVX
{
    /**
     * @brief Index data type for mesh indices
     */
    enum class IndexType
    {
        UInt8,      // 8-bit unsigned integer
        UInt16,     // 16-bit unsigned integer
        UInt32      // 32-bit unsigned integer
    };

    /**
     * @brief Primitive topology type
     */
    enum class PrimitiveType
    {
        Triangles,      // Triangle list
        TriangleStrip,  // Triangle strip
        TriangleFan,    // Triangle fan
        Lines,          // Line list
        LineStrip,      // Line strip
        LineLoop,       // Line loop
        Points          // Point list
    };

    /**
     * @brief SubMesh definition - a portion of the mesh with a specific material
     */
    struct SubMesh
    {
        uint32_t indexOffset = 0;      // Start index in the index buffer
        uint32_t indexCount = 0;       // Number of indices
        int32_t baseVertex = 0;        // Base vertex offset (for indexed drawing)
        uint32_t materialId = 0;       // Material ID reference
        std::optional<BoundingBox> localBounds;
        std::optional<PrimitiveType> primitive;
        std::string name;
    };

    /**
     * @brief Modern mesh class supporting flexible vertex attributes
     * 
     * Features:
     * - Arbitrary vertex attributes (position, normal, uv, etc.)
     * - Multiple index types (8/16/32-bit)
     * - SubMesh support for multi-material meshes
     * - Bounding box computation
     * - Normal and tangent generation
     */
    class Mesh
    {
    public:
        using Ptr = std::shared_ptr<Mesh>;
        using WeakPtr = std::weak_ptr<Mesh>;
        using ConstPtr = std::shared_ptr<const Mesh>;

        /// Mesh name (used for animation target matching, debugging)
        std::string name;

        // =====================================================================
        // Construction
        // =====================================================================

        Mesh() = default;
        ~Mesh() = default;

        // Move semantics
        Mesh(Mesh&&) = default;
        Mesh& operator=(Mesh&&) = default;

        // Disable copy
        Mesh(const Mesh&) = delete;
        Mesh& operator=(const Mesh&) = delete;

        // =====================================================================
        // Vertex Attributes
        // =====================================================================

        /**
         * @brief Add a vertex attribute
         */
        void AddAttribute(const std::string& name, std::unique_ptr<VertexAttribute> attribute);

        /**
         * @brief Template helper to add attribute from vector
         */
        template<typename T>
        void AddAttribute(const std::string& name, const std::vector<T>& data,
                         size_t components, bool normalized = false)
        {
            auto attribute = std::make_unique<VertexAttribute>(data, components, normalized);
            AddAttribute(name, std::move(attribute));
        }

        // Convenience methods for common attributes
        void AddAttribute(const std::string& name, const std::vector<Vec2>& data, bool normalized = false);
        void AddAttribute(const std::string& name, const std::vector<Vec3>& data, bool normalized = false);
        void AddAttribute(const std::string& name, const std::vector<Vec4>& data, bool normalized = false);
        void AddAttribute(const std::string& name, const std::vector<IVec4>& data, bool normalized = false);

        void SetPositions(const std::vector<Vec3>& positions);
        void SetNormals(const std::vector<Vec3>& normals);
        void SetUVs(const std::vector<Vec2>& uvs, const std::string& name = VertexBufferNames::UV);
        void SetColors(const std::vector<Vec4>& colors);
        void SetTangents(const std::vector<Vec4>& tangents);
        void SetBoneData(const std::vector<IVec4>& boneIndices, const std::vector<Vec4>& boneWeights);

        bool HasAttribute(const std::string& name) const;
        const VertexAttribute* GetAttribute(const std::string& name) const;
        const std::unordered_map<std::string, std::unique_ptr<VertexAttribute>>& GetAttributes() const;

        // =====================================================================
        // Index Buffer
        // =====================================================================

        /**
         * @brief Set index data
         */
        template<typename T>
        void SetIndices(const std::vector<T>& indices)
        {
            static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>,
                         "Index type must be unsigned integral");

            if constexpr (sizeof(T) == 1)
                m_indexType = IndexType::UInt8;
            else if constexpr (sizeof(T) == 2)
                m_indexType = IndexType::UInt16;
            else if constexpr (sizeof(T) == 4)
                m_indexType = IndexType::UInt32;

            m_indexData.clear();
            m_indexData.reserve(indices.size() * sizeof(T));
            const uint8_t* rawData = reinterpret_cast<const uint8_t*>(indices.data());
            m_indexData.assign(rawData, rawData + indices.size() * sizeof(T));
            m_indexCount = indices.size();
        }

        void SetIndices(const std::vector<uint32_t>& indices) { SetIndices<uint32_t>(indices); }

        template<typename T>
        std::vector<T> GetTypedIndices() const
        {
            if (m_indexData.empty()) return {};
            const T* typedData = reinterpret_cast<const T*>(m_indexData.data());
            size_t count = m_indexData.size() / sizeof(T);
            return std::vector<T>(typedData, typedData + count);
        }

        const std::vector<uint8_t>& GetIndexData() const { return m_indexData; }
        size_t GetIndexCount() const { return m_indexCount; }
        IndexType GetIndexType() const { return m_indexType; }
        size_t GetVertexCount() const { return m_vertexCount; }

        // =====================================================================
        // Primitive Type
        // =====================================================================

        PrimitiveType GetPrimitiveType() const { return m_primitiveType; }
        void SetPrimitiveType(PrimitiveType type) { m_primitiveType = type; }

        // =====================================================================
        // Bounding Box
        // =====================================================================

        void SetBoundingBox(const Vec3& min, const Vec3& max);
        const std::optional<BoundingBox>& GetBoundingBox() const { return m_boundingBox; }
        bool ComputeBoundingBox();

        // =====================================================================
        // SubMesh Management
        // =====================================================================

        const std::vector<SubMesh>& GetSubMeshes() const { return m_subMeshes; }
        void SetSubMeshes(std::vector<SubMesh> subMeshes) { m_subMeshes = std::move(subMeshes); }
        void AddSubMesh(const SubMesh& sm) { m_subMeshes.push_back(sm); }
        void ClearSubMeshes() { m_subMeshes.clear(); }
        bool HasSubMeshes() const { return !m_subMeshes.empty(); }
        bool ValidateSubMeshes() const;
        void ComputeSubMeshBounds();

        // =====================================================================
        // Geometry Generation
        // =====================================================================

        bool IsValid() const;
        bool GenerateNormals();
        bool GenerateTangents();
        Ptr Clone() const;

        /**
         * @brief Compute vertex normals for a triangle list
         * @note Static utility function for use without a Mesh instance
         */
        static std::vector<Vec3> ComputeVertexNormalsTriList(
            const std::vector<Vec3>& positions,
            const std::vector<uint32_t>& indices);

    private:
        // Vertex attributes
        std::unordered_map<std::string, std::unique_ptr<VertexAttribute>> m_attributes;
        
        // Index data
        std::vector<uint8_t> m_indexData;
        size_t m_indexCount = 0;
        IndexType m_indexType = IndexType::UInt32;
        
        // Geometry info
        size_t m_vertexCount = 0;
        PrimitiveType m_primitiveType = PrimitiveType::Triangles;
        
        // Bounding box
        mutable std::optional<BoundingBox> m_boundingBox;

        // SubMeshes
        std::vector<SubMesh> m_subMeshes;

        void UpdateVertexCount();
    };

    // =========================================================================
    // Mesh Factory Functions
    // =========================================================================
    namespace MeshFactory
    {
        Mesh::Ptr CreateTriangle();
        Mesh::Ptr CreateQuad();
        Mesh::Ptr CreateCube();
        Mesh::Ptr CreateSphere(int segments = 32, int rings = 16);
        Mesh::Ptr CreateCylinder(int segments = 32, float height = 2.0f);
        Mesh::Ptr CreatePlane(float width = 2.0f, float height = 2.0f,
                             int widthSegments = 1, int heightSegments = 1);
    }

} // namespace RVX
