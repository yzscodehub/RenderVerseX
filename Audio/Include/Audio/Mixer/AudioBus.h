/**
 * @file AudioBus.h
 * @brief Audio bus for hierarchical mixing
 */

#pragma once

#include "Audio/AudioTypes.h"
#include <string>
#include <vector>
#include <memory>

namespace RVX::Audio
{

class IAudioEffect;

/**
 * @brief Audio bus identifier constants
 */
namespace BusId
{
    constexpr uint32 Master  = 0;
    constexpr uint32 Music   = 1;
    constexpr uint32 SFX     = 2;
    constexpr uint32 Voice   = 3;
    constexpr uint32 Ambient = 4;
    constexpr uint32 UI      = 5;
}

/**
 * @brief Audio bus for hierarchical mixing
 * 
 * Buses allow grouping sounds for collective volume control
 * and effect processing. They form a tree structure with
 * the Master bus at the root.
 * 
 * Structure:
 * Master
 * ├── Music
 * ├── SFX
 * │   ├── Weapons
 * │   ├── Footsteps
 * │   └── UI
 * ├── Voice (dialogue)
 * └── Ambient
 */
class AudioBusNode
{
public:
    using Ptr = std::shared_ptr<AudioBusNode>;

    AudioBusNode(uint32 id, const std::string& name);
    ~AudioBusNode() = default;

    // =========================================================================
    // Properties
    // =========================================================================

    uint32 GetId() const { return m_id; }
    const std::string& GetName() const { return m_name; }

    // =========================================================================
    // Volume Control
    // =========================================================================

    /**
     * @brief Set bus volume (0-1)
     */
    void SetVolume(float volume);
    float GetVolume() const { return m_volume; }

    /**
     * @brief Get effective volume (includes parent volumes)
     */
    float GetEffectiveVolume() const;

    /**
     * @brief Mute/unmute the bus
     */
    void SetMuted(bool muted);
    bool IsMuted() const { return m_muted; }

    /**
     * @brief Solo this bus (mute all others)
     */
    void SetSolo(bool solo);
    bool IsSolo() const { return m_solo; }

    // =========================================================================
    // Pan
    // =========================================================================

    void SetPan(float pan) { m_pan = pan; }
    float GetPan() const { return m_pan; }

    // =========================================================================
    // Hierarchy
    // =========================================================================

    void SetParent(AudioBusNode* parent) { m_parent = parent; }
    AudioBusNode* GetParent() const { return m_parent; }

    void AddChild(Ptr child);
    void RemoveChild(uint32 childId);
    const std::vector<Ptr>& GetChildren() const { return m_children; }

    // =========================================================================
    // Effects
    // =========================================================================

    /**
     * @brief Add an effect to the bus
     */
    void AddEffect(std::shared_ptr<IAudioEffect> effect);

    /**
     * @brief Remove an effect
     */
    void RemoveEffect(size_t index);

    /**
     * @brief Get effects
     */
    const std::vector<std::shared_ptr<IAudioEffect>>& GetEffects() const { return m_effects; }

    // =========================================================================
    // Send
    // =========================================================================

    /**
     * @brief Set send amount to another bus (for effects sends)
     */
    void SetSend(uint32 targetBusId, float amount);
    float GetSend(uint32 targetBusId) const;

private:
    uint32 m_id;
    std::string m_name;

    float m_volume = 1.0f;
    float m_pan = 0.0f;
    bool m_muted = false;
    bool m_solo = false;

    AudioBusNode* m_parent = nullptr;
    std::vector<Ptr> m_children;

    std::vector<std::shared_ptr<IAudioEffect>> m_effects;
    std::unordered_map<uint32, float> m_sends;  // Bus ID -> send amount
};

} // namespace RVX::Audio
