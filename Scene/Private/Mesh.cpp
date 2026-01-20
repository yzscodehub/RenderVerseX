#include "Scene/Mesh.h"
#include <cmath>

namespace RVX
{
    // =========================================================================
    // Mesh Implementation
    // =========================================================================

    void Mesh::AddAttribute(const std::string& attrName, std::unique_ptr<VertexAttribute> attribute)
    {
        if (!attribute) return;
        
        // Update vertex count from first valid attribute
        if (m_vertexCount == 0)
        {
            m_vertexCount = attribute->GetVertexCount();
        }
        
        m_attributes[attrName] = std::move(attribute);
    }

    void Mesh::AddAttribute(const std::string& attrName, const std::vector<Vec2>& data, bool normalized)
    {
        auto attribute = std::make_unique<VertexAttribute>(data, normalized);
        AddAttribute(attrName, std::move(attribute));
    }

    void Mesh::AddAttribute(const std::string& attrName, const std::vector<Vec3>& data, bool normalized)
    {
        auto attribute = std::make_unique<VertexAttribute>(data, normalized);
        AddAttribute(attrName, std::move(attribute));
    }

    void Mesh::AddAttribute(const std::string& attrName, const std::vector<Vec4>& data, bool normalized)
    {
        auto attribute = std::make_unique<VertexAttribute>(data, normalized);
        AddAttribute(attrName, std::move(attribute));
    }

    void Mesh::AddAttribute(const std::string& attrName, const std::vector<IVec4>& data, bool normalized)
    {
        auto attribute = std::make_unique<VertexAttribute>(data, normalized);
        AddAttribute(attrName, std::move(attribute));
    }

    void Mesh::SetPositions(const std::vector<Vec3>& positions)
    {
        AddAttribute(VertexBufferNames::Position, positions, false);
    }

    void Mesh::SetNormals(const std::vector<Vec3>& normals)
    {
        AddAttribute(VertexBufferNames::Normal, normals, false);
    }

    void Mesh::SetUVs(const std::vector<Vec2>& uvs, const std::string& attrName)
    {
        AddAttribute(attrName, uvs, false);
    }

    void Mesh::SetColors(const std::vector<Vec4>& colors)
    {
        AddAttribute(VertexBufferNames::Color, colors, false);
    }

    void Mesh::SetTangents(const std::vector<Vec4>& tangents)
    {
        AddAttribute(VertexBufferNames::Tangent, tangents, false);
    }

    void Mesh::SetBoneData(const std::vector<IVec4>& boneIndices, const std::vector<Vec4>& boneWeights)
    {
        AddAttribute(VertexBufferNames::BoneIndices, boneIndices, false);
        AddAttribute(VertexBufferNames::BoneWeights, boneWeights, false);
    }

    bool Mesh::HasAttribute(const std::string& attrName) const
    {
        return m_attributes.find(attrName) != m_attributes.end();
    }

    const VertexAttribute* Mesh::GetAttribute(const std::string& attrName) const
    {
        auto it = m_attributes.find(attrName);
        return it != m_attributes.end() ? it->second.get() : nullptr;
    }

    const std::unordered_map<std::string, std::unique_ptr<VertexAttribute>>& Mesh::GetAttributes() const
    {
        return m_attributes;
    }

    void Mesh::SetBoundingBox(const Vec3& min, const Vec3& max)
    {
        m_boundingBox = BoundingBox(min, max);
    }

    bool Mesh::ComputeBoundingBox()
    {
        const VertexAttribute* posAttr = GetAttribute(VertexBufferNames::Position);
        if (!posAttr || posAttr->GetVertexCount() == 0)
        {
            return false;
        }

        BoundingBox bbox;
        const float* data = static_cast<const float*>(posAttr->GetData());
        size_t vertexCount = posAttr->GetVertexCount();
        
        for (size_t i = 0; i < vertexCount; ++i)
        {
            Vec3 pos(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
            bbox.Expand(pos);
        }
        
        m_boundingBox = bbox;
        return true;
    }

    bool Mesh::ValidateSubMeshes() const
    {
        for (const auto& sm : m_subMeshes)
        {
            if (sm.indexOffset + sm.indexCount > m_indexCount)
            {
                return false;
            }
        }
        return true;
    }

    void Mesh::ComputeSubMeshBounds()
    {
        const VertexAttribute* posAttr = GetAttribute(VertexBufferNames::Position);
        if (!posAttr) return;

        const float* positions = static_cast<const float*>(posAttr->GetData());
        std::vector<uint32_t> indices = GetTypedIndices<uint32_t>();
        
        for (auto& sm : m_subMeshes)
        {
            BoundingBox bounds;
            for (uint32_t i = 0; i < sm.indexCount; ++i)
            {
                uint32_t idx = indices[sm.indexOffset + i];
                Vec3 pos(positions[idx * 3], positions[idx * 3 + 1], positions[idx * 3 + 2]);
                bounds.Expand(pos);
            }
            if (bounds.IsValid())
            {
                sm.localBounds = bounds;
            }
        }
    }

    bool Mesh::IsValid() const
    {
        return m_vertexCount > 0 && HasAttribute(VertexBufferNames::Position);
    }

    bool Mesh::GenerateNormals()
    {
        const VertexAttribute* posAttr = GetAttribute(VertexBufferNames::Position);
        if (!posAttr || m_indexCount == 0)
        {
            return false;
        }

        std::vector<Vec3> positions;
        positions.reserve(posAttr->GetVertexCount());
        const float* data = static_cast<const float*>(posAttr->GetData());
        for (size_t i = 0; i < posAttr->GetVertexCount(); ++i)
        {
            positions.emplace_back(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
        }

        std::vector<uint32_t> indices = GetTypedIndices<uint32_t>();
        std::vector<Vec3> normals = ComputeVertexNormalsTriList(positions, indices);
        
        if (normals.empty())
        {
            return false;
        }

        SetNormals(normals);
        return true;
    }

    bool Mesh::GenerateTangents()
    {
        // Tangent generation requires position, normal, uv, and indices
        const VertexAttribute* posAttr = GetAttribute(VertexBufferNames::Position);
        const VertexAttribute* normAttr = GetAttribute(VertexBufferNames::Normal);
        const VertexAttribute* uvAttr = GetAttribute(VertexBufferNames::UV);
        
        if (!posAttr || !normAttr || !uvAttr || m_indexCount == 0)
        {
            return false;
        }

        size_t vertexCount = posAttr->GetVertexCount();
        std::vector<Vec3> tangents(vertexCount, Vec3(0.0f));
        std::vector<Vec3> bitangents(vertexCount, Vec3(0.0f));

        const float* positions = static_cast<const float*>(posAttr->GetData());
        const float* normals = static_cast<const float*>(normAttr->GetData());
        const float* uvs = static_cast<const float*>(uvAttr->GetData());
        std::vector<uint32_t> indices = GetTypedIndices<uint32_t>();

        // Compute tangents per triangle
        for (size_t i = 0; i + 2 < m_indexCount; i += 3)
        {
            uint32_t i0 = indices[i];
            uint32_t i1 = indices[i + 1];
            uint32_t i2 = indices[i + 2];

            Vec3 v0(positions[i0 * 3], positions[i0 * 3 + 1], positions[i0 * 3 + 2]);
            Vec3 v1(positions[i1 * 3], positions[i1 * 3 + 1], positions[i1 * 3 + 2]);
            Vec3 v2(positions[i2 * 3], positions[i2 * 3 + 1], positions[i2 * 3 + 2]);

            Vec2 uv0(uvs[i0 * 2], uvs[i0 * 2 + 1]);
            Vec2 uv1(uvs[i1 * 2], uvs[i1 * 2 + 1]);
            Vec2 uv2(uvs[i2 * 2], uvs[i2 * 2 + 1]);

            Vec3 edge1 = v1 - v0;
            Vec3 edge2 = v2 - v0;
            Vec2 deltaUV1 = uv1 - uv0;
            Vec2 deltaUV2 = uv2 - uv0;

            float denom = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
            if (std::abs(denom) < 1e-8f) continue;

            float f = 1.0f / denom;
            Vec3 tangent = (edge1 * deltaUV2.y - edge2 * deltaUV1.y) * f;
            Vec3 bitangent = (edge2 * deltaUV1.x - edge1 * deltaUV2.x) * f;

            tangents[i0] += tangent;
            tangents[i1] += tangent;
            tangents[i2] += tangent;
            bitangents[i0] += bitangent;
            bitangents[i1] += bitangent;
            bitangents[i2] += bitangent;
        }

        // Orthonormalize and compute handedness
        std::vector<Vec4> finalTangents(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            Vec3 n(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]);
            Vec3 t = tangents[i];
            
            // Gram-Schmidt orthogonalize
            t = normalize(t - n * dot(n, t));
            
            // Calculate handedness
            float w = (dot(cross(n, t), bitangents[i]) < 0.0f) ? -1.0f : 1.0f;
            
            finalTangents[i] = Vec4(t.x, t.y, t.z, w);
        }

        SetTangents(finalTangents);
        return true;
    }

    Mesh::Ptr Mesh::Clone() const
    {
        auto clone = std::make_shared<Mesh>();
        clone->name = name;
        clone->m_indexData = m_indexData;
        clone->m_indexCount = m_indexCount;
        clone->m_indexType = m_indexType;
        clone->m_vertexCount = m_vertexCount;
        clone->m_primitiveType = m_primitiveType;
        clone->m_boundingBox = m_boundingBox;
        clone->m_subMeshes = m_subMeshes;
        
        for (const auto& [attrName, attr] : m_attributes)
        {
            if (attr)
            {
                clone->m_attributes[attrName] = attr->Clone();
            }
        }
        
        return clone;
    }

    std::vector<Vec3> Mesh::ComputeVertexNormalsTriList(
        const std::vector<Vec3>& positions,
        const std::vector<uint32_t>& indices)
    {
        if (positions.empty() || indices.size() < 3)
        {
            return {};
        }

        std::vector<Vec3> normals(positions.size(), Vec3(0.0f));

        // Accumulate face normals (area-weighted)
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            uint32_t i0 = indices[i];
            uint32_t i1 = indices[i + 1];
            uint32_t i2 = indices[i + 2];

            if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
            {
                continue;
            }

            const Vec3& v0 = positions[i0];
            const Vec3& v1 = positions[i1];
            const Vec3& v2 = positions[i2];

            Vec3 edge1 = v1 - v0;
            Vec3 edge2 = v2 - v0;
            Vec3 faceNormal = cross(edge1, edge2);  // Area-weighted

            normals[i0] += faceNormal;
            normals[i1] += faceNormal;
            normals[i2] += faceNormal;
        }

        // Normalize
        for (auto& n : normals)
        {
            float len = length(n);
            if (len > 1e-8f)
            {
                n /= len;
            }
        }

        return normals;
    }

    void Mesh::UpdateVertexCount()
    {
        m_vertexCount = 0;
        for (const auto& [attrName, attr] : m_attributes)
        {
            (void)attrName;  // Unused
            if (attr && attr->GetVertexCount() > 0)
            {
                m_vertexCount = attr->GetVertexCount();
                break;
            }
        }
    }

    // =========================================================================
    // MeshFactory Implementation
    // =========================================================================

    namespace MeshFactory
    {
        Mesh::Ptr CreateTriangle()
        {
            auto mesh = std::make_shared<Mesh>();
            mesh->name = "Triangle";
            
            std::vector<Vec3> positions = {
                {0.0f, 0.5f, 0.0f},
                {-0.5f, -0.5f, 0.0f},
                {0.5f, -0.5f, 0.0f}
            };
            
            std::vector<Vec3> normals = {
                {0.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, 1.0f}
            };
            
            std::vector<Vec2> uvs = {
                {0.5f, 1.0f},
                {0.0f, 0.0f},
                {1.0f, 0.0f}
            };
            
            std::vector<uint32_t> indices = {0, 1, 2};
            
            mesh->SetPositions(positions);
            mesh->SetNormals(normals);
            mesh->SetUVs(uvs);
            mesh->SetIndices(indices);
            mesh->ComputeBoundingBox();
            
            return mesh;
        }

        Mesh::Ptr CreateQuad()
        {
            auto mesh = std::make_shared<Mesh>();
            mesh->name = "Quad";
            
            std::vector<Vec3> positions = {
                {-0.5f, -0.5f, 0.0f},
                {0.5f, -0.5f, 0.0f},
                {0.5f, 0.5f, 0.0f},
                {-0.5f, 0.5f, 0.0f}
            };
            
            std::vector<Vec3> normals = {
                {0.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, 1.0f}
            };
            
            std::vector<Vec2> uvs = {
                {0.0f, 0.0f},
                {1.0f, 0.0f},
                {1.0f, 1.0f},
                {0.0f, 1.0f}
            };
            
            std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
            
            mesh->SetPositions(positions);
            mesh->SetNormals(normals);
            mesh->SetUVs(uvs);
            mesh->SetIndices(indices);
            mesh->ComputeBoundingBox();
            
            return mesh;
        }

        Mesh::Ptr CreateCube()
        {
            auto mesh = std::make_shared<Mesh>();
            mesh->name = "Cube";
            
            // 24 vertices (4 per face, for proper normals)
            std::vector<Vec3> positions;
            std::vector<Vec3> normals;
            std::vector<Vec2> uvs;
            std::vector<uint32_t> indices;
            
            // Helper to add a face
            auto addFace = [&](const Vec3& normal, const Vec3& right, const Vec3& up) {
                uint32_t baseIdx = static_cast<uint32_t>(positions.size());
                Vec3 center = normal * 0.5f;
                
                positions.push_back(center - right * 0.5f - up * 0.5f);
                positions.push_back(center + right * 0.5f - up * 0.5f);
                positions.push_back(center + right * 0.5f + up * 0.5f);
                positions.push_back(center - right * 0.5f + up * 0.5f);
                
                for (int i = 0; i < 4; ++i) normals.push_back(normal);
                
                uvs.push_back({0.0f, 0.0f});
                uvs.push_back({1.0f, 0.0f});
                uvs.push_back({1.0f, 1.0f});
                uvs.push_back({0.0f, 1.0f});
                
                indices.push_back(baseIdx);
                indices.push_back(baseIdx + 1);
                indices.push_back(baseIdx + 2);
                indices.push_back(baseIdx);
                indices.push_back(baseIdx + 2);
                indices.push_back(baseIdx + 3);
            };
            
            // +Z, -Z, +X, -X, +Y, -Y
            addFace({0, 0, 1}, {1, 0, 0}, {0, 1, 0});   // Front
            addFace({0, 0, -1}, {-1, 0, 0}, {0, 1, 0}); // Back
            addFace({1, 0, 0}, {0, 0, -1}, {0, 1, 0});  // Right
            addFace({-1, 0, 0}, {0, 0, 1}, {0, 1, 0});  // Left
            addFace({0, 1, 0}, {1, 0, 0}, {0, 0, -1});  // Top
            addFace({0, -1, 0}, {1, 0, 0}, {0, 0, 1});  // Bottom
            
            mesh->SetPositions(positions);
            mesh->SetNormals(normals);
            mesh->SetUVs(uvs);
            mesh->SetIndices(indices);
            mesh->ComputeBoundingBox();
            
            return mesh;
        }

        Mesh::Ptr CreateSphere(int segments, int rings)
        {
            auto mesh = std::make_shared<Mesh>();
            mesh->name = "Sphere";
            
            std::vector<Vec3> positions;
            std::vector<Vec3> normals;
            std::vector<Vec2> uvs;
            std::vector<uint32_t> indices;
            
            const float radius = 0.5f;
            
            for (int ring = 0; ring <= rings; ++ring)
            {
                float phi = static_cast<float>(ring) / rings * glm::pi<float>();
                float sinPhi = std::sin(phi);
                float cosPhi = std::cos(phi);
                
                for (int seg = 0; seg <= segments; ++seg)
                {
                    float theta = static_cast<float>(seg) / segments * 2.0f * glm::pi<float>();
                    float sinTheta = std::sin(theta);
                    float cosTheta = std::cos(theta);
                    
                    Vec3 normal(sinPhi * cosTheta, cosPhi, sinPhi * sinTheta);
                    Vec3 pos = normal * radius;
                    Vec2 uv(static_cast<float>(seg) / segments, static_cast<float>(ring) / rings);
                    
                    positions.push_back(pos);
                    normals.push_back(normal);
                    uvs.push_back(uv);
                }
            }
            
            for (int ring = 0; ring < rings; ++ring)
            {
                for (int seg = 0; seg < segments; ++seg)
                {
                    uint32_t curr = ring * (segments + 1) + seg;
                    uint32_t next = curr + segments + 1;
                    
                    indices.push_back(curr);
                    indices.push_back(next);
                    indices.push_back(curr + 1);
                    
                    indices.push_back(curr + 1);
                    indices.push_back(next);
                    indices.push_back(next + 1);
                }
            }
            
            mesh->SetPositions(positions);
            mesh->SetNormals(normals);
            mesh->SetUVs(uvs);
            mesh->SetIndices(indices);
            mesh->ComputeBoundingBox();
            
            return mesh;
        }

        Mesh::Ptr CreateCylinder(int segments, float height)
        {
            auto mesh = std::make_shared<Mesh>();
            mesh->name = "Cylinder";
            
            std::vector<Vec3> positions;
            std::vector<Vec3> normals;
            std::vector<Vec2> uvs;
            std::vector<uint32_t> indices;
            
            const float radius = 0.5f;
            const float halfHeight = height * 0.5f;
            
            // Side vertices
            for (int i = 0; i <= segments; ++i)
            {
                float angle = static_cast<float>(i) / segments * 2.0f * glm::pi<float>();
                float x = std::cos(angle) * radius;
                float z = std::sin(angle) * radius;
                Vec3 normal(std::cos(angle), 0.0f, std::sin(angle));
                
                positions.push_back({x, halfHeight, z});
                positions.push_back({x, -halfHeight, z});
                normals.push_back(normal);
                normals.push_back(normal);
                uvs.push_back({static_cast<float>(i) / segments, 1.0f});
                uvs.push_back({static_cast<float>(i) / segments, 0.0f});
            }
            
            // Side indices
            for (int i = 0; i < segments; ++i)
            {
                uint32_t base = i * 2;
                indices.push_back(base);
                indices.push_back(base + 1);
                indices.push_back(base + 2);
                indices.push_back(base + 2);
                indices.push_back(base + 1);
                indices.push_back(base + 3);
            }
            
            // Top cap
            uint32_t topCenter = static_cast<uint32_t>(positions.size());
            positions.push_back({0.0f, halfHeight, 0.0f});
            normals.push_back({0.0f, 1.0f, 0.0f});
            uvs.push_back({0.5f, 0.5f});
            
            for (int i = 0; i <= segments; ++i)
            {
                float angle = static_cast<float>(i) / segments * 2.0f * glm::pi<float>();
                float x = std::cos(angle) * radius;
                float z = std::sin(angle) * radius;
                positions.push_back({x, halfHeight, z});
                normals.push_back({0.0f, 1.0f, 0.0f});
                uvs.push_back({std::cos(angle) * 0.5f + 0.5f, std::sin(angle) * 0.5f + 0.5f});
            }
            
            for (int i = 0; i < segments; ++i)
            {
                indices.push_back(topCenter);
                indices.push_back(topCenter + 1 + i);
                indices.push_back(topCenter + 2 + i);
            }
            
            // Bottom cap
            uint32_t bottomCenter = static_cast<uint32_t>(positions.size());
            positions.push_back({0.0f, -halfHeight, 0.0f});
            normals.push_back({0.0f, -1.0f, 0.0f});
            uvs.push_back({0.5f, 0.5f});
            
            for (int i = 0; i <= segments; ++i)
            {
                float angle = static_cast<float>(i) / segments * 2.0f * glm::pi<float>();
                float x = std::cos(angle) * radius;
                float z = std::sin(angle) * radius;
                positions.push_back({x, -halfHeight, z});
                normals.push_back({0.0f, -1.0f, 0.0f});
                uvs.push_back({std::cos(angle) * 0.5f + 0.5f, std::sin(angle) * 0.5f + 0.5f});
            }
            
            for (int i = 0; i < segments; ++i)
            {
                indices.push_back(bottomCenter);
                indices.push_back(bottomCenter + 2 + i);
                indices.push_back(bottomCenter + 1 + i);
            }
            
            mesh->SetPositions(positions);
            mesh->SetNormals(normals);
            mesh->SetUVs(uvs);
            mesh->SetIndices(indices);
            mesh->ComputeBoundingBox();
            
            return mesh;
        }

        Mesh::Ptr CreatePlane(float width, float height, int widthSegments, int heightSegments)
        {
            auto mesh = std::make_shared<Mesh>();
            mesh->name = "Plane";
            
            std::vector<Vec3> positions;
            std::vector<Vec3> normals;
            std::vector<Vec2> uvs;
            std::vector<uint32_t> indices;
            
            float halfW = width * 0.5f;
            float halfH = height * 0.5f;
            
            for (int y = 0; y <= heightSegments; ++y)
            {
                for (int x = 0; x <= widthSegments; ++x)
                {
                    float u = static_cast<float>(x) / widthSegments;
                    float v = static_cast<float>(y) / heightSegments;
                    
                    positions.push_back({u * width - halfW, 0.0f, v * height - halfH});
                    normals.push_back({0.0f, 1.0f, 0.0f});
                    uvs.push_back({u, v});
                }
            }
            
            for (int y = 0; y < heightSegments; ++y)
            {
                for (int x = 0; x < widthSegments; ++x)
                {
                    uint32_t curr = y * (widthSegments + 1) + x;
                    uint32_t next = curr + widthSegments + 1;
                    
                    indices.push_back(curr);
                    indices.push_back(next);
                    indices.push_back(curr + 1);
                    
                    indices.push_back(curr + 1);
                    indices.push_back(next);
                    indices.push_back(next + 1);
                }
            }
            
            mesh->SetPositions(positions);
            mesh->SetNormals(normals);
            mesh->SetUVs(uvs);
            mesh->SetIndices(indices);
            mesh->ComputeBoundingBox();
            
            return mesh;
        }
    }

} // namespace RVX
