#include "Geometry/Asset/Mesh.h"

#include <mikktspace.h>

#include <array>
#include <cmath>
#include <limits>

namespace RVX
{
    namespace
    {
        constexpr float RVX_TANGENT_EPSILON = 1.0e-8f;
        constexpr float RVX_UV_DETERMINANT_EPSILON = 1.0e-12f;
        constexpr float RVX_GEOMETRIC_DEGENERACY_RELATIVE_EPSILON = 1.0e-12f;

        bool IsFiniteVec3(const Vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        Vec3 NormalizeOr(const Vec3& value, const Vec3& fallback)
        {
            if (!IsFiniteVec3(value))
            {
                return fallback;
            }

            const float lengthSq = dot(value, value);
            if (!std::isfinite(lengthSq) || lengthSq <= RVX_TANGENT_EPSILON)
            {
                return fallback;
            }

            const Vec3 normalized = normalize(value);
            return IsFiniteVec3(normalized) ? normalized : fallback;
        }

        Vec3 BuildFallbackTangent(const Vec3& normal)
        {
            const Vec3 n = NormalizeOr(normal, Vec3(0.0f, 0.0f, 1.0f));
            const Vec3 axis = std::abs(n.z) < 0.999f ? Vec3(0.0f, 0.0f, 1.0f)
                                                     : Vec3(0.0f, 1.0f, 0.0f);
            return NormalizeOr(cross(axis, n), Vec3(1.0f, 0.0f, 0.0f));
        }

        bool TryNormalize(const Vec3& value, Vec3& outNormalized)
        {
            if (!IsFiniteVec3(value))
            {
                return false;
            }

            const float lengthSquared = dot(value, value);
            if (!std::isfinite(lengthSquared) ||
                lengthSquared <= RVX_TANGENT_EPSILON)
            {
                return false;
            }

            outNormalized = value * glm::inversesqrt(lengthSquared);
            return IsFiniteVec3(outNormalized);
        }

        bool IsGeometricallyDegenerate(const Vec3& position0,
                                       const Vec3& position1,
                                       const Vec3& position2)
        {
            const Vec3 edge01 = position1 - position0;
            const Vec3 edge02 = position2 - position0;
            const Vec3 edge12 = position2 - position1;
            const float largestEdgeLengthSquared = std::max({
                dot(edge01, edge01),
                dot(edge02, edge02),
                dot(edge12, edge12)});
            const Vec3 geometricNormal = cross(edge01, edge02);
            const float geometricAreaSquared =
                dot(geometricNormal, geometricNormal);
            if (!std::isfinite(largestEdgeLengthSquared) ||
                !std::isfinite(geometricAreaSquared) ||
                largestEdgeLengthSquared <=
                    std::numeric_limits<float>::min())
            {
                return true;
            }

            // This is deliberately scale-relative.  An absolute area test
            // incorrectly rejects valid small meshes such as the Porsche
            // detail geometry, while this still rejects a numerically
            // collapsed triangle.
            return geometricAreaSquared <=
                largestEdgeLengthSquared * largestEdgeLengthSquared *
                    RVX_GEOMETRIC_DEGENERACY_RELATIVE_EPSILON;
        }

        bool TryBuildDegenerateFallbackTangent(const Vec3& normal,
                                               const Vec3& position0,
                                               const Vec3& position1,
                                               const Vec3& position2,
                                               Vec4& outTangent)
        {
            Vec3 normalizedNormal;
            if (!TryNormalize(normal, normalizedNormal))
            {
                return false;
            }

            const std::array<Vec3, 3> edges{
                position1 - position0,
                position2 - position0,
                position2 - position1};
            for (const Vec3& edge : edges)
            {
                const Vec3 planarEdge =
                    edge - normalizedNormal * dot(normalizedNormal, edge);
                Vec3 tangent;
                if (TryNormalize(planarEdge, tangent))
                {
                    outTangent = Vec4(tangent, 1.0f);
                    return true;
                }
            }

            // A collapsed triangle can have no usable edge.  A finite normal
            // still defines one deterministic orthogonal frame; do not
            // fabricate one when that normal itself is invalid.
            Vec3 tangent;
            if (!TryNormalize(BuildFallbackTangent(normalizedNormal), tangent) ||
                std::abs(dot(normalizedNormal, tangent)) > 1.0e-4f)
            {
                return false;
            }

            outTangent = Vec4(tangent, 1.0f);
            return true;
        }

        enum class TangentCornerSplitBuildResult : uint8
        {
            Success,
            VertexLimitExceeded,
            ByteBudgetExceeded,
            BaseVertexUnsupported,
            CopyFailed
        };

        bool CheckedAddSize(size_t left, size_t right, size_t& outValue)
        {
            if (left > std::numeric_limits<size_t>::max() - right)
            {
                return false;
            }
            outValue = left + right;
            return true;
        }

        bool CheckedMultiplySize(size_t left, size_t right, size_t& outValue)
        {
            if (left != 0u &&
                right > std::numeric_limits<size_t>::max() / left)
            {
                return false;
            }
            outValue = left * right;
            return true;
        }

        TangentCornerSplitBuildResult TryBuildTangentCornerSplitMesh(
            const Mesh& source,
            const std::vector<uint32>& sourceIndices,
            uint32 vertexLimit,
            size_t byteLimit,
            Mesh& outMesh)
        {
            if (sourceIndices.empty() ||
                vertexLimit == 0u ||
                sourceIndices.size() > static_cast<size_t>(vertexLimit))
            {
                return TangentCornerSplitBuildResult::VertexLimitExceeded;
            }
            if (!source.ValidateSubMeshes())
            {
                return TangentCornerSplitBuildResult::CopyFailed;
            }
            for (const SubMesh& subMesh : source.GetSubMeshes())
            {
                if (subMesh.baseVertex != 0)
                {
                    return TangentCornerSplitBuildResult::BaseVertexUnsupported;
                }
            }

            const size_t sourceVertexCount = source.GetVertexCount();
            if (sourceVertexCount == 0u ||
                sourceIndices.size() >
                    static_cast<size_t>(std::numeric_limits<uint32>::max()))
            {
                return TangentCornerSplitBuildResult::CopyFailed;
            }

            size_t totalAttributeBytes = 0u;
            for (const auto& [attributeName, attribute] :
                 source.GetAttributes())
            {
                (void)attributeName;
                if (!attribute || attribute->GetVertexCount() !=
                                      sourceVertexCount ||
                    attribute->GetComponents() == 0u ||
                    attribute->GetComponents() > 4u ||
                    !attribute->GetData() || attribute->GetStride() == 0u)
                {
                    return TangentCornerSplitBuildResult::CopyFailed;
                }
                size_t expandedAttributeBytes = 0u;
                if (!CheckedMultiplySize(sourceIndices.size(),
                                         attribute->GetStride(),
                                         expandedAttributeBytes) ||
                    !CheckedAddSize(totalAttributeBytes,
                                    expandedAttributeBytes,
                                    totalAttributeBytes))
                {
                    return TangentCornerSplitBuildResult::ByteBudgetExceeded;
                }
            }

            size_t expandedIndexBytes = 0u;
            size_t requiredWorkingBytes = 0u;
            if (!CheckedMultiplySize(sourceIndices.size(), sizeof(uint32),
                                     expandedIndexBytes) ||
                // Each attribute has a published VertexAttribute payload and
                // one temporary expanded byte vector. Index data exists in the
                // temporary vector and again after SetIndices().
                !CheckedMultiplySize(totalAttributeBytes, 2u,
                                     requiredWorkingBytes) ||
                !CheckedAddSize(requiredWorkingBytes, expandedIndexBytes,
                                requiredWorkingBytes) ||
                !CheckedAddSize(requiredWorkingBytes, expandedIndexBytes,
                                requiredWorkingBytes) ||
                !CheckedAddSize(requiredWorkingBytes, expandedIndexBytes,
                                requiredWorkingBytes) ||
                requiredWorkingBytes > byteLimit)
            {
                return TangentCornerSplitBuildResult::ByteBudgetExceeded;
            }

            try
            {
                outMesh.name = source.name;
                outMesh.SetPrimitiveType(source.GetPrimitiveType());
                for (const auto& [attributeName, attribute] :
                     source.GetAttributes())
                {
                    if (!attribute || attribute->GetVertexCount() !=
                                          sourceVertexCount ||
                        attribute->GetComponents() == 0u ||
                        attribute->GetComponents() > 4u ||
                        !attribute->GetData())
                    {
                        return TangentCornerSplitBuildResult::CopyFailed;
                    }

                    const size_t stride = attribute->GetStride();
                    if (stride == 0u ||
                        sourceIndices.size() >
                            std::numeric_limits<size_t>::max() / stride)
                    {
                        return TangentCornerSplitBuildResult::CopyFailed;
                    }

                    std::vector<uint8> expandedBytes(
                        sourceIndices.size() * stride);
                    const auto* sourceBytes =
                        static_cast<const uint8*>(attribute->GetData());
                    for (size_t corner = 0; corner < sourceIndices.size();
                         ++corner)
                    {
                        const uint32 sourceIndex = sourceIndices[corner];
                        if (sourceIndex >= sourceVertexCount)
                        {
                            return TangentCornerSplitBuildResult::CopyFailed;
                        }
                        std::memcpy(
                            expandedBytes.data() + corner * stride,
                            sourceBytes + static_cast<size_t>(sourceIndex) *
                                              stride,
                            stride);
                    }
                    outMesh.AddAttribute(
                        attributeName,
                        std::make_unique<VertexAttribute>(
                            expandedBytes.data(),
                            sourceIndices.size(),
                            attribute->GetComponents(),
                            attribute->GetType(),
                            attribute->IsNormalized()));
                }

                std::vector<uint32> expandedIndices(sourceIndices.size());
                for (size_t corner = 0; corner < expandedIndices.size();
                     ++corner)
                {
                    expandedIndices[corner] = static_cast<uint32>(corner);
                }
                outMesh.SetIndices(expandedIndices);

                std::vector<SubMesh> splitSubMeshes = source.GetSubMeshes();
                outMesh.SetSubMeshes(std::move(splitSubMeshes));
                if (const std::optional<AABB>& sourceBounds =
                        source.GetBoundingBox();
                    sourceBounds.has_value())
                {
                    outMesh.SetBoundingBox(
                        sourceBounds->GetMin(), sourceBounds->GetMax());
                }
                else if (!outMesh.ComputeBoundingBox())
                {
                    return TangentCornerSplitBuildResult::CopyFailed;
                }
            }
            catch (const std::exception&)
            {
                return TangentCornerSplitBuildResult::CopyFailed;
            }

            return outMesh.GetVertexCount() == sourceIndices.size() &&
                   outMesh.GetIndexCount() == sourceIndices.size() &&
                   outMesh.ValidateSubMeshes()
                ? TangentCornerSplitBuildResult::Success
                : TangentCornerSplitBuildResult::CopyFailed;
        }

        struct MikkMeshData
        {
            const float* positions = nullptr;
            const float* normals = nullptr;
            const float* uvs = nullptr;
            const std::vector<uint32>* indices = nullptr;
            const std::vector<uint32>* originalFaceIndices = nullptr;
            std::vector<Vec4>* tangents = nullptr;
            std::vector<bool>* assigned = nullptr;
            std::vector<Vec4>* cornerTangents = nullptr;
            std::vector<bool>* cornerAssigned = nullptr;
            std::vector<uint32>* invalidOutputFaces = nullptr;
            bool invalidOutput = false;
            bool conflictingCornerBasis = false;
        };

        enum class TangentAssignmentResult : uint8
        {
            Assigned,
            CompatibleDuplicate,
            Invalid,
            Conflict
        };

        TangentAssignmentResult AssignTangent(MikkMeshData& data,
                                              uint32 index,
                                              const Vec3& tangent,
                                              float signValue)
        {
            const float lengthSquared = glm::dot(tangent, tangent);
            if (!IsFiniteVec3(tangent) || !std::isfinite(signValue) ||
                !std::isfinite(lengthSquared) ||
                lengthSquared <= RVX_TANGENT_EPSILON)
            {
                return TangentAssignmentResult::Invalid;
            }

            const Vec3 normalized = tangent * glm::inversesqrt(lengthSquared);
            if (!IsFiniteVec3(normalized))
            {
                return TangentAssignmentResult::Invalid;
            }

            const float sign = signValue < 0.0f ? -1.0f : 1.0f;
            if ((*data.assigned)[index])
            {
                const Vec4& existing = (*data.tangents)[index];
                if (glm::dot(Vec3(existing), normalized) < 0.999f ||
                    existing.w != sign)
                {
                    return TangentAssignmentResult::Conflict;
                }
                return TangentAssignmentResult::CompatibleDuplicate;
            }

            (*data.tangents)[index] = Vec4(normalized, sign);
            (*data.assigned)[index] = true;
            return TangentAssignmentResult::Assigned;
        }

        MikkMeshData& GetMikkMeshData(const SMikkTSpaceContext* context)
        {
            return *static_cast<MikkMeshData*>(context->m_pUserData);
        }

        uint32 GetMikkVertexIndex(const MikkMeshData& data,
                                  int face,
                                  int vertex)
        {
            return (*data.indices)[static_cast<size_t>(face) * 3u +
                                   static_cast<size_t>(vertex)];
        }

        int MikkGetNumFaces(const SMikkTSpaceContext* context)
        {
            return static_cast<int>(GetMikkMeshData(context).indices->size() / 3u);
        }

        int MikkGetNumVerticesOfFace(const SMikkTSpaceContext*, int)
        {
            return 3;
        }

        void MikkGetPosition(const SMikkTSpaceContext* context,
                             float output[],
                             int face,
                             int vertex)
        {
            const MikkMeshData& data = GetMikkMeshData(context);
            const uint32 index = GetMikkVertexIndex(data, face, vertex);
            output[0] = data.positions[index * 3u];
            output[1] = data.positions[index * 3u + 1u];
            output[2] = data.positions[index * 3u + 2u];
        }

        void MikkGetNormal(const SMikkTSpaceContext* context,
                           float output[],
                           int face,
                           int vertex)
        {
            const MikkMeshData& data = GetMikkMeshData(context);
            const uint32 index = GetMikkVertexIndex(data, face, vertex);
            const Vec3 normal = NormalizeOr(
                Vec3(data.normals[index * 3u],
                     data.normals[index * 3u + 1u],
                     data.normals[index * 3u + 2u]),
                Vec3(0.0f, 0.0f, 1.0f));
            output[0] = normal.x;
            output[1] = normal.y;
            output[2] = normal.z;
        }

        void MikkGetTexCoord(const SMikkTSpaceContext* context,
                             float output[],
                             int face,
                             int vertex)
        {
            const MikkMeshData& data = GetMikkMeshData(context);
            const uint32 index = GetMikkVertexIndex(data, face, vertex);
            output[0] = data.uvs[index * 2u];
            output[1] = data.uvs[index * 2u + 1u];
        }

        void MikkSetTSpaceBasic(const SMikkTSpaceContext* context,
                                const float tangentValue[],
                                float signValue,
                                int face,
                                int vertex)
        {
            MikkMeshData& data = GetMikkMeshData(context);
            const uint32 index = GetMikkVertexIndex(data, face, vertex);
            const Vec3 tangent(
                tangentValue[0], tangentValue[1], tangentValue[2]);
            const float lengthSquared = glm::dot(tangent, tangent);
            if (IsFiniteVec3(tangent) && std::isfinite(signValue) &&
                std::isfinite(lengthSquared) &&
                lengthSquared > RVX_TANGENT_EPSILON &&
                data.cornerTangents != nullptr &&
                data.cornerAssigned != nullptr &&
                data.originalFaceIndices != nullptr && face >= 0 &&
                vertex >= 0 && vertex < 3 &&
                static_cast<size_t>(face) < data.originalFaceIndices->size())
            {
                const uint32 originalFace = (*data.originalFaceIndices)[
                    static_cast<size_t>(face)];
                const size_t cornerIndex =
                    static_cast<size_t>(originalFace) * 3u +
                    static_cast<size_t>(vertex);
                if (cornerIndex < data.cornerTangents->size() &&
                    cornerIndex < data.cornerAssigned->size())
                {
                    const Vec3 normalized = tangent *
                        glm::inversesqrt(lengthSquared);
                    if (IsFiniteVec3(normalized))
                    {
                        (*data.cornerTangents)[cornerIndex] = Vec4(
                            normalized,
                            signValue < 0.0f ? -1.0f : 1.0f);
                        (*data.cornerAssigned)[cornerIndex] = true;
                    }
                }
            }
            switch (AssignTangent(data, index, tangent, signValue))
            {
                case TangentAssignmentResult::Invalid:
                    data.invalidOutput = true;
                    if (data.invalidOutputFaces != nullptr &&
                        data.originalFaceIndices != nullptr &&
                        face >= 0 &&
                        static_cast<size_t>(face) <
                            data.originalFaceIndices->size())
                    {
                        data.invalidOutputFaces->push_back(
                            (*data.originalFaceIndices)[
                                static_cast<size_t>(face)]);
                    }
                    break;
                case TangentAssignmentResult::Conflict:
                    // One indexed vertex cannot represent distinct per-corner
                    // tangent frames. The source must split its UV/normal seam.
                    data.conflictingCornerBasis = true;
                    break;
                case TangentAssignmentResult::Assigned:
                case TangentAssignmentResult::CompatibleDuplicate:
                    break;
            }
        }
    } // namespace

    const char* GetTangentGenerationFailureName(
        TangentGenerationFailure failure)
    {
        switch (failure)
        {
            case TangentGenerationFailure::None:
                return "none";
            case TangentGenerationFailure::InvalidUVSet:
                return "invalid-uv-set";
            case TangentGenerationFailure::InvalidMeshLayout:
                return "invalid-mesh-layout";
            case TangentGenerationFailure::InvalidIndex:
                return "invalid-index";
            case TangentGenerationFailure::InvalidVertexData:
                return "invalid-vertex-data";
            case TangentGenerationFailure::MikkGenerationFailed:
                return "mikk-generation-failed";
            case TangentGenerationFailure::MikkInvalidOutput:
                return "mikk-invalid-output";
            case TangentGenerationFailure::ConflictingCornerBasis:
                return "conflicting-corner-basis";
            case TangentGenerationFailure::TopologySplitLimitExceeded:
                return "topology-split-limit-exceeded";
            case TangentGenerationFailure::TopologySplitByteBudgetExceeded:
                return "topology-split-byte-budget-exceeded";
            case TangentGenerationFailure::TopologySplitBaseVertexUnsupported:
                return "topology-split-base-vertex-unsupported";
            case TangentGenerationFailure::TopologySplitCopyFailed:
                return "topology-split-copy-failed";
            case TangentGenerationFailure::UnrecoverableDegenerateBasis:
                return "unrecoverable-degenerate-basis";
        }
        return "unknown";
    }

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
        m_boundingBox = AABB(min, max);
    }

    bool Mesh::ComputeBoundingBox()
    {
        const VertexAttribute* posAttr = GetAttribute(VertexBufferNames::Position);
        if (!posAttr || posAttr->GetVertexCount() == 0)
        {
            return false;
        }

        AABB bbox;
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
        std::vector<uint32_t> indices = GetIndices32();

        for (auto& sm : m_subMeshes)
        {
            if (sm.indexOffset + sm.indexCount > indices.size())
            {
                continue;
            }

            AABB bounds;
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

        std::vector<uint32_t> indices = GetIndices32();
        std::vector<Vec3> normals = ComputeVertexNormalsTriList(positions, indices);

        if (normals.empty())
        {
            return false;
        }

        SetNormals(normals);
        return true;
    }

    bool Mesh::GenerateFlatNormalsTriList()
    {
        const VertexAttribute* positionAttribute =
            GetAttribute(VertexBufferNames::Position);
        const std::vector<uint32> sourceIndices = GetIndices32();
        if (!positionAttribute ||
            positionAttribute->GetType() != AttributeType::Float ||
            positionAttribute->GetComponents() != 3u ||
            positionAttribute->GetVertexCount() == 0u ||
            !m_subMeshes.empty() ||
            m_primitiveType != PrimitiveType::Triangles ||
            sourceIndices.empty() || sourceIndices.size() % 3u != 0u)
        {
            return false;
        }

        const size_t sourceVertexCount = positionAttribute->GetVertexCount();
        const float* sourcePositions =
            static_cast<const float*>(positionAttribute->GetData());
        if (!sourcePositions)
        {
            return false;
        }

        std::vector<Vec3> flatNormals(sourceIndices.size());
        for (size_t triangle = 0; triangle < sourceIndices.size(); triangle += 3u)
        {
            const uint32 index0 = sourceIndices[triangle];
            const uint32 index1 = sourceIndices[triangle + 1u];
            const uint32 index2 = sourceIndices[triangle + 2u];
            if (index0 >= sourceVertexCount || index1 >= sourceVertexCount ||
                index2 >= sourceVertexCount)
            {
                return false;
            }

            const Vec3 position0(
                sourcePositions[index0 * 3u],
                sourcePositions[index0 * 3u + 1u],
                sourcePositions[index0 * 3u + 2u]);
            const Vec3 position1(
                sourcePositions[index1 * 3u],
                sourcePositions[index1 * 3u + 1u],
                sourcePositions[index1 * 3u + 2u]);
            const Vec3 position2(
                sourcePositions[index2 * 3u],
                sourcePositions[index2 * 3u + 1u],
                sourcePositions[index2 * 3u + 2u]);
            if (!IsFiniteVec3(position0) || !IsFiniteVec3(position1) ||
                !IsFiniteVec3(position2))
            {
                return false;
            }

            const Vec3 faceNormal = cross(
                position1 - position0, position2 - position0);
            const float lengthSquared = dot(faceNormal, faceNormal);
            if (!std::isfinite(lengthSquared) ||
                lengthSquared <= RVX_TANGENT_EPSILON)
            {
                return false;
            }
            const Vec3 normalized = faceNormal * glm::inversesqrt(lengthSquared);
            flatNormals[triangle] = normalized;
            flatNormals[triangle + 1u] = normalized;
            flatNormals[triangle + 2u] = normalized;
        }

        std::unordered_map<std::string, std::unique_ptr<VertexAttribute>>
            expandedAttributes;
        expandedAttributes.reserve(m_attributes.size() + 1u);
        for (const auto& [attributeName, attribute] : m_attributes)
        {
            if (!attribute || attributeName == VertexBufferNames::Normal ||
                attributeName == VertexBufferNames::Tangent)
            {
                continue;
            }
            if (attribute->GetVertexCount() != sourceVertexCount ||
                attribute->GetComponents() == 0u ||
                attribute->GetComponents() > 4u || !attribute->GetData())
            {
                return false;
            }

            const size_t stride = attribute->GetStride();
            if (stride == 0u ||
                sourceIndices.size() >
                    std::numeric_limits<size_t>::max() / stride)
            {
                return false;
            }
            std::vector<uint8> expandedBytes(sourceIndices.size() * stride);
            const auto* sourceBytes =
                static_cast<const uint8*>(attribute->GetData());
            for (size_t corner = 0; corner < sourceIndices.size(); ++corner)
            {
                std::memcpy(
                    expandedBytes.data() + corner * stride,
                    sourceBytes + static_cast<size_t>(sourceIndices[corner]) *
                                      stride,
                    stride);
            }
            expandedAttributes.emplace(
                attributeName,
                std::make_unique<VertexAttribute>(
                    expandedBytes.data(),
                    sourceIndices.size(),
                    attribute->GetComponents(),
                    attribute->GetType(),
                    attribute->IsNormalized()));
        }
        expandedAttributes.emplace(
            VertexBufferNames::Normal,
            std::make_unique<VertexAttribute>(flatNormals));

        if (sourceIndices.size() >
            static_cast<size_t>(std::numeric_limits<uint32>::max()) + 1u)
        {
            return false;
        }
        std::vector<uint32> expandedIndices(sourceIndices.size());
        for (size_t index = 0; index < expandedIndices.size(); ++index)
        {
            expandedIndices[index] = static_cast<uint32>(index);
        }

        m_attributes = std::move(expandedAttributes);
        m_vertexCount = sourceIndices.size();
        SetIndices(expandedIndices);
        m_boundingBox.reset();
        return true;
    }

    bool Mesh::GenerateTangents(uint32 uvSet)
    {
        return GenerateTangentsDetailed(uvSet).success;
    }

    TangentGenerationResult Mesh::GenerateTangentsDetailed(
        uint32 uvSet,
        bool allowTopologySplit,
        uint32 topologySplitVertexLimit,
        size_t topologySplitByteLimit)
    {
        TangentGenerationResult result;
        const auto fail = [&result](TangentGenerationFailure failure)
        {
            result.failure = failure;
            return result;
        };

        if (uvSet > 1u)
        {
            return fail(TangentGenerationFailure::InvalidUVSet);
        }
        const std::string uvAttributeName = uvSet == 1u
            ? VertexBufferNames::UV1
            : VertexBufferNames::UV;
        const VertexAttribute* posAttr = GetAttribute(VertexBufferNames::Position);
        const VertexAttribute* normAttr = GetAttribute(VertexBufferNames::Normal);
        const VertexAttribute* uvAttr = GetAttribute(uvAttributeName);

        if (!posAttr || !normAttr || !uvAttr || m_indexCount == 0 ||
            m_primitiveType != PrimitiveType::Triangles ||
            posAttr->GetType() != AttributeType::Float ||
            normAttr->GetType() != AttributeType::Float ||
            uvAttr->GetType() != AttributeType::Float ||
            posAttr->GetComponents() != 3u ||
            normAttr->GetComponents() != 3u ||
            uvAttr->GetComponents() != 2u ||
            posAttr->GetVertexCount() == 0u ||
            normAttr->GetVertexCount() != posAttr->GetVertexCount() ||
            uvAttr->GetVertexCount() != posAttr->GetVertexCount())
        {
            return fail(TangentGenerationFailure::InvalidMeshLayout);
        }

        const size_t vertexCount = posAttr->GetVertexCount();
        if (vertexCount > static_cast<size_t>(std::numeric_limits<uint32>::max()))
        {
            return fail(TangentGenerationFailure::InvalidMeshLayout);
        }
        result.originalVertexCount = static_cast<uint32>(vertexCount);
        result.publishedVertexCount = static_cast<uint32>(vertexCount);
        const float* positions = static_cast<const float*>(posAttr->GetData());
        const float* normals = static_cast<const float*>(normAttr->GetData());
        const float* uvs = static_cast<const float*>(uvAttr->GetData());
        const std::vector<uint32> indices = GetIndices32();
        if (!positions || !normals || !uvs || indices.empty() ||
            indices.size() % 3u != 0u ||
            indices.size() / 3u > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return fail(TangentGenerationFailure::InvalidMeshLayout);
        }

        result.triangleCount =
            static_cast<uint32>(indices.size() / 3u);

        std::vector<bool> referenced(vertexCount, false);
        for (uint32 index : indices)
        {
            if (index >= vertexCount)
            {
                return fail(TangentGenerationFailure::InvalidIndex);
            }
            referenced[index] = true;
            const Vec3 position(
                positions[index * 3u],
                positions[index * 3u + 1u],
                positions[index * 3u + 2u]);
            const Vec3 normal(
                normals[index * 3u],
                normals[index * 3u + 1u],
                normals[index * 3u + 2u]);
            const Vec2 uv(uvs[index * 2u], uvs[index * 2u + 1u]);
            if (!IsFiniteVec3(position) || !IsFiniteVec3(normal) ||
                !std::isfinite(uv.x) || !std::isfinite(uv.y) ||
                glm::dot(normal, normal) <= RVX_TANGENT_EPSILON)
            {
                return fail(TangentGenerationFailure::InvalidVertexData);
            }
        }

        std::vector<bool> fallbackFace(result.triangleCount, false);
        std::vector<bool> mikkRejectedFace(result.triangleCount, false);
        std::vector<uint32> mikkIndices;
        std::vector<uint32> mikkOriginalFaces;
        mikkIndices.reserve(indices.size());
        mikkOriginalFaces.reserve(result.triangleCount);
        for (size_t triangle = 0; triangle < indices.size(); triangle += 3u)
        {
            const uint32 index0 = indices[triangle];
            const uint32 index1 = indices[triangle + 1u];
            const uint32 index2 = indices[triangle + 2u];
            const Vec3 position0(
                positions[index0 * 3u],
                positions[index0 * 3u + 1u],
                positions[index0 * 3u + 2u]);
            const Vec3 position1(
                positions[index1 * 3u],
                positions[index1 * 3u + 1u],
                positions[index1 * 3u + 2u]);
            const Vec3 position2(
                positions[index2 * 3u],
                positions[index2 * 3u + 1u],
                positions[index2 * 3u + 2u]);
            const Vec2 uv0(uvs[index0 * 2u], uvs[index0 * 2u + 1u]);
            const Vec2 uv1(uvs[index1 * 2u], uvs[index1 * 2u + 1u]);
            const Vec2 uv2(uvs[index2 * 2u], uvs[index2 * 2u + 1u]);
            const Vec2 uvEdge1 = uv1 - uv0;
            const Vec2 uvEdge2 = uv2 - uv0;
            const float uvDeterminant =
                uvEdge1.x * uvEdge2.y - uvEdge1.y * uvEdge2.x;
            if (!std::isfinite(uvDeterminant))
            {
                return fail(TangentGenerationFailure::InvalidVertexData);
            }

            const bool geometricDegenerate = IsGeometricallyDegenerate(
                position0, position1, position2);
            const bool uvDegenerate =
                std::abs(uvDeterminant) <= RVX_UV_DETERMINANT_EPSILON;
            if (geometricDegenerate || uvDegenerate)
            {
                const size_t face = triangle / 3u;
                fallbackFace[face] = true;
                ++result.fallbackTriangleCount;
                if (geometricDegenerate)
                {
                    ++result.degenerateGeometryTriangleCount;
                }
                if (uvDegenerate)
                {
                    ++result.degenerateUVTriangleCount;
                }
                continue;
            }

            mikkIndices.push_back(index0);
            mikkIndices.push_back(index1);
            mikkIndices.push_back(index2);
            mikkOriginalFaces.push_back(
                static_cast<uint32>(triangle / 3u));
            ++result.mikkTriangleCount;
        }

        std::vector<Vec4> finalTangents(vertexCount, Vec4(0.0f));
        std::vector<bool> assigned(vertexCount, false);
        std::vector<Vec4> cornerTangents(indices.size(), Vec4(0.0f));
        std::vector<bool> cornerAssigned(indices.size(), false);
        MikkMeshData data;
        std::vector<uint32> invalidOutputFaces;
        data.positions = positions;
        data.normals = normals;
        data.uvs = uvs;
        data.indices = &mikkIndices;
        data.originalFaceIndices = &mikkOriginalFaces;
        data.tangents = &finalTangents;
        data.assigned = &assigned;
        data.cornerTangents = &cornerTangents;
        data.cornerAssigned = &cornerAssigned;
        data.invalidOutputFaces = &invalidOutputFaces;
        if (!mikkIndices.empty())
        {
            SMikkTSpaceInterface interface{
                MikkGetNumFaces,
                MikkGetNumVerticesOfFace,
                MikkGetPosition,
                MikkGetNormal,
                MikkGetTexCoord,
                MikkSetTSpaceBasic,
                nullptr};
            const SMikkTSpaceContext context{&interface, &data};
            const int mikkGenerated = genTangSpaceDefault(&context);
            const TangentGenerationFailure mikkGenerationFailure =
                ClassifyMikkGenerationResult(mikkGenerated != 0);
            if (mikkGenerationFailure != TangentGenerationFailure::None)
            {
                return fail(mikkGenerationFailure);
            }
            const auto recordMikkRejectedFaces = [&]()
            {
                bool markedFallbackFace = false;
                for (uint32 face : invalidOutputFaces)
                {
                    if (face >= fallbackFace.size())
                    {
                        return false;
                    }
                    if (!fallbackFace[face])
                    {
                        fallbackFace[face] = true;
                        mikkRejectedFace[face] = true;
                        ++result.fallbackTriangleCount;
                        ++result.mikkRejectedTriangleCount;
                        if (result.firstMikkRejectedTriangle ==
                            RVX_INVALID_INDEX)
                        {
                            result.firstMikkRejectedTriangle = face;
                        }
                        markedFallbackFace = true;
                    }
                }
                return markedFallbackFace;
            };
            if (data.conflictingCornerBasis)
            {
                if (allowTopologySplit)
                {
                    if (data.invalidOutput && !recordMikkRejectedFaces())
                    {
                        return fail(
                            TangentGenerationFailure::MikkInvalidOutput);
                    }

                    Mesh splitMesh;
                    const TangentCornerSplitBuildResult splitBuild =
                        TryBuildTangentCornerSplitMesh(
                            *this,
                            indices,
                            topologySplitVertexLimit,
                            topologySplitByteLimit,
                            splitMesh);
                    switch (splitBuild)
                    {
                        case TangentCornerSplitBuildResult::VertexLimitExceeded:
                            return fail(
                                TangentGenerationFailure::TopologySplitLimitExceeded);
                        case TangentCornerSplitBuildResult::ByteBudgetExceeded:
                            return fail(
                                TangentGenerationFailure::TopologySplitByteBudgetExceeded);
                        case TangentCornerSplitBuildResult::BaseVertexUnsupported:
                            return fail(
                                TangentGenerationFailure::TopologySplitBaseVertexUnsupported);
                        case TangentCornerSplitBuildResult::CopyFailed:
                            return fail(
                                TangentGenerationFailure::TopologySplitCopyFailed);
                        case TangentCornerSplitBuildResult::Success:
                            break;
                    }

                    std::vector<Vec4> splitTangents(indices.size(), Vec4(0.0f));
                    for (size_t face = 0; face < fallbackFace.size(); ++face)
                    {
                        const size_t triangle = face * 3u;
                        const uint32 index0 = indices[triangle];
                        const uint32 index1 = indices[triangle + 1u];
                        const uint32 index2 = indices[triangle + 2u];
                        const std::array<uint32, 3> cornerIndices{
                            index0, index1, index2};
                        const std::array<Vec3, 3> positionsByCorner{
                            Vec3(positions[index0 * 3u], positions[index0 * 3u + 1u],
                                 positions[index0 * 3u + 2u]),
                            Vec3(positions[index1 * 3u], positions[index1 * 3u + 1u],
                                 positions[index1 * 3u + 2u]),
                            Vec3(positions[index2 * 3u], positions[index2 * 3u + 1u],
                                 positions[index2 * 3u + 2u])};
                        for (size_t corner = 0; corner < cornerIndices.size(); ++corner)
                        {
                            const size_t cornerOffset = triangle + corner;
                            if (cornerAssigned[cornerOffset])
                            {
                                splitTangents[cornerOffset] =
                                    cornerTangents[cornerOffset];
                                continue;
                            }
                            if (!fallbackFace[face] &&
                                !mikkRejectedFace[face])
                            {
                                return fail(
                                    TangentGenerationFailure::MikkInvalidOutput);
                            }
                            const uint32 index = cornerIndices[corner];
                            const Vec3 normal(
                                normals[index * 3u], normals[index * 3u + 1u],
                                normals[index * 3u + 2u]);
                            if (!TryBuildDegenerateFallbackTangent(
                                    normal,
                                    positionsByCorner[corner],
                                    positionsByCorner[(corner + 1u) % 3u],
                                    positionsByCorner[(corner + 2u) % 3u],
                                    splitTangents[cornerOffset]))
                            {
                                return fail(
                                    TangentGenerationFailure::UnrecoverableDegenerateBasis);
                            }
                            ++result.fallbackCornerCount;
                        }
                    }

                    splitMesh.SetTangents(splitTangents);
                    *this = std::move(splitMesh);
                    result.topologySplit = true;
                    result.originalVertexCount =
                        static_cast<uint32>(vertexCount);
                    result.publishedVertexCount =
                        static_cast<uint32>(GetVertexCount());
                    result.splitVertexCount =
                        result.publishedVertexCount -
                        result.originalVertexCount;
                    result.success = true;
                    return result;
                }
                return fail(TangentGenerationFailure::ConflictingCornerBasis);
            }
            if (data.invalidOutput)
            {
                return fail(TangentGenerationFailure::MikkInvalidOutput);
            }
        }

        for (size_t face = 0; face < fallbackFace.size(); ++face)
        {
            if (!fallbackFace[face])
            {
                continue;
            }

            const size_t triangle = face * 3u;
            const uint32 index0 = indices[triangle];
            const uint32 index1 = indices[triangle + 1u];
            const uint32 index2 = indices[triangle + 2u];
            const std::array<uint32, 3> cornerIndices{
                index0, index1, index2};
            const std::array<Vec3, 3> positionsByCorner{
                Vec3(positions[index0 * 3u], positions[index0 * 3u + 1u],
                     positions[index0 * 3u + 2u]),
                Vec3(positions[index1 * 3u], positions[index1 * 3u + 1u],
                     positions[index1 * 3u + 2u]),
                Vec3(positions[index2 * 3u], positions[index2 * 3u + 1u],
                     positions[index2 * 3u + 2u])};
            for (size_t corner = 0; corner < cornerIndices.size(); ++corner)
            {
                const uint32 index = cornerIndices[corner];
                if (mikkRejectedFace[face] && assigned[index])
                {
                    continue;
                }
                const Vec3 normal(
                    normals[index * 3u], normals[index * 3u + 1u],
                    normals[index * 3u + 2u]);
                Vec4 fallbackTangent;
                if (!TryBuildDegenerateFallbackTangent(
                        normal,
                        positionsByCorner[corner],
                        positionsByCorner[(corner + 1u) % 3u],
                        positionsByCorner[(corner + 2u) % 3u],
                        fallbackTangent))
                {
                    return fail(
                        TangentGenerationFailure::UnrecoverableDegenerateBasis);
                }

                const TangentAssignmentResult assignment = AssignTangent(
                    data, index, Vec3(fallbackTangent), fallbackTangent.w);
                if (assignment == TangentAssignmentResult::Invalid)
                {
                    return fail(
                        TangentGenerationFailure::UnrecoverableDegenerateBasis);
                }
                if (assignment == TangentAssignmentResult::Conflict)
                {
                    return fail(TangentGenerationFailure::ConflictingCornerBasis);
                }
                ++result.fallbackCornerCount;
            }
        }

        for (size_t index = 0; index < vertexCount; ++index)
        {
            if (referenced[index] && !assigned[index])
            {
                return fail(TangentGenerationFailure::MikkGenerationFailed);
            }
            if (!referenced[index])
            {
                const Vec3 normal(
                    normals[index * 3u],
                    normals[index * 3u + 1u],
                    normals[index * 3u + 2u]);
                Vec4 fallbackTangent;
                if (!TryBuildDegenerateFallbackTangent(
                        normal, Vec3(0.0f), Vec3(0.0f), Vec3(0.0f),
                        fallbackTangent))
                {
                    return fail(
                        TangentGenerationFailure::UnrecoverableDegenerateBasis);
                }
                finalTangents[index] = fallbackTangent;
            }
        }
        SetTangents(finalTangents);
        result.success = true;
        return result;
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
