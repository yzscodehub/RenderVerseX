#pragma once

#include "Core/Types.h"
#include "ECS/Entity.h"
#include "ECS/Fragment.h"

#include <memory>
#include <typeindex>
#include <utility>
#include <vector>

namespace RVX::ECS
{
    /** @brief Runtime-erased sparse-set pool contract owned by Registry. */
    class IFragmentPool
    {
    public:
        virtual ~IFragmentPool() = default;

        [[nodiscard]] virtual std::unique_ptr<IFragmentPool> Clone() const = 0;
        [[nodiscard]] virtual std::type_index GetFragmentType() const = 0;
        [[nodiscard]] virtual bool Contains(EntityHandle entity) const = 0;
        virtual bool Remove(EntityHandle entity) = 0;
        [[nodiscard]] virtual uint32 GetEntityCount() const = 0;
        [[nodiscard]] virtual EntityHandle GetEntityAt(uint32 denseIndex) const = 0;
    };

    /**
     * @brief Typed sparse-set fragment storage.
     *
     * Sparse entries index into parallel dense entity and value arrays. Removal
     * uses swap-remove, preserving compact data while intentionally making dense
     * ordering structural-operation dependent.
     */
    template<Fragment T>
    class FragmentPool final : public IFragmentPool
    {
    public:
        [[nodiscard]] std::unique_ptr<IFragmentPool> Clone() const override
        {
            return std::make_unique<FragmentPool>(*this);
        }

        [[nodiscard]] std::type_index GetFragmentType() const override
        {
            return std::type_index(typeid(T));
        }

        [[nodiscard]] bool Contains(EntityHandle entity) const override
        {
            const uint32 entityIndex = entity.GetIndex();
            if (entityIndex >= m_sparse.size())
            {
                return false;
            }

            const uint32 denseIndex = m_sparse[entityIndex];
            return denseIndex != RVX_INVALID_INDEX &&
                   denseIndex < m_entities.size() &&
                   m_entities[denseIndex] == entity;
        }

        [[nodiscard]] const T* TryGet(EntityHandle entity) const
        {
            if (!Contains(entity))
            {
                return nullptr;
            }
            return &m_values[m_sparse[entity.GetIndex()]];
        }

        [[nodiscard]] uint64 GetWriteVersion() const { return m_writeVersion; }

        [[nodiscard]] uint64 GetEntityWriteVersion(EntityHandle entity) const
        {
            return Contains(entity) ? m_entityWriteVersions[m_sparse[entity.GetIndex()]] : 0;
        }

        [[nodiscard]] bool IsEnabled(EntityHandle entity) const
        {
            return Contains(entity) && m_enabled[m_sparse[entity.GetIndex()]] != 0;
        }

        bool Add(EntityHandle entity, T value)
        {
            if (Contains(entity))
            {
                return false;
            }

            const uint32 entityIndex = entity.GetIndex();
            if (entityIndex >= m_sparse.size())
            {
                m_sparse.resize(static_cast<size_t>(entityIndex) + 1u, RVX_INVALID_INDEX);
            }

            const uint32 denseIndex = static_cast<uint32>(m_entities.size());
            m_entities.push_back(entity);
            try
            {
                m_values.push_back(value);
            }
            catch (...)
            {
                m_entities.pop_back();
                throw;
            }
            try
            {
                m_entityWriteVersions.push_back(0);
            }
            catch (...)
            {
                m_values.pop_back();
                m_entities.pop_back();
                throw;
            }
            try
            {
                m_enabled.push_back(1);
            }
            catch (...)
            {
                m_entityWriteVersions.pop_back();
                m_values.pop_back();
                m_entities.pop_back();
                throw;
            }
            m_sparse[entityIndex] = denseIndex;
            return true;
        }

        bool Remove(EntityHandle entity) override
        {
            if (!Contains(entity))
            {
                return false;
            }

            const uint32 entityIndex = entity.GetIndex();
            const uint32 denseIndex = m_sparse[entityIndex];
            const uint32 lastIndex = static_cast<uint32>(m_entities.size() - 1u);
            if (denseIndex != lastIndex)
            {
                const EntityHandle movedEntity = m_entities[lastIndex];
                m_entities[denseIndex] = movedEntity;
                m_values[denseIndex] = m_values[lastIndex];
                m_entityWriteVersions[denseIndex] = m_entityWriteVersions[lastIndex];
                m_enabled[denseIndex] = m_enabled[lastIndex];
                m_sparse[movedEntity.GetIndex()] = denseIndex;
            }

            m_entities.pop_back();
            m_values.pop_back();
            m_entityWriteVersions.pop_back();
            m_enabled.pop_back();
            m_sparse[entityIndex] = RVX_INVALID_INDEX;
            return true;
        }

        [[nodiscard]] uint32 GetEntityCount() const override
        {
            return static_cast<uint32>(m_entities.size());
        }

        [[nodiscard]] EntityHandle GetEntityAt(uint32 denseIndex) const override
        {
            return denseIndex < m_entities.size() ? m_entities[denseIndex] : EntityHandle::Invalid();
        }

    private:
        friend class Registry;

        [[nodiscard]] T* TryGetForWrite(EntityHandle entity)
        {
            return Contains(entity) ? &m_values[m_sparse[entity.GetIndex()]] : nullptr;
        }

        void MarkWritten(EntityHandle entity, uint64 version)
        {
            if (!Contains(entity))
            {
                return;
            }

            m_writeVersion = version;
            m_entityWriteVersions[m_sparse[entity.GetIndex()]] = version;
        }

        bool SetEnabled(EntityHandle entity, bool enabled, uint64 version)
        {
            if (!Contains(entity))
            {
                return false;
            }

            const uint32 denseIndex = m_sparse[entity.GetIndex()];
            const uint8 next = enabled ? 1u : 0u;
            if (m_enabled[denseIndex] == next)
            {
                return false;
            }
            m_enabled[denseIndex] = next;
            m_writeVersion = version;
            m_entityWriteVersions[denseIndex] = version;
            return true;
        }

        std::vector<uint32> m_sparse;
        std::vector<EntityHandle> m_entities;
        std::vector<T> m_values;
        std::vector<uint64> m_entityWriteVersions;
        std::vector<uint8> m_enabled;
        uint64 m_writeVersion = 0;
    };
} // namespace RVX::ECS
