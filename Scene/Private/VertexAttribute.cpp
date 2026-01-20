#include "Scene/VertexAttribute.h"
#include <cstring>

namespace RVX
{
    // =========================================================================
    // VertexAttribute Implementation
    // =========================================================================

    VertexAttribute::VertexAttribute(const void* data, size_t vertexCount, size_t components,
                                     AttributeType type, bool normalized)
        : m_data(nullptr)
        , m_vertexCount(vertexCount)
        , m_components(components)
        , m_type(type)
        , m_normalized(normalized)
    {
        size_t elementSize = GetAttributeTypeSize(type);
        size_t totalSize = vertexCount * components * elementSize;
        
        if (totalSize > 0 && data)
        {
            m_data = std::malloc(totalSize);
            if (!m_data)
            {
                throw std::bad_alloc();
            }
            std::memcpy(m_data, data, totalSize);
        }
    }

    VertexAttribute::VertexAttribute(const std::vector<Vec2>& data, bool normalized)
        : m_vertexCount(data.size())
        , m_components(2)
        , m_type(AttributeType::Float)
        , m_normalized(normalized)
        , m_data(nullptr)
    {
        size_t totalSize = data.size() * sizeof(Vec2);
        if (totalSize > 0)
        {
            m_data = std::malloc(totalSize);
            if (!m_data) throw std::bad_alloc();
            std::memcpy(m_data, data.data(), totalSize);
        }
    }

    VertexAttribute::VertexAttribute(const std::vector<Vec3>& data, bool normalized)
        : m_vertexCount(data.size())
        , m_components(3)
        , m_type(AttributeType::Float)
        , m_normalized(normalized)
        , m_data(nullptr)
    {
        size_t totalSize = data.size() * sizeof(Vec3);
        if (totalSize > 0)
        {
            m_data = std::malloc(totalSize);
            if (!m_data) throw std::bad_alloc();
            std::memcpy(m_data, data.data(), totalSize);
        }
    }

    VertexAttribute::VertexAttribute(const std::vector<Vec4>& data, bool normalized)
        : m_vertexCount(data.size())
        , m_components(4)
        , m_type(AttributeType::Float)
        , m_normalized(normalized)
        , m_data(nullptr)
    {
        size_t totalSize = data.size() * sizeof(Vec4);
        if (totalSize > 0)
        {
            m_data = std::malloc(totalSize);
            if (!m_data) throw std::bad_alloc();
            std::memcpy(m_data, data.data(), totalSize);
        }
    }

    VertexAttribute::VertexAttribute(const std::vector<IVec2>& data, bool normalized)
        : m_vertexCount(data.size())
        , m_components(2)
        , m_type(AttributeType::Int)
        , m_normalized(normalized)
        , m_data(nullptr)
    {
        size_t totalSize = data.size() * sizeof(IVec2);
        if (totalSize > 0)
        {
            m_data = std::malloc(totalSize);
            if (!m_data) throw std::bad_alloc();
            std::memcpy(m_data, data.data(), totalSize);
        }
    }

    VertexAttribute::VertexAttribute(const std::vector<IVec3>& data, bool normalized)
        : m_vertexCount(data.size())
        , m_components(3)
        , m_type(AttributeType::Int)
        , m_normalized(normalized)
        , m_data(nullptr)
    {
        size_t totalSize = data.size() * sizeof(IVec3);
        if (totalSize > 0)
        {
            m_data = std::malloc(totalSize);
            if (!m_data) throw std::bad_alloc();
            std::memcpy(m_data, data.data(), totalSize);
        }
    }

    VertexAttribute::VertexAttribute(const std::vector<IVec4>& data, bool normalized)
        : m_vertexCount(data.size())
        , m_components(4)
        , m_type(AttributeType::Int)
        , m_normalized(normalized)
        , m_data(nullptr)
    {
        size_t totalSize = data.size() * sizeof(IVec4);
        if (totalSize > 0)
        {
            m_data = std::malloc(totalSize);
            if (!m_data) throw std::bad_alloc();
            std::memcpy(m_data, data.data(), totalSize);
        }
    }

    VertexAttribute::~VertexAttribute()
    {
        if (m_data)
        {
            std::free(m_data);
            m_data = nullptr;
        }
    }

    VertexAttribute::VertexAttribute(VertexAttribute&& other) noexcept
        : m_data(other.m_data)
        , m_vertexCount(other.m_vertexCount)
        , m_components(other.m_components)
        , m_type(other.m_type)
        , m_normalized(other.m_normalized)
    {
        other.m_data = nullptr;
        other.m_vertexCount = 0;
    }

    VertexAttribute& VertexAttribute::operator=(VertexAttribute&& other) noexcept
    {
        if (this != &other)
        {
            if (m_data)
            {
                std::free(m_data);
            }
            m_data = other.m_data;
            m_vertexCount = other.m_vertexCount;
            m_components = other.m_components;
            m_type = other.m_type;
            m_normalized = other.m_normalized;
            
            other.m_data = nullptr;
            other.m_vertexCount = 0;
        }
        return *this;
    }

    bool VertexAttribute::IsValid() const
    {
        return m_data != nullptr && m_vertexCount > 0 && m_components > 0;
    }

    std::unique_ptr<VertexAttribute> VertexAttribute::Clone() const
    {
        return std::make_unique<VertexAttribute>(m_data, m_vertexCount, m_components, m_type, m_normalized);
    }

    // =========================================================================
    // AttributeFactory Implementation
    // =========================================================================

    namespace AttributeFactory
    {
        std::unique_ptr<VertexAttribute> CreatePositions(const std::vector<Vec3>& positions)
        {
            return std::make_unique<VertexAttribute>(positions, false);
        }

        std::unique_ptr<VertexAttribute> CreateNormals(const std::vector<Vec3>& normals)
        {
            return std::make_unique<VertexAttribute>(normals, false);
        }

        std::unique_ptr<VertexAttribute> CreateUVs(const std::vector<Vec2>& uvs)
        {
            return std::make_unique<VertexAttribute>(uvs, false);
        }

        std::unique_ptr<VertexAttribute> CreateColors(const std::vector<Vec4>& colors)
        {
            return std::make_unique<VertexAttribute>(colors, false);
        }

        std::unique_ptr<VertexAttribute> CreateTangents(const std::vector<Vec4>& tangents)
        {
            return std::make_unique<VertexAttribute>(tangents, false);
        }

        std::unique_ptr<VertexAttribute> CreateBoneIndices(const std::vector<IVec4>& boneIndices)
        {
            return std::make_unique<VertexAttribute>(boneIndices, false);
        }

        std::unique_ptr<VertexAttribute> CreateBoneWeights(const std::vector<Vec4>& boneWeights)
        {
            return std::make_unique<VertexAttribute>(boneWeights, false);
        }
    }

} // namespace RVX
