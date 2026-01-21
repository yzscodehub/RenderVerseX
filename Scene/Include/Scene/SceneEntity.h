#pragma once

/**
 * @file SceneEntity.h
 * @brief Base class for all scene entities
 * 
 * SceneEntity implements ISpatialEntity for spatial indexing integration.
 */

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Spatial/Index/ISpatialEntity.h"
#include <string>
#include <memory>
#include <atomic>

namespace RVX
{
    // Forward declarations
    class Transform;
    class SceneManager;

    /**
     * @brief Entity type enumeration
     */
    enum class EntityType : uint8_t
    {
        Node = 0,
        StaticMesh,
        SkeletalMesh,
        Light,
        Camera,
        Probe,
        Decal,
        Custom
    };

    /**
     * @brief Base class for all scene entities
     * 
     * Implements ISpatialEntity for integration with spatial indexing.
     * All renderable objects in the scene should derive from this class.
     */
    class SceneEntity : public Spatial::ISpatialEntity
    {
    public:
        using Handle = Spatial::EntityHandle;
        using Ptr = std::shared_ptr<SceneEntity>;
        using WeakPtr = std::weak_ptr<SceneEntity>;

        static constexpr Handle InvalidHandle = Spatial::InvalidHandle;

        // =====================================================================
        // Construction
        // =====================================================================

        explicit SceneEntity(const std::string& name = "Entity");
        ~SceneEntity() override = default;

        // Non-copyable, movable
        SceneEntity(const SceneEntity&) = delete;
        SceneEntity& operator=(const SceneEntity&) = delete;
        SceneEntity(SceneEntity&&) = default;
        SceneEntity& operator=(SceneEntity&&) = default;

        // =====================================================================
        // ISpatialEntity Implementation
        // =====================================================================

        Handle GetHandle() const override { return m_handle; }
        AABB GetWorldBounds() const override;
        uint32_t GetLayerMask() const override { return m_layerMask; }
        uint32_t GetTypeMask() const override { return 1u << static_cast<uint8_t>(GetEntityType()); }
        bool IsSpatialDirty() const override { return m_spatialDirty; }
        void ClearSpatialDirty() override { m_spatialDirty = false; }
        void* GetUserData() const override { return m_userData; }

        // =====================================================================
        // Basic Properties
        // =====================================================================

        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& name) { m_name = name; }

        virtual EntityType GetEntityType() const { return EntityType::Node; }

        bool IsActive() const { return m_active; }
        void SetActive(bool active) { m_active = active; }

        void SetLayerMask(uint32_t mask) { m_layerMask = mask; }
        void SetLayer(uint32_t layer) { m_layerMask = 1u << layer; }
        void AddLayer(uint32_t layer) { m_layerMask |= (1u << layer); }
        void RemoveLayer(uint32_t layer) { m_layerMask &= ~(1u << layer); }
        bool IsInLayer(uint32_t layer) const { return (m_layerMask & (1u << layer)) != 0; }

        void SetUserData(void* data) { m_userData = data; }

        // =====================================================================
        // Transform
        // =====================================================================

        const Vec3& GetPosition() const { return m_position; }
        void SetPosition(const Vec3& position);

        const Quat& GetRotation() const { return m_rotation; }
        void SetRotation(const Quat& rotation);

        const Vec3& GetScale() const { return m_scale; }
        void SetScale(const Vec3& scale);

        Mat4 GetWorldMatrix() const;
        Vec3 GetWorldPosition() const { return m_position; } // For root entities

        void Translate(const Vec3& delta);
        void Rotate(const Quat& delta);
        void RotateAround(const Vec3& axis, float angle);

        // =====================================================================
        // Bounds
        // =====================================================================

        void SetLocalBounds(const AABB& bounds);
        const AABB& GetLocalBounds() const { return m_localBounds; }

        // =====================================================================
        // Scene Management
        // =====================================================================

        SceneManager* GetSceneManager() const { return m_sceneManager; }

    protected:
        void MarkSpatialDirty() { m_spatialDirty = true; }
        void MarkTransformDirty() { m_transformDirty = true; MarkSpatialDirty(); }

        // Allow SceneManager to set itself
        friend class SceneManager;
        void SetSceneManager(SceneManager* manager) { m_sceneManager = manager; }

    private:
        // Identity
        Handle m_handle;
        std::string m_name;
        bool m_active = true;

        // Filtering
        uint32_t m_layerMask = ~0u;

        // Transform
        Vec3 m_position{0.0f};
        Quat m_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 m_scale{1.0f};
        mutable Mat4 m_worldMatrix{1.0f};
        mutable bool m_transformDirty = true;

        // Bounds
        AABB m_localBounds;
        mutable bool m_spatialDirty = true;

        // Scene manager reference
        SceneManager* m_sceneManager = nullptr;

        // User data
        void* m_userData = nullptr;

        // Handle generation
        static Handle GenerateHandle();
        static std::atomic<Handle> s_nextHandle;
    };

} // namespace RVX
