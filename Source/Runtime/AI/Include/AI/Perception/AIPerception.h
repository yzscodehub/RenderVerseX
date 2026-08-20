#pragma once

/**
 * @file AIPerception.h
 * @brief AI perception component for sensing the environment
 */

#include "AI/AITypes.h"
#include "Core/Types.h"

#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

namespace RVX::AI
{

class AISense;
class SightSense;
class HearingSense;

/**
 * @brief Callback for perception events
 */
using PerceptionCallback = std::function<void(const PerceptionStimulus&)>;

/**
 * @brief Perception event type
 */
enum class PerceptionEvent : uint8
{
    GainedSense,    ///< Started perceiving a target
    LostSense,      ///< Lost perception of a target
    Updated         ///< Perception was updated
};

/**
 * @brief Perceived actor data
 */
struct PerceivedActor
{
    uint64 actorId = 0;
    Vec3 lastKnownLocation;
    Vec3 lastKnownVelocity;
    float lastSeenTime = 0.0f;
    float stimulusStrength = 0.0f;
    Affiliation affiliation = Affiliation::Neutral;
    SenseType dominantSense = SenseType::Sight;
    bool isCurrentlyPerceived = false;

    /// Bit flags for which senses perceive this actor
    uint32 senseFlags = 0;
};

/**
 * @brief AI perception component
 * 
 * AIPerception manages multiple senses (sight, hearing, etc.) and maintains
 * a list of perceived actors. It processes stimuli from the environment and
 * notifies listeners of perception events.
 * 
 * Usage:
 * @code
 * AIPerception perception;
 * 
 * // Configure sight
 * SightConfig sightCfg;
 * sightCfg.sightRadius = 20.0f;
 * sightCfg.sightAngle = 90.0f;
 * perception.ConfigureSight(sightCfg);
 * 
 * // Configure hearing
 * HearingConfig hearingCfg;
 * hearingCfg.hearingRange = 30.0f;
 * perception.ConfigureHearing(hearingCfg);
 * 
 * // Set up callbacks
 * perception.OnPerceptionUpdate([](const PerceptionStimulus& stim) {
 *     if (stim.sense == SenseType::Sight)
 *     {
 *         // Saw something!
 *     }
 * });
 * 
 * // In AI subsystem update
 * perception.Update(deltaTime, ownerPosition, ownerForward);
 * @endcode
 */
class AIPerception
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    AIPerception();
    ~AIPerception();

    AIPerception(const AIPerception&) = delete;
    AIPerception& operator=(const AIPerception&) = delete;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Enable/disable a sense type
     */
    void SetSenseEnabled(SenseType sense, bool enabled);

    /**
     * @brief Check if a sense is enabled
     */
    bool IsSenseEnabled(SenseType sense) const;

    /**
     * @brief Configure sight sense
     */
    void ConfigureSight(const SightConfig& config);

    /**
     * @brief Configure hearing sense
     */
    void ConfigureHearing(const HearingConfig& config);

    /**
     * @brief Get sight sense
     */
    SightSense* GetSightSense() { return m_sightSense.get(); }

    /**
     * @brief Get hearing sense
     */
    HearingSense* GetHearingSense() { return m_hearingSense.get(); }

    /**
     * @brief Set the owner's affiliation
     */
    void SetAffiliation(Affiliation affiliation) { m_affiliation = affiliation; }

    /**
     * @brief Get the owner's affiliation
     */
    Affiliation GetAffiliation() const { return m_affiliation; }

    /**
     * @brief Set filter for affiliations to detect
     */
    void SetDetectionFilter(uint32 affiliationMask) { m_affiliationMask = affiliationMask; }

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update perception
     * @param deltaTime Time since last update
     * @param ownerPosition Owner's current position
     * @param ownerForward Owner's forward direction
     */
    void Update(float deltaTime, const Vec3& ownerPosition, const Vec3& ownerForward);

    /**
     * @brief Process an incoming stimulus
     */
    void ProcessStimulus(const PerceptionStimulus& stimulus);

    /**
     * @brief Force forget an actor
     */
    void ForgetActor(uint64 actorId);

    /**
     * @brief Clear all perceived actors
     */
    void ClearPerception();

    // =========================================================================
    // Queries
    // =========================================================================

    /**
     * @brief Get all currently perceived actors
     */
    const std::vector<PerceivedActor>& GetPerceivedActors() const { return m_perceivedActors; }

    /**
     * @brief Get actors perceived by a specific sense
     */
    std::vector<const PerceivedActor*> GetActorsPerceivedBy(SenseType sense) const;

    /**
     * @brief Check if a specific actor is currently perceived
     */
    bool IsActorPerceived(uint64 actorId) const;

    /**
     * @brief Get perceived actor data
     */
    const PerceivedActor* GetPerceivedActor(uint64 actorId) const;

    /**
     * @brief Get the most recently perceived hostile actor
     */
    const PerceivedActor* GetMostRecentHostile() const;

    /**
     * @brief Get the closest perceived actor
     */
    const PerceivedActor* GetClosestPerceived(const Vec3& fromPosition) const;

    /**
     * @brief Check if owner has line of sight to a position
     * @note This requires a raycast callback to be set
     */
    bool HasLineOfSight(const Vec3& targetPosition) const;

    // =========================================================================
    // Callbacks
    // =========================================================================

    /**
     * @brief Set callback for perception events
     */
    void OnPerceptionUpdate(PerceptionCallback callback);

    /**
     * @brief Set callback for gaining sense of an actor
     */
    void OnGainedSense(PerceptionCallback callback);

    /**
     * @brief Set callback for losing sense of an actor
     */
    void OnLostSense(PerceptionCallback callback);

    /**
     * @brief Set raycast callback for line of sight checks
     */
    using RaycastCallback = std::function<bool(const Vec3& start, const Vec3& end)>;
    void SetRaycastCallback(RaycastCallback callback) { m_raycastCallback = std::move(callback); }

    // =========================================================================
    // Properties
    // =========================================================================

    /**
     * @brief Set the entity ID of the owner
     */
    void SetOwnerId(uint64 ownerId) { m_ownerId = ownerId; }

    /**
     * @brief Get the entity ID of the owner
     */
    uint64 GetOwnerId() const { return m_ownerId; }

    /**
     * @brief Set max age before forgetting an actor
     */
    void SetMaxStimulusAge(float age) { m_maxStimulusAge = age; }

private:
    uint64 m_ownerId = 0;
    Affiliation m_affiliation = Affiliation::Neutral;
    uint32 m_affiliationMask = 0xFFFFFFFF;  // Detect all by default
    Vec3 m_ownerPosition{0.0f};
    Vec3 m_ownerForward{0.0f, 0.0f, 1.0f};
    float m_maxStimulusAge = 10.0f;

    // Senses
    std::unique_ptr<SightSense> m_sightSense;
    std::unique_ptr<HearingSense> m_hearingSense;
    uint32 m_enabledSenses = 0;

    // Perceived actors
    std::vector<PerceivedActor> m_perceivedActors;
    std::unordered_map<uint64, size_t> m_actorIndexMap;

    // Callbacks
    PerceptionCallback m_onUpdate;
    PerceptionCallback m_onGainedSense;
    PerceptionCallback m_onLostSense;
    RaycastCallback m_raycastCallback;

    // Internal methods
    void UpdateSenses(float deltaTime);
    void AgeStimuli(float deltaTime);
    void CleanupOldStimuli();
    PerceivedActor* FindOrAddActor(uint64 actorId);
    void NotifyPerceptionEvent(PerceptionEvent event, const PerceptionStimulus& stimulus);
};

} // namespace RVX::AI
