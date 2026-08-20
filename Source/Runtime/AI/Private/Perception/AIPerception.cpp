/**
 * @file AIPerception.cpp
 * @brief AI perception implementation
 */

#include "AI/Perception/AIPerception.h"
#include "AI/Perception/SightSense.h"
#include "AI/Perception/HearingSense.h"
#include "Core/Log.h"

#include <algorithm>

namespace RVX::AI
{

// =========================================================================
// Construction
// =========================================================================

AIPerception::AIPerception()
    : m_sightSense(std::make_unique<SightSense>())
    , m_hearingSense(std::make_unique<HearingSense>())
{
    // Enable sight and hearing by default
    m_enabledSenses = (1u << static_cast<uint32>(SenseType::Sight)) |
                      (1u << static_cast<uint32>(SenseType::Hearing));
}

AIPerception::~AIPerception() = default;

// =========================================================================
// Configuration
// =========================================================================

void AIPerception::SetSenseEnabled(SenseType sense, bool enabled)
{
    uint32 bit = 1u << static_cast<uint32>(sense);
    if (enabled)
    {
        m_enabledSenses |= bit;
    }
    else
    {
        m_enabledSenses &= ~bit;
    }
}

bool AIPerception::IsSenseEnabled(SenseType sense) const
{
    uint32 bit = 1u << static_cast<uint32>(sense);
    return (m_enabledSenses & bit) != 0;
}

void AIPerception::ConfigureSight(const SightConfig& config)
{
    if (m_sightSense)
    {
        m_sightSense->SetConfig(config);
    }
}

void AIPerception::ConfigureHearing(const HearingConfig& config)
{
    if (m_hearingSense)
    {
        m_hearingSense->SetConfig(config);
    }
}

// =========================================================================
// Update
// =========================================================================

void AIPerception::Update(float deltaTime, const Vec3& ownerPosition, 
                          const Vec3& ownerForward)
{
    m_ownerPosition = ownerPosition;
    m_ownerForward = ownerForward;

    // Age existing stimuli
    AgeStimuli(deltaTime);

    // Update senses
    UpdateSenses(deltaTime);

    // Clean up old stimuli
    CleanupOldStimuli();
}

void AIPerception::ProcessStimulus(const PerceptionStimulus& stimulus)
{
    // Check if sense is enabled
    if (!IsSenseEnabled(stimulus.sense))
    {
        return;
    }

    // Check affiliation filter
    uint32 affiliationBit = 1u << static_cast<uint32>(stimulus.affiliation);
    if ((m_affiliationMask & affiliationBit) == 0)
    {
        return;
    }

    // Don't perceive self
    if (stimulus.sourceId == m_ownerId)
    {
        return;
    }

    // Find or create perceived actor
    PerceivedActor* actor = FindOrAddActor(stimulus.sourceId);
    if (!actor)
    {
        return;
    }

    bool wasPerceived = actor->isCurrentlyPerceived;

    // Update actor data
    actor->lastKnownLocation = stimulus.location;
    actor->lastSeenTime = 0.0f;  // Reset age
    actor->stimulusStrength = stimulus.strength;
    actor->affiliation = stimulus.affiliation;
    actor->isCurrentlyPerceived = true;

    // Update sense flags
    uint32 senseBit = 1u << static_cast<uint32>(stimulus.sense);
    actor->senseFlags |= senseBit;

    // Determine dominant sense (prefer sight over hearing)
    if (stimulus.sense == SenseType::Sight)
    {
        actor->dominantSense = SenseType::Sight;
    }
    else if (actor->dominantSense != SenseType::Sight)
    {
        actor->dominantSense = stimulus.sense;
    }

    // Fire events
    if (!wasPerceived)
    {
        NotifyPerceptionEvent(PerceptionEvent::GainedSense, stimulus);
    }
    else
    {
        NotifyPerceptionEvent(PerceptionEvent::Updated, stimulus);
    }
}

void AIPerception::ForgetActor(uint64 actorId)
{
    auto it = m_actorIndexMap.find(actorId);
    if (it != m_actorIndexMap.end())
    {
        size_t index = it->second;
        
        // Create lost sense stimulus for callback
        PerceptionStimulus stimulus;
        stimulus.sourceId = actorId;
        stimulus.sense = m_perceivedActors[index].dominantSense;
        stimulus.location = m_perceivedActors[index].lastKnownLocation;
        
        NotifyPerceptionEvent(PerceptionEvent::LostSense, stimulus);

        // Remove from vector (swap with last for efficiency)
        if (index < m_perceivedActors.size() - 1)
        {
            uint64 lastId = m_perceivedActors.back().actorId;
            m_perceivedActors[index] = std::move(m_perceivedActors.back());
            m_actorIndexMap[lastId] = index;
        }
        m_perceivedActors.pop_back();
        m_actorIndexMap.erase(it);
    }
}

void AIPerception::ClearPerception()
{
    m_perceivedActors.clear();
    m_actorIndexMap.clear();
}

// =========================================================================
// Queries
// =========================================================================

std::vector<const PerceivedActor*> AIPerception::GetActorsPerceivedBy(SenseType sense) const
{
    std::vector<const PerceivedActor*> result;
    uint32 senseBit = 1u << static_cast<uint32>(sense);

    for (const auto& actor : m_perceivedActors)
    {
        if ((actor.senseFlags & senseBit) != 0)
        {
            result.push_back(&actor);
        }
    }

    return result;
}

bool AIPerception::IsActorPerceived(uint64 actorId) const
{
    auto it = m_actorIndexMap.find(actorId);
    if (it == m_actorIndexMap.end())
    {
        return false;
    }
    return m_perceivedActors[it->second].isCurrentlyPerceived;
}

const PerceivedActor* AIPerception::GetPerceivedActor(uint64 actorId) const
{
    auto it = m_actorIndexMap.find(actorId);
    if (it == m_actorIndexMap.end())
    {
        return nullptr;
    }
    return &m_perceivedActors[it->second];
}

const PerceivedActor* AIPerception::GetMostRecentHostile() const
{
    const PerceivedActor* mostRecent = nullptr;
    float lowestAge = FLT_MAX;

    for (const auto& actor : m_perceivedActors)
    {
        if (actor.affiliation == Affiliation::Hostile && 
            actor.isCurrentlyPerceived &&
            actor.lastSeenTime < lowestAge)
        {
            mostRecent = &actor;
            lowestAge = actor.lastSeenTime;
        }
    }

    return mostRecent;
}

const PerceivedActor* AIPerception::GetClosestPerceived(const Vec3& fromPosition) const
{
    const PerceivedActor* closest = nullptr;
    float closestDist = FLT_MAX;

    for (const auto& actor : m_perceivedActors)
    {
        if (!actor.isCurrentlyPerceived)
        {
            continue;
        }

        float dist = glm::length(actor.lastKnownLocation - fromPosition);
        if (dist < closestDist)
        {
            closest = &actor;
            closestDist = dist;
        }
    }

    return closest;
}

bool AIPerception::HasLineOfSight(const Vec3& targetPosition) const
{
    if (!m_raycastCallback)
    {
        return true;  // Assume LOS if no callback set
    }

    return !m_raycastCallback(m_ownerPosition, targetPosition);
}

// =========================================================================
// Callbacks
// =========================================================================

void AIPerception::OnPerceptionUpdate(PerceptionCallback callback)
{
    m_onUpdate = std::move(callback);
}

void AIPerception::OnGainedSense(PerceptionCallback callback)
{
    m_onGainedSense = std::move(callback);
}

void AIPerception::OnLostSense(PerceptionCallback callback)
{
    m_onLostSense = std::move(callback);
}

// =========================================================================
// Internal Methods
// =========================================================================

void AIPerception::UpdateSenses(float deltaTime)
{
    (void)deltaTime;
    
    // Sight and hearing updates are typically driven by the game/world
    // through ProcessStimulus() or ReportNoise() calls.
    // This method could be used for periodic active scanning.
}

void AIPerception::AgeStimuli(float deltaTime)
{
    for (auto& actor : m_perceivedActors)
    {
        actor.lastSeenTime += deltaTime;
        
        // Decay strength over time
        float decayRate = 0.1f;  // 10% per second
        actor.stimulusStrength = std::max(0.0f, 
            actor.stimulusStrength - decayRate * deltaTime);
    }
}

void AIPerception::CleanupOldStimuli()
{
    // Mark actors as not perceived if too old
    for (auto& actor : m_perceivedActors)
    {
        if (actor.lastSeenTime > m_maxStimulusAge)
        {
            if (actor.isCurrentlyPerceived)
            {
                actor.isCurrentlyPerceived = false;
                actor.senseFlags = 0;

                PerceptionStimulus stimulus;
                stimulus.sourceId = actor.actorId;
                stimulus.sense = actor.dominantSense;
                stimulus.location = actor.lastKnownLocation;
                NotifyPerceptionEvent(PerceptionEvent::LostSense, stimulus);
            }
        }
    }

    // Actually remove actors that have been forgotten for a long time
    float removalAge = m_maxStimulusAge * 2.0f;
    
    auto it = m_perceivedActors.begin();
    while (it != m_perceivedActors.end())
    {
        if (it->lastSeenTime > removalAge)
        {
            uint64 id = it->actorId;
            
            // Update index map
            size_t currentIndex = std::distance(m_perceivedActors.begin(), it);
            if (currentIndex < m_perceivedActors.size() - 1)
            {
                // Swap with last and update that actor's index
                uint64 lastId = m_perceivedActors.back().actorId;
                *it = std::move(m_perceivedActors.back());
                m_actorIndexMap[lastId] = currentIndex;
            }
            
            m_perceivedActors.pop_back();
            m_actorIndexMap.erase(id);
            
            // Don't increment iterator - we swapped in a new element
            if (m_perceivedActors.empty() || currentIndex >= m_perceivedActors.size())
            {
                break;
            }
        }
        else
        {
            ++it;
        }
    }
}

PerceivedActor* AIPerception::FindOrAddActor(uint64 actorId)
{
    auto it = m_actorIndexMap.find(actorId);
    if (it != m_actorIndexMap.end())
    {
        return &m_perceivedActors[it->second];
    }

    // Add new actor
    PerceivedActor newActor;
    newActor.actorId = actorId;
    
    size_t index = m_perceivedActors.size();
    m_perceivedActors.push_back(newActor);
    m_actorIndexMap[actorId] = index;

    return &m_perceivedActors.back();
}

void AIPerception::NotifyPerceptionEvent(PerceptionEvent event, 
                                         const PerceptionStimulus& stimulus)
{
    switch (event)
    {
        case PerceptionEvent::GainedSense:
            if (m_onGainedSense)
            {
                m_onGainedSense(stimulus);
            }
            break;

        case PerceptionEvent::LostSense:
            if (m_onLostSense)
            {
                m_onLostSense(stimulus);
            }
            break;

        case PerceptionEvent::Updated:
            if (m_onUpdate)
            {
                m_onUpdate(stimulus);
            }
            break;
    }
}

} // namespace RVX::AI
