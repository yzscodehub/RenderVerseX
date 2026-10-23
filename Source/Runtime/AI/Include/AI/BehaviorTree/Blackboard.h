#pragma once

/**
 * @file Blackboard.h
 * @brief Shared data storage for behavior trees
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"

#include <string>
#include <unordered_map>
#include <variant>
#include <any>
#include <functional>
#include <optional>

namespace RVX::AI
{

/**
 * @brief Key for blackboard entries
 */
struct BlackboardKey
{
    std::string name;
    size_t hash = 0;

    BlackboardKey() = default;
    
    explicit BlackboardKey(const std::string& n)
        : name(n), hash(std::hash<std::string>{}(n)) {}
    
    explicit BlackboardKey(const char* n)
        : name(n), hash(std::hash<std::string>{}(name)) {}

    bool operator==(const BlackboardKey& other) const
    {
        return hash == other.hash && name == other.name;
    }
};

} // namespace RVX::AI

// Hash specialization for BlackboardKey
template<>
struct std::hash<RVX::AI::BlackboardKey>
{
    size_t operator()(const RVX::AI::BlackboardKey& key) const
    {
        return key.hash;
    }
};

namespace RVX::AI
{

/**
 * @brief Common blackboard value types
 */
using BlackboardValue = std::variant<
    bool,
    int32,
    float,
    Vec3,
    std::string,
    uint64      // Entity/object references
>;

/**
 * @brief Callback for blackboard value changes
 */
using BlackboardObserver = std::function<void(const BlackboardKey& key)>;

/**
 * @brief Shared data storage for behavior trees
 * 
 * The Blackboard provides a key-value store for AI data that can be
 * shared between behavior tree nodes, perception, and game systems.
 * 
 * Features:
 * - Type-safe value access
 * - Observer pattern for value changes
 * - Support for common types and custom data
 * 
 * Usage:
 * @code
 * Blackboard blackboard;
 * 
 * // Set values
 * blackboard.SetValue<Vec3>("TargetLocation", enemyPos);
 * blackboard.SetValue<uint64>("TargetEnemy", enemyId);
 * blackboard.SetValue<bool>("HasAmmo", true);
 * 
 * // Get values
 * auto target = blackboard.GetValue<Vec3>("TargetLocation");
 * if (target.has_value())
 * {
 *     MoveToward(*target);
 * }
 * 
 * // Observe changes
 * blackboard.AddObserver("TargetEnemy", [](const BlackboardKey& key) {
 *     // React to target change
 * });
 * @endcode
 */
class Blackboard
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    Blackboard() = default;
    ~Blackboard() = default;

    Blackboard(const Blackboard&) = default;
    Blackboard& operator=(const Blackboard&) = default;
    Blackboard(Blackboard&&) = default;
    Blackboard& operator=(Blackboard&&) = default;

    // =========================================================================
    // Value Access (Type-Safe)
    // =========================================================================

    /**
     * @brief Set a typed value
     */
    template<typename T>
    void SetValue(const BlackboardKey& key, const T& value)
    {
        bool changed = true;
        
        auto it = m_values.find(key);
        if (it != m_values.end())
        {
            if (auto* existing = std::get_if<T>(&it->second))
            {
                changed = (*existing != value);
            }
            it->second = value;
        }
        else
        {
            m_values[key] = value;
        }

        if (changed)
        {
            NotifyObservers(key);
        }
    }

    /**
     * @brief Set a value by string key
     */
    template<typename T>
    void SetValue(const std::string& keyName, const T& value)
    {
        SetValue<T>(BlackboardKey(keyName), value);
    }

    /**
     * @brief Get a typed value
     * @return Optional containing the value, or empty if not found or wrong type
     */
    template<typename T>
    std::optional<T> GetValue(const BlackboardKey& key) const
    {
        auto it = m_values.find(key);
        if (it == m_values.end())
        {
            return std::nullopt;
        }

        if (auto* val = std::get_if<T>(&it->second))
        {
            return *val;
        }

        return std::nullopt;
    }

    /**
     * @brief Get a value by string key
     */
    template<typename T>
    std::optional<T> GetValue(const std::string& keyName) const
    {
        return GetValue<T>(BlackboardKey(keyName));
    }

    /**
     * @brief Get a value with default fallback
     */
    template<typename T>
    T GetValueOr(const BlackboardKey& key, const T& defaultValue) const
    {
        return GetValue<T>(key).value_or(defaultValue);
    }

    /**
     * @brief Get a value by string key with default
     */
    template<typename T>
    T GetValueOr(const std::string& keyName, const T& defaultValue) const
    {
        return GetValueOr<T>(BlackboardKey(keyName), defaultValue);
    }

    // =========================================================================
    // Generic Value Access
    // =========================================================================

    /**
     * @brief Check if a key exists
     */
    bool HasKey(const BlackboardKey& key) const
    {
        return m_values.find(key) != m_values.end();
    }

    /**
     * @brief Check if a key exists by name
     */
    bool HasKey(const std::string& keyName) const
    {
        return HasKey(BlackboardKey(keyName));
    }

    /**
     * @brief Remove a key
     */
    void RemoveKey(const BlackboardKey& key)
    {
        if (m_values.erase(key) > 0)
        {
            NotifyObservers(key);
        }
    }

    /**
     * @brief Remove a key by name
     */
    void RemoveKey(const std::string& keyName)
    {
        RemoveKey(BlackboardKey(keyName));
    }

    /**
     * @brief Clear all values
     */
    void Clear()
    {
        m_values.clear();
    }

    /**
     * @brief Get all keys
     */
    std::vector<BlackboardKey> GetAllKeys() const
    {
        std::vector<BlackboardKey> keys;
        keys.reserve(m_values.size());
        for (const auto& [key, value] : m_values)
        {
            keys.push_back(key);
        }
        return keys;
    }

    // =========================================================================
    // Custom Data (for types not in BlackboardValue)
    // =========================================================================

    /**
     * @brief Set custom data using std::any
     */
    void SetCustomData(const BlackboardKey& key, std::any data)
    {
        m_customData[key] = std::move(data);
        NotifyObservers(key);
    }

    /**
     * @brief Get custom data
     */
    template<typename T>
    T* GetCustomData(const BlackboardKey& key)
    {
        auto it = m_customData.find(key);
        if (it == m_customData.end())
        {
            return nullptr;
        }
        return std::any_cast<T>(&it->second);
    }

    /**
     * @brief Get custom data (const)
     */
    template<typename T>
    const T* GetCustomData(const BlackboardKey& key) const
    {
        auto it = m_customData.find(key);
        if (it == m_customData.end())
        {
            return nullptr;
        }
        return std::any_cast<T>(&it->second);
    }

    // =========================================================================
    // Observers
    // =========================================================================

    /**
     * @brief Add an observer for a key
     * @return Observer ID for removal
     */
    uint32 AddObserver(const BlackboardKey& key, BlackboardObserver observer)
    {
        uint32 id = m_nextObserverId++;
        m_observers[key].push_back({id, std::move(observer)});
        return id;
    }

    /**
     * @brief Add observer by key name
     */
    uint32 AddObserver(const std::string& keyName, BlackboardObserver observer)
    {
        return AddObserver(BlackboardKey(keyName), std::move(observer));
    }

    /**
     * @brief Remove an observer
     */
    void RemoveObserver(uint32 observerId)
    {
        for (auto& [key, observers] : m_observers)
        {
            auto it = std::remove_if(observers.begin(), observers.end(),
                [observerId](const ObserverEntry& entry) {
                    return entry.id == observerId;
                });
            observers.erase(it, observers.end());
        }
    }

    /**
     * @brief Remove all observers for a key
     */
    void RemoveObservers(const BlackboardKey& key)
    {
        m_observers.erase(key);
    }

private:
    struct ObserverEntry
    {
        uint32 id;
        BlackboardObserver callback;
    };

    std::unordered_map<BlackboardKey, BlackboardValue> m_values;
    std::unordered_map<BlackboardKey, std::any> m_customData;
    std::unordered_map<BlackboardKey, std::vector<ObserverEntry>> m_observers;
    uint32 m_nextObserverId = 1;

    void NotifyObservers(const BlackboardKey& key)
    {
        auto it = m_observers.find(key);
        if (it != m_observers.end())
        {
            for (const auto& entry : it->second)
            {
                entry.callback(key);
            }
        }
    }
};

} // namespace RVX::AI
