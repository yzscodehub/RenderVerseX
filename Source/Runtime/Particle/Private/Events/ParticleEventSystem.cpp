#include "Particle/Events/ParticleEvent.h"
#include "Particle/Events/ParticleEventHandler.h"
#include "Particle/Modules/SubEmitterModule.h"
#include "Core/Log.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace RVX::Particle
{

// ============================================================================
// ParticleEventDispatcher - Central event management
// ============================================================================

/**
 * @brief Central particle event dispatcher that routes events to handlers
 */
class ParticleEventDispatcher
{
public:
    static ParticleEventDispatcher& Get()
    {
        static ParticleEventDispatcher instance;
        return instance;
    }

    // =========================================================================
    // Handler Registration
    // =========================================================================

    /// Register a handler for a specific particle system instance
    void RegisterHandler(uint64 instanceId, ParticleEventHandler* handler)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handlers[instanceId] = handler;
    }

    /// Unregister a handler
    void UnregisterHandler(uint64 instanceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handlers.erase(instanceId);
    }

    /// Get handler for an instance
    ParticleEventHandler* GetHandler(uint64 instanceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_handlers.find(instanceId);
        return it != m_handlers.end() ? it->second : nullptr;
    }

    // =========================================================================
    // Event Dispatching
    // =========================================================================

    /// Dispatch an event to the appropriate handler
    void DispatchEvent(const ParticleEvent& event)
    {
        ParticleEventHandler* handler = GetHandler(event.instanceId);
        if (handler)
        {
            handler->DispatchEvent(event);
        }
        
        // Also dispatch to global listeners
        DispatchToGlobalListeners(event);
    }

    /// Dispatch multiple events
    void DispatchEvents(const std::vector<ParticleEvent>& events)
    {
        for (const auto& event : events)
        {
            DispatchEvent(event);
        }
    }

    // =========================================================================
    // Global Listeners
    // =========================================================================

    using GlobalEventCallback = std::function<void(const ParticleEvent&)>;

    /// Add a global event listener
    uint32 AddGlobalListener(GlobalEventCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint32 id = m_nextListenerId++;
        m_globalListeners[id] = std::move(callback);
        return id;
    }

    /// Remove a global listener
    void RemoveGlobalListener(uint32 listenerId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_globalListeners.erase(listenerId);
    }

    /// Clear all handlers and listeners
    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handlers.clear();
        m_globalListeners.clear();
    }

private:
    ParticleEventDispatcher() = default;
    ~ParticleEventDispatcher() = default;

    void DispatchToGlobalListeners(const ParticleEvent& event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, callback] : m_globalListeners)
        {
            callback(event);
        }
    }

    std::mutex m_mutex;
    std::unordered_map<uint64, ParticleEventHandler*> m_handlers;
    std::unordered_map<uint32, GlobalEventCallback> m_globalListeners;
    uint32 m_nextListenerId = 1;
};

// ============================================================================
// SubEmitter Event Integration
// ============================================================================

/**
 * @brief Handles sub-emitter spawning on particle events
 */
class SubEmitterEventProcessor
{
public:
    /**
     * @brief Process an event for potential sub-emitter triggering
     * @param event The particle event
     * @param subModule The sub-emitter module configuration
     * @param spawnCallback Callback to spawn a new particle system instance
     */
    static void ProcessEvent(
        const ParticleEvent& event,
        const SubEmitterModule& subModule,
        std::function<void(const SubEmitter&, const Vec3&, const Vec3&, const Vec4&)> spawnCallback)
    {
        for (const auto& subEmitter : subModule.subEmitters)
        {
            // Check if this sub-emitter should trigger
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
                    // Manual triggers are handled separately
                    break;
            }
            
            if (!shouldTrigger)
                continue;
            
            // Check probability
            if (subEmitter.probability < 1.0f)
            {
                float roll = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
                if (roll > subEmitter.probability)
                    continue;
            }
            
            // Spawn sub-emitter
            if (spawnCallback)
            {
                spawnCallback(subEmitter, event.position, event.velocity, event.color);
            }
        }
    }
};

// ============================================================================
// Event Statistics (for debugging/profiling)
// ============================================================================

/**
 * @brief Tracks particle event statistics
 */
class ParticleEventStats
{
public:
    struct FrameStats
    {
        uint32 birthEvents = 0;
        uint32 deathEvents = 0;
        uint32 collisionEvents = 0;
        uint32 triggerEvents = 0;
        uint32 subEmittersSpawned = 0;
    };

    static ParticleEventStats& Get()
    {
        static ParticleEventStats instance;
        return instance;
    }

    void RecordEvent(ParticleEventType type)
    {
        switch (type)
        {
            case ParticleEventType::OnBirth:
                m_currentFrame.birthEvents++;
                break;
            case ParticleEventType::OnDeath:
                m_currentFrame.deathEvents++;
                break;
            case ParticleEventType::OnCollision:
                m_currentFrame.collisionEvents++;
                break;
            case ParticleEventType::OnTriggerEnter:
            case ParticleEventType::OnTriggerExit:
                m_currentFrame.triggerEvents++;
                break;
        }
    }

    void RecordSubEmitterSpawn()
    {
        m_currentFrame.subEmittersSpawned++;
    }

    void BeginFrame()
    {
        m_lastFrame = m_currentFrame;
        m_currentFrame = {};
    }

    const FrameStats& GetLastFrame() const { return m_lastFrame; }
    const FrameStats& GetCurrentFrame() const { return m_currentFrame; }

private:
    ParticleEventStats() = default;

    FrameStats m_currentFrame;
    FrameStats m_lastFrame;
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Create events from particle death batch
 */
std::vector<ParticleEvent> CreateDeathEvents(
    const std::vector<uint32>& deadIndices,
    const void* particleData,  // CPUParticle array
    size_t particleStride,
    uint64 instanceId)
{
    std::vector<ParticleEvent> events;
    events.reserve(deadIndices.size());
    
    for (uint32 index : deadIndices)
    {
        const uint8* ptr = static_cast<const uint8*>(particleData) + index * particleStride;
        // Cast to CPUParticle structure - accessing common fields
        const Vec3* position = reinterpret_cast<const Vec3*>(ptr);
        const Vec3* velocity = reinterpret_cast<const Vec3*>(ptr + sizeof(Vec3));
        const Vec4* color = reinterpret_cast<const Vec4*>(ptr + sizeof(Vec3) * 2);
        const float* lifetime = reinterpret_cast<const float*>(ptr + sizeof(Vec3) * 2 + sizeof(Vec4) + sizeof(Vec4));
        
        events.push_back(MakeDeathEvent(
            *position,
            *velocity,
            *color,
            *lifetime,  // age (at death, age == lifetime)
            *lifetime,
            index,
            0,  // emitterIndex would need to be passed
            instanceId
        ));
    }
    
    return events;
}

/**
 * @brief Filter events by type
 */
std::vector<ParticleEvent> FilterEvents(
    const std::vector<ParticleEvent>& events,
    ParticleEventType type)
{
    std::vector<ParticleEvent> filtered;
    filtered.reserve(events.size());
    
    std::copy_if(events.begin(), events.end(), std::back_inserter(filtered),
        [type](const ParticleEvent& e) { return e.type == type; });
    
    return filtered;
}

/**
 * @brief Sort events by position for spatial locality
 */
void SortEventsByPosition(std::vector<ParticleEvent>& events)
{
    std::sort(events.begin(), events.end(),
        [](const ParticleEvent& a, const ParticleEvent& b)
        {
            // Morton code-like sorting for spatial locality
            return (a.position.x + a.position.y * 1000.0f + a.position.z * 1000000.0f) <
                   (b.position.x + b.position.y * 1000.0f + b.position.z * 1000000.0f);
        });
}

} // namespace RVX::Particle
