#pragma once

/**
 * @file VertexAttribute.h
 * @brief Type-safe vertex attribute storage
 * 
 * Migrated from found::model::VertexAttribute
 */

#include "Core/MathTypes.h"
#include <vector>
#include <memory>
#include <string>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <cstring>

namespace RVX
{
    // =========================================================================
    // Vertex Buffer Attribute Names
    // =========================================================================
    namespace VertexBufferNames
    {
        static constexpr const char* Position = "position";
        static constexpr const char* Normal = "normal";
        static constexpr const char* Tangent = "tangent";
        static constexpr const char* Color = "color";
        static constexpr const char* UV = "uv";
        static constexpr const char* UV0 = "uv0";
        static constexpr const char* UV1 = "uv1";
        static constexpr const char* BoneIndices = "bone_indices";
        static constexpr const char* BoneWeights = "bone_weights";
    }

    /**
     * @brief Supported vertex attribute data types
     */
    enum class AttributeType
    {
        Float,      // 32-bit floating point
        Int,        // 32-bit signed integer
        UInt,       // 32-bit unsigned integer
        Short,      // 16-bit signed integer
        UShort,     // 16-bit unsigned integer
        Byte,       // 8-bit signed integer
        UByte       // 8-bit unsigned integer
    };

    /**
     * @brief Get the size in bytes of an AttributeType
     */
    inline size_t GetAttributeTypeSize(AttributeType type)
    {
        switch (type)
        {
            case AttributeType::Float:  return sizeof(float);
            case AttributeType::Int:    return sizeof(int32_t);
            case AttributeType::UInt:   return sizeof(uint32_t);
            case AttributeType::Short:  return sizeof(int16_t);
            case AttributeType::UShort: return sizeof(uint16_t);
            case AttributeType::Byte:   return sizeof(int8_t);
            case AttributeType::UByte:  return sizeof(uint8_t);
            default: return 0;
        }
    }

    /**
     * @brief Get the string name of an AttributeType
     */
    inline const char* AttributeTypeToString(AttributeType type)
    {
        switch (type)
        {
            case AttributeType::Float:  return "Float";
            case AttributeType::Int:    return "Int";
            case AttributeType::UInt:   return "UInt";
            case AttributeType::Short:  return "Short";
            case AttributeType::UShort: return "UShort";
            case AttributeType::Byte:   return "Byte";
            case AttributeType::UByte:  return "UByte";
            default: return "Unknown";
        }
    }

    /**
     * @brief Vertex attribute class - stores a single vertex attribute
     * 
     * Features:
     * - Type-safe storage with compile-time type checking
     * - Memory-safe with RAII semantics
     * - Supports move semantics for efficient transfers
     * - Supports arbitrary component counts and data types
     */
    class VertexAttribute
    {
    public:
        // =====================================================================
        // Constructors
        // =====================================================================

        /**
         * @brief Construct from raw data
         * @param data Raw data pointer
         * @param vertexCount Number of vertices
         * @param components Components per vertex (1-4)
         * @param type Data type
         * @param normalized Whether data should be normalized when accessed
         */
        VertexAttribute(const void* data, size_t vertexCount, size_t components,
                       AttributeType type, bool normalized = false);

        /**
         * @brief Template constructor from std::vector
         */
        template<typename T>
        VertexAttribute(const std::vector<T>& data, size_t components, bool normalized = false)
            : m_vertexCount(data.size() / components)
            , m_components(components)
            , m_type(DeduceAttributeType<T>())
            , m_normalized(normalized)
            , m_data(nullptr)
        {
            static_assert(std::is_arithmetic_v<T>, "Attribute data type must be arithmetic");
            
            if (data.size() % components != 0)
            {
                throw std::invalid_argument("Data size must be divisible by component count");
            }
            
            size_t totalSize = data.size() * sizeof(T);
            m_data = std::malloc(totalSize);
            if (!m_data)
            {
                throw std::bad_alloc();
            }
            std::memcpy(m_data, data.data(), totalSize);
        }

        // Convenience constructors for GLM types
        explicit VertexAttribute(const std::vector<Vec2>& data, bool normalized = false);
        explicit VertexAttribute(const std::vector<Vec3>& data, bool normalized = false);
        explicit VertexAttribute(const std::vector<Vec4>& data, bool normalized = false);
        explicit VertexAttribute(const std::vector<IVec2>& data, bool normalized = false);
        explicit VertexAttribute(const std::vector<IVec3>& data, bool normalized = false);
        explicit VertexAttribute(const std::vector<IVec4>& data, bool normalized = false);

        ~VertexAttribute();

        // Move semantics
        VertexAttribute(VertexAttribute&& other) noexcept;
        VertexAttribute& operator=(VertexAttribute&& other) noexcept;

        // Disable copy (avoid accidental expensive copies)
        VertexAttribute(const VertexAttribute&) = delete;
        VertexAttribute& operator=(const VertexAttribute&) = delete;

        // =====================================================================
        // Accessors
        // =====================================================================
        
        const void* GetData() const { return m_data; }
        size_t GetVertexCount() const { return m_vertexCount; }
        size_t GetComponents() const { return m_components; }
        AttributeType GetType() const { return m_type; }
        bool IsNormalized() const { return m_normalized; }
        
        size_t GetElementSize() const { return GetAttributeTypeSize(m_type); }
        size_t GetStride() const { return m_components * GetElementSize(); }
        size_t GetTotalSize() const { return m_vertexCount * GetStride(); }

        // =====================================================================
        // Data Access
        // =====================================================================

        /**
         * @brief Get a single component value
         */
        template<typename T>
        T GetValue(size_t vertexIndex, size_t componentIndex = 0) const
        {
            if (vertexIndex >= m_vertexCount || componentIndex >= m_components)
            {
                throw std::out_of_range("Index out of range");
            }
            
            const T* typedData = static_cast<const T*>(m_data);
            return typedData[vertexIndex * m_components + componentIndex];
        }
        
        /**
         * @brief Get a vector value
         */
        template<typename VecType>
        VecType GetVector(size_t vertexIndex) const
        {
            constexpr int vecComponents = VecType::length();
            if (static_cast<size_t>(vecComponents) != m_components)
            {
                throw std::invalid_argument("Vector component count mismatch");
            }
            
            using ComponentType = typename VecType::value_type;
            VecType result;
            for (int i = 0; i < vecComponents; ++i)
            {
                result[i] = GetValue<ComponentType>(vertexIndex, i);
            }
            return result;
        }

        // =====================================================================
        // Utility
        // =====================================================================
        
        bool IsValid() const;
        std::unique_ptr<VertexAttribute> Clone() const;

    private:
        void* m_data;
        size_t m_vertexCount;
        size_t m_components;
        AttributeType m_type;
        bool m_normalized;

        template<typename T>
        static constexpr bool AlwaysFalse = false;

        template<typename T>
        static constexpr AttributeType DeduceAttributeType()
        {
            if constexpr (std::is_same_v<T, float>)
                return AttributeType::Float;
            else if constexpr (std::is_same_v<T, int32_t>)
                return AttributeType::Int;
            else if constexpr (std::is_same_v<T, uint32_t>)
                return AttributeType::UInt;
            else if constexpr (std::is_same_v<T, int16_t>)
                return AttributeType::Short;
            else if constexpr (std::is_same_v<T, uint16_t>)
                return AttributeType::UShort;
            else if constexpr (std::is_same_v<T, int8_t>)
                return AttributeType::Byte;
            else if constexpr (std::is_same_v<T, uint8_t>)
                return AttributeType::UByte;
            else
                static_assert(AlwaysFalse<T>, "Unsupported attribute type");
        }
    };

    // =========================================================================
    // Attribute Factory Functions
    // =========================================================================
    namespace AttributeFactory
    {
        std::unique_ptr<VertexAttribute> CreatePositions(const std::vector<Vec3>& positions);
        std::unique_ptr<VertexAttribute> CreateNormals(const std::vector<Vec3>& normals);
        std::unique_ptr<VertexAttribute> CreateUVs(const std::vector<Vec2>& uvs);
        std::unique_ptr<VertexAttribute> CreateColors(const std::vector<Vec4>& colors);
        std::unique_ptr<VertexAttribute> CreateTangents(const std::vector<Vec4>& tangents);
        std::unique_ptr<VertexAttribute> CreateBoneIndices(const std::vector<IVec4>& boneIndices);
        std::unique_ptr<VertexAttribute> CreateBoneWeights(const std::vector<Vec4>& boneWeights);
    }

} // namespace RVX
