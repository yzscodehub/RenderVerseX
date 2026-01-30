#include "Particle/Modules/SubEmitterModule.h"
#include "Particle/Events/ParticleEvent.h"
#include "Particle/ParticleTypes.h"
#include "Core/MathTypes.h"
#include <vector>
#include <functional>
#include <random>
#include <queue>

namespace RVX::Particle
{

// ============================================================================
// SubEmitter Spawn Request
// ============================================================================

/**
 * @brief Request to spawn a sub-emitter instance
 */
struct SubEmitterSpawnRequest
{
    const SubEmitter* config = nullptr;  ///< Sub-emitter configuration
    Vec3 position;                       ///< Spawn position
    Vec3 velocity;                       ///< Inherited velocity
    Vec4 color;                          ///< Inherited color
    Vec2 size;                           ///< Inherited size
    float rotation;                      ///< Inherited rotation
    float lifetime;                      ///< Parent lifetime (for duration inheritance)
    uint32 emitCount;                    ///< Number of particles to emit
};

// ============================================================================
// SubEmitterModuleProcessor
// ============================================================================

/**
 * @brief Processes sub-emitter triggers and manages spawning
 */
class SubEmitterModuleProcessor
{
public:
    SubEmitterModuleProcessor() = default;
    ~SubEmitterModuleProcessor() = default;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set the sub-emitter module to process
     */
    void SetModule(const SubEmitterModule* module)
    {
        m_module = module;
    }

    // =========================================================================
    // Event Processing
    // =========================================================================

    /**
     * @brief Process a particle event for potential sub-emitter triggering
     * @param event The particle event
     */
    void ProcessEvent(const ParticleEvent& event)
    {
        if (!m_module || !m_module->enabled)
            return;

        for (const auto& subEmitter : m_module->subEmitters)
        {
            // Check if trigger matches event type
            bool shouldTrigger = false;
            
            switch (subEmitter.trigger)
            {
                case SubEmitterTrigger::Birth:
                    shouldTrigger = (event.type == ParticleEventType::OnBirth);
                    break;
                    
                case SubEmitterTrigger::Death:
                    shouldTrigger = (event.type == ParticleEventType::OnDeath);
                    break;
                    
                case SubEmitterTrigger::Collision:
                    shouldTrigger = (event.type == ParticleEventType::OnCollision);
                    break;
                    
                case SubEmitterTrigger::Manual:
                    // Manual triggers are handled via TriggerManual()
                    break;
            }
            
            if (!shouldTrigger)
                continue;

            // Check probability
            if (subEmitter.probability < 1.0f)
            {
                float roll = m_dist(m_rng);
                if (roll > subEmitter.probability)
                    continue;
            }

            // Create spawn request
            SubEmitterSpawnRequest request;
            request.config = &subEmitter;
            request.position = event.position;
            request.velocity = event.velocity;
            request.color = event.color;
            request.size = Vec2(1.0f); // Would need size from particle
            request.rotation = 0.0f;
            request.lifetime = event.lifetime;
            request.emitCount = subEmitter.emitCount;
            
            m_spawnQueue.push(request);
        }
    }

    /**
     * @brief Process multiple events
     */
    void ProcessEvents(const std::vector<ParticleEvent>& events)
    {
        for (const auto& event : events)
        {
            ProcessEvent(event);
        }
    }

    /**
     * @brief Manually trigger a sub-emitter at a position
     * @param subEmitterIndex Index of sub-emitter to trigger
     * @param position World position
     * @param velocity Optional velocity
     * @param color Optional color
     */
    void TriggerManual(
        uint32 subEmitterIndex,
        const Vec3& position,
        const Vec3& velocity = Vec3(0),
        const Vec4& color = Vec4(1))
    {
        if (!m_module || subEmitterIndex >= m_module->subEmitters.size())
            return;

        const auto& subEmitter = m_module->subEmitters[subEmitterIndex];
        
        if (subEmitter.trigger != SubEmitterTrigger::Manual)
            return;

        SubEmitterSpawnRequest request;
        request.config = &subEmitter;
        request.position = position;
        request.velocity = velocity;
        request.color = color;
        request.size = Vec2(1.0f);
        request.rotation = 0.0f;
        request.lifetime = 1.0f;
        request.emitCount = subEmitter.emitCount;
        
        m_spawnQueue.push(request);
    }

    // =========================================================================
    // Spawn Request Processing
    // =========================================================================

    /**
     * @brief Callback type for spawning particle system instances
     */
    using SpawnCallback = std::function<void(
        const std::string& systemPath,
        const Vec3& position,
        const Vec3& velocity,
        const Vec4& color,
        float sizeMultiplier,
        float lifetimeMultiplier,
        uint32 emitCount
    )>;

    /**
     * @brief Process pending spawn requests
     * @param callback Callback to spawn particle system instances
     */
    void ProcessSpawnRequests(SpawnCallback callback)
    {
        while (!m_spawnQueue.empty())
        {
            const auto& request = m_spawnQueue.front();
            
            if (request.config && callback)
            {
                // Calculate inherited properties
                float sizeMultiplier = 1.0f;
                float lifetimeMultiplier = 1.0f;
                Vec4 color = request.color;
                
                if (HasFlag(request.config->inherit, SubEmitterInherit::Size))
                {
                    sizeMultiplier = (request.size.x + request.size.y) * 0.5f;
                }
                
                if (HasFlag(request.config->inherit, SubEmitterInherit::Lifetime) ||
                    HasFlag(request.config->inherit, SubEmitterInherit::Duration))
                {
                    lifetimeMultiplier = request.lifetime;
                }
                
                if (!HasFlag(request.config->inherit, SubEmitterInherit::Color))
                {
                    color = Vec4(1.0f);
                }

                callback(
                    request.config->systemPath,
                    request.position,
                    request.velocity,
                    color,
                    sizeMultiplier,
                    lifetimeMultiplier,
                    request.emitCount
                );
            }
            
            m_spawnQueue.pop();
        }
    }

    /**
     * @brief Check if there are pending spawn requests
     */
    bool HasPendingRequests() const { return !m_spawnQueue.empty(); }

    /**
     * @brief Get number of pending requests
     */
    size_t GetPendingRequestCount() const { return m_spawnQueue.size(); }

    /**
     * @brief Clear all pending requests
     */
    void ClearPendingRequests()
    {
        while (!m_spawnQueue.empty())
            m_spawnQueue.pop();
    }

    // =========================================================================
    // Utility
    // =========================================================================

    /**
     * @brief Get sub-emitters triggered by a specific event type
     */
    std::vector<const SubEmitter*> GetSubEmittersForEvent(ParticleEventType eventType) const
    {
        std::vector<const SubEmitter*> result;
        
        if (!m_module)
            return result;

        SubEmitterTrigger trigger;
        switch (eventType)
        {
            case ParticleEventType::OnBirth:
                trigger = SubEmitterTrigger::Birth;
                break;
            case ParticleEventType::OnDeath:
                trigger = SubEmitterTrigger::Death;
                break;
            case ParticleEventType::OnCollision:
                trigger = SubEmitterTrigger::Collision;
                break;
            default:
                return result;
        }

        for (const auto& subEmitter : m_module->subEmitters)
        {
            if (subEmitter.trigger == trigger)
            {
                result.push_back(&subEmitter);
            }
        }

        return result;
    }

    /**
     * @brief Check if any sub-emitter is listening for a specific event
     */
    bool HasSubEmitterForEvent(ParticleEventType eventType) const
    {
        return !GetSubEmittersForEvent(eventType).empty();
    }

private:
    const SubEmitterModule* m_module = nullptr;
    std::queue<SubEmitterSpawnRequest> m_spawnQueue;
    std::minstd_rand m_rng{std::random_device{}()};
    std::uniform_real_distribution<float> m_dist{0.0f, 1.0f};
};

// ============================================================================
// SubEmitter Pool Management
// ============================================================================

/**
 * @brief Manages a pool of sub-emitter instances for reuse
 */
class SubEmitterPool
{
public:
    /**
     * @brief Sub-emitter instance data
     */
    struct Instance
    {
        std::string systemPath;
        Vec3 position;
        Vec3 velocity;
        Vec4 color;
        float sizeMultiplier = 1.0f;
        float lifetimeMultiplier = 1.0f;
        float timeRemaining = 0.0f;
        bool active = false;
    };

    SubEmitterPool(size_t maxInstances = 100)
        : m_maxInstances(maxInstances)
    {
        m_instances.resize(maxInstances);
    }

    /**
     * @brief Allocate a new sub-emitter instance
     * @return Pointer to instance, or nullptr if pool is full
     */
    Instance* Allocate()
    {
        for (auto& instance : m_instances)
        {
            if (!instance.active)
            {
                instance.active = true;
                return &instance;
            }
        }
        return nullptr;
    }

    /**
     * @brief Free an instance back to the pool
     */
    void Free(Instance* instance)
    {
        if (instance)
        {
            instance->active = false;
        }
    }

    /**
     * @brief Update all active instances
     * @param deltaTime Frame delta time
     */
    void Update(float deltaTime)
    {
        for (auto& instance : m_instances)
        {
            if (instance.active)
            {
                instance.timeRemaining -= deltaTime;
                if (instance.timeRemaining <= 0.0f)
                {
                    instance.active = false;
                }
            }
        }
    }

    /**
     * @brief Get all active instances
     */
    std::vector<Instance*> GetActiveInstances()
    {
        std::vector<Instance*> active;
        for (auto& instance : m_instances)
        {
            if (instance.active)
            {
                active.push_back(&instance);
            }
        }
        return active;
    }

    /**
     * @brief Get count of active instances
     */
    size_t GetActiveCount() const
    {
        size_t count = 0;
        for (const auto& instance : m_instances)
        {
            if (instance.active)
                count++;
        }
        return count;
    }

    /**
     * @brief Clear all instances
     */
    void Clear()
    {
        for (auto& instance : m_instances)
        {
            instance.active = false;
        }
    }

private:
    std::vector<Instance> m_instances;
    size_t m_maxInstances;
};

} // namespace RVX::Particle
