#pragma once

/**
 * @file CVarSystem.h
 * @brief Configuration variable system for runtime settings
 * 
 * Features:
 * - Typed configuration variables (bool, int, float, string)
 * - Value change callbacks
 * - Console integration
 * - Serialization to/from files
 * - Category organization
 */

#include "Core/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <variant>
#include <optional>
#include <mutex>

namespace RVX
{
    /**
     * @brief CVar flags
     */
    enum class CVarFlags : uint32
    {
        None        = 0,
        ReadOnly    = 1 << 0,   // Cannot be changed at runtime
        Cheat       = 1 << 1,   // Requires cheat mode to change
        RequireRestart = 1 << 2, // Changes require restart to take effect
        Archive     = 1 << 3,   // Saved to config file
        Hidden      = 1 << 4,   // Hidden from listings
        DevOnly     = 1 << 5,   // Only available in development builds
    };

    // Enable bitwise operations on CVarFlags
    inline CVarFlags operator|(CVarFlags a, CVarFlags b)
    {
        return static_cast<CVarFlags>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline CVarFlags operator&(CVarFlags a, CVarFlags b)
    {
        return static_cast<CVarFlags>(static_cast<uint32>(a) & static_cast<uint32>(b));
    }

    inline bool HasFlag(CVarFlags flags, CVarFlags flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    /**
     * @brief CVar value types
     */
    enum class CVarType : uint8
    {
        Bool,
        Int,
        Float,
        String
    };

    /**
     * @brief CVar value container
     */
    using CVarValue = std::variant<bool, int32, float32, std::string>;

    /**
     * @brief Callback for CVar value changes
     */
    using CVarCallback = std::function<void(const CVarValue& oldValue, const CVarValue& newValue)>;

    /**
     * @brief Reference to a registered CVar
     */
    class CVarRef
    {
    public:
        CVarRef() = default;

        bool IsValid() const { return m_index != RVX_INVALID_INDEX; }
        uint32 GetIndex() const { return m_index; }

        // Value access (implementation in CVarSystem)
        bool GetBool() const;
        int32 GetInt() const;
        float32 GetFloat() const;
        const std::string& GetString() const;

        void SetBool(bool value);
        void SetInt(int32 value);
        void SetFloat(float32 value);
        void SetString(const std::string& value);

        // Conversion operators for convenience
        operator bool() const { return GetBool(); }
        operator int32() const { return GetInt(); }
        operator float32() const { return GetFloat(); }

    private:
        friend class CVarSystem;
        explicit CVarRef(uint32 index) : m_index(index) {}

        uint32 m_index = RVX_INVALID_INDEX;
    };

    /**
     * @brief CVar definition
     */
    struct CVarDef
    {
        std::string name;
        std::string description;
        std::string category;
        CVarType type = CVarType::Bool;
        CVarFlags flags = CVarFlags::None;
        CVarValue defaultValue;
        CVarValue currentValue;
        CVarValue minValue;  // For numeric types
        CVarValue maxValue;  // For numeric types
        std::vector<CVarCallback> callbacks;
    };

    /**
     * @brief Configuration variable system
     * 
     * Usage:
     * @code
     * // Register CVars
     * auto vsync = CVarSystem::Get().RegisterBool("r.vsync", true, "Enable VSync");
     * auto fov = CVarSystem::Get().RegisterFloat("r.fov", 90.0f, "Field of view", 60.0f, 120.0f);
     * 
     * // Use CVars
     * if (vsync.GetBool()) { EnableVSync(); }
     * SetFOV(fov.GetFloat());
     * 
     * // Change values
     * CVarSystem::Get().SetBool("r.vsync", false);
     * @endcode
     */
    class CVarSystem
    {
    public:
        // =========================================================================
        // Singleton Access
        // =========================================================================

        /**
         * @brief Get the global CVar system instance
         */
        static CVarSystem& Get();

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the CVar system
         */
        void Initialize();

        /**
         * @brief Shutdown the CVar system
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =========================================================================
        // Registration
        // =========================================================================

        /**
         * @brief Register a boolean CVar
         * @param name CVar name (e.g., "r.vsync")
         * @param defaultValue Default value
         * @param description Help text
         * @param flags CVar flags
         * @return Reference to the CVar
         */
        CVarRef RegisterBool(
            const char* name,
            bool defaultValue,
            const char* description,
            CVarFlags flags = CVarFlags::None
        );

        /**
         * @brief Register an integer CVar
         * @param name CVar name
         * @param defaultValue Default value
         * @param description Help text
         * @param minValue Minimum allowed value
         * @param maxValue Maximum allowed value
         * @param flags CVar flags
         */
        CVarRef RegisterInt(
            const char* name,
            int32 defaultValue,
            const char* description,
            int32 minValue = INT32_MIN,
            int32 maxValue = INT32_MAX,
            CVarFlags flags = CVarFlags::None
        );

        /**
         * @brief Register a float CVar
         * @param name CVar name
         * @param defaultValue Default value
         * @param description Help text
         * @param minValue Minimum allowed value
         * @param maxValue Maximum allowed value
         * @param flags CVar flags
         */
        CVarRef RegisterFloat(
            const char* name,
            float32 defaultValue,
            const char* description,
            float32 minValue = -FLT_MAX,
            float32 maxValue = FLT_MAX,
            CVarFlags flags = CVarFlags::None
        );

        /**
         * @brief Register a string CVar
         */
        CVarRef RegisterString(
            const char* name,
            const char* defaultValue,
            const char* description,
            CVarFlags flags = CVarFlags::None
        );

        /**
         * @brief Unregister a CVar
         */
        void Unregister(const char* name);

        // =========================================================================
        // Access
        // =========================================================================

        /**
         * @brief Find a CVar by name
         * @return Invalid ref if not found
         */
        CVarRef Find(const char* name) const;

        /**
         * @brief Check if a CVar exists
         */
        bool Exists(const char* name) const;

        /**
         * @brief Get CVar definition
         */
        const CVarDef* GetDef(CVarRef ref) const;
        const CVarDef* GetDef(const char* name) const;

        /**
         * @brief Get all CVars in a category
         */
        std::vector<CVarRef> GetByCategory(const std::string& category) const;

        /**
         * @brief Get all CVar names
         */
        std::vector<std::string> GetAllNames() const;

        // =========================================================================
        // Value Access
        // =========================================================================

        // Getters
        bool GetBool(const char* name) const;
        bool GetBool(CVarRef ref) const;

        int32 GetInt(const char* name) const;
        int32 GetInt(CVarRef ref) const;

        float32 GetFloat(const char* name) const;
        float32 GetFloat(CVarRef ref) const;

        const std::string& GetString(const char* name) const;
        const std::string& GetString(CVarRef ref) const;

        // Setters (return false if failed due to flags/validation)
        bool SetBool(const char* name, bool value);
        bool SetBool(CVarRef ref, bool value);

        bool SetInt(const char* name, int32 value);
        bool SetInt(CVarRef ref, int32 value);

        bool SetFloat(const char* name, float32 value);
        bool SetFloat(CVarRef ref, float32 value);

        bool SetString(const char* name, const std::string& value);
        bool SetString(CVarRef ref, const std::string& value);

        /**
         * @brief Set value from string (for console)
         */
        bool SetFromString(const char* name, const std::string& value);

        /**
         * @brief Get value as string (for display)
         */
        std::string GetAsString(const char* name) const;
        std::string GetAsString(CVarRef ref) const;

        /**
         * @brief Reset CVar to default value
         */
        void ResetToDefault(const char* name);
        void ResetToDefault(CVarRef ref);

        /**
         * @brief Reset all CVars to defaults
         */
        void ResetAllToDefaults();

        // =========================================================================
        // Callbacks
        // =========================================================================

        /**
         * @brief Register a callback for value changes
         */
        void RegisterCallback(const char* name, CVarCallback callback);
        void RegisterCallback(CVarRef ref, CVarCallback callback);

        // =========================================================================
        // Persistence
        // =========================================================================

        /**
         * @brief Save archived CVars to file
         */
        bool SaveToFile(const std::string& filepath) const;

        /**
         * @brief Load CVars from file
         */
        bool LoadFromFile(const std::string& filepath);

        // =========================================================================
        // Console Integration
        // =========================================================================

        /**
         * @brief Register console commands for CVar access
         */
        void RegisterConsoleCommands();

    private:
        CVarSystem() = default;
        ~CVarSystem() { Shutdown(); }

        // Non-copyable
        CVarSystem(const CVarSystem&) = delete;
        CVarSystem& operator=(const CVarSystem&) = delete;

        void NotifyCallbacks(CVarDef& def, const CVarValue& oldValue);
        bool ValidateValue(const CVarDef& def, const CVarValue& value) const;
        std::string ExtractCategory(const std::string& name) const;

        // =========================================================================
        // Internal State
        // =========================================================================

        bool m_initialized = false;
        bool m_cheatsEnabled = false;

        std::vector<CVarDef> m_cvars;
        std::unordered_map<std::string, uint32> m_nameToIndex;

        mutable std::mutex m_mutex;

        static const std::string s_emptyString;
    };

    // =========================================================================
    // CVarRef Inline Implementations
    // =========================================================================

    inline bool CVarRef::GetBool() const
    {
        return CVarSystem::Get().GetBool(*this);
    }

    inline int32 CVarRef::GetInt() const
    {
        return CVarSystem::Get().GetInt(*this);
    }

    inline float32 CVarRef::GetFloat() const
    {
        return CVarSystem::Get().GetFloat(*this);
    }

    inline const std::string& CVarRef::GetString() const
    {
        return CVarSystem::Get().GetString(*this);
    }

    inline void CVarRef::SetBool(bool value)
    {
        CVarSystem::Get().SetBool(*this, value);
    }

    inline void CVarRef::SetInt(int32 value)
    {
        CVarSystem::Get().SetInt(*this, value);
    }

    inline void CVarRef::SetFloat(float32 value)
    {
        CVarSystem::Get().SetFloat(*this, value);
    }

    inline void CVarRef::SetString(const std::string& value)
    {
        CVarSystem::Get().SetString(*this, value);
    }

} // namespace RVX

// =============================================================================
// CVar Macros
// =============================================================================

/**
 * @brief Declare and register a boolean CVar
 */
#define RVX_CVAR_BOOL(varName, name, defaultVal, desc) \
    static ::RVX::CVarRef varName = ::RVX::CVarSystem::Get().RegisterBool(name, defaultVal, desc)

/**
 * @brief Declare and register an integer CVar
 */
#define RVX_CVAR_INT(varName, name, defaultVal, desc) \
    static ::RVX::CVarRef varName = ::RVX::CVarSystem::Get().RegisterInt(name, defaultVal, desc)

/**
 * @brief Declare and register a float CVar
 */
#define RVX_CVAR_FLOAT(varName, name, defaultVal, desc) \
    static ::RVX::CVarRef varName = ::RVX::CVarSystem::Get().RegisterFloat(name, defaultVal, desc)

/**
 * @brief Declare and register a string CVar
 */
#define RVX_CVAR_STRING(varName, name, defaultVal, desc) \
    static ::RVX::CVarRef varName = ::RVX::CVarSystem::Get().RegisterString(name, defaultVal, desc)
