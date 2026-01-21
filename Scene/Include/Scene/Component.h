#pragma once

/**
 * @file Component.h
 * @brief Base class for all entity components
 * 
 * Components add functionality to SceneEntity through composition.
 * Each component type can only be attached once per entity.
 */

#include "Core/Math/AABB.h"
#include <string>

namespace RVX
{
    // Forward declaration
    class SceneEntity;

    /**
     * @brief Base class for all components
     * 
     * Components are attached to SceneEntity to add functionality.
     * Override virtual methods to customize behavior.
     * 
     * Usage:
     * @code
     * class MeshRendererComponent : public Component
     * {
     * public:
     *     const char* GetTypeName() const override { return "MeshRenderer"; }
     *     bool ProvidesBounds() const override { return true; }
     *     AABB GetLocalBounds() const override { return m_mesh->GetBounds(); }
     * };
     * @endcode
     */
    class Component
    {
    public:
        Component() = default;
        virtual ~Component() = default;

        // Non-copyable
        Component(const Component&) = delete;
        Component& operator=(const Component&) = delete;

        // =====================================================================
        // Type Information
        // =====================================================================

        /// Get the component type name (for debugging/serialization)
        virtual const char* GetTypeName() const = 0;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /// Called when component is attached to an entity
        virtual void OnAttach() {}

        /// Called when component is detached from an entity
        virtual void OnDetach() {}

        /// Called every frame (if enabled)
        virtual void Tick(float deltaTime) { (void)deltaTime; }

        // =====================================================================
        // State
        // =====================================================================

        /// Get the owning entity
        SceneEntity* GetOwner() const { return m_owner; }

        /// Check if component is enabled
        bool IsEnabled() const { return m_enabled; }

        /// Enable/disable the component
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        // =====================================================================
        // Spatial Bounds (Optional - for hybrid bounds system)
        // =====================================================================

        /// Does this component provide spatial bounds?
        virtual bool ProvidesBounds() const { return false; }

        /// Get local-space bounds (only if ProvidesBounds() returns true)
        virtual AABB GetLocalBounds() const { return AABB(); }

    protected:
        /// Notify owner that bounds have changed (call after modifying data that affects bounds)
        void NotifyBoundsChanged();

        /// Mark the component as needing a tick update
        void RequestTick() { m_needsTick = true; }

    private:
        friend class SceneEntity;

        /// Set by SceneEntity when attached
        void SetOwner(SceneEntity* owner) { m_owner = owner; }

        SceneEntity* m_owner = nullptr;
        bool m_enabled = true;
        bool m_needsTick = false;
    };

} // namespace RVX
