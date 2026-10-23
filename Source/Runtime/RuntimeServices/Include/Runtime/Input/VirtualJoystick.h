#pragma once

/**
 * @file VirtualJoystick.h
 * @brief Virtual on-screen joystick for touch input
 * 
 * Provides a virtual joystick that can be used with touch input
 * to simulate analog stick input on mobile devices.
 */

#include "HAL/Input/TouchState.h"
#include "Core/Types.h"

namespace RVX
{
    /**
     * @brief Virtual joystick behavior mode
     */
    enum class VirtualJoystickMode : uint8
    {
        Fixed,      ///< Joystick is at a fixed position
        Floating,   ///< Joystick appears where you touch
        Dynamic     ///< Joystick follows the finger if moved beyond range
    };

    /**
     * @brief Virtual joystick configuration
     */
    struct VirtualJoystickConfig
    {
        /// Joystick mode
        VirtualJoystickMode mode = VirtualJoystickMode::Floating;

        /// Position of the joystick center (for Fixed mode)
        float centerX = 150.0f;
        float centerY = 150.0f;

        /// Outer ring radius (touch area for Fixed mode)
        float outerRadius = 100.0f;

        /// Inner knob radius
        float innerRadius = 40.0f;

        /// Dead zone (0.0 to 1.0)
        float deadZone = 0.1f;

        /// Maximum distance the knob can move from center
        float maxDistance = 60.0f;

        /// Whether to clamp output to unit circle
        bool normalizeOutput = true;

        /// Which side of the screen (for auto-detection)
        /// < 0.5 = left side, >= 0.5 = right side
        float screenSideThreshold = 0.5f;

        /// Fade out time when not touched (seconds)
        float fadeOutTime = 0.3f;

        /// Minimum opacity when visible
        float minOpacity = 0.3f;

        /// Maximum opacity when active
        float maxOpacity = 0.8f;
    };

    /**
     * @brief Virtual joystick state for rendering
     */
    struct VirtualJoystickVisual
    {
        /// Is the joystick currently visible?
        bool visible = false;

        /// Is the joystick currently being touched?
        bool active = false;

        /// Current opacity (0.0 to 1.0)
        float opacity = 0.0f;

        /// Center position of the outer ring
        float outerX = 0.0f;
        float outerY = 0.0f;
        float outerRadius = 0.0f;

        /// Position of the inner knob
        float innerX = 0.0f;
        float innerY = 0.0f;
        float innerRadius = 0.0f;
    };

    /**
     * @brief Virtual on-screen joystick
     */
    class VirtualJoystick
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        VirtualJoystick();
        explicit VirtualJoystick(const VirtualJoystickConfig& config);
        ~VirtualJoystick() = default;

        // =====================================================================
        // Configuration
        // =====================================================================

        /**
         * @brief Set joystick configuration
         */
        void SetConfig(const VirtualJoystickConfig& config);

        /**
         * @brief Get current configuration
         */
        const VirtualJoystickConfig& GetConfig() const { return m_config; }

        /**
         * @brief Set screen dimensions (for position calculations)
         */
        void SetScreenSize(float width, float height);

        // =====================================================================
        // Input Processing
        // =====================================================================

        /**
         * @brief Process touch input
         * @param touch Current touch state
         * @param deltaTime Frame delta time
         */
        void Process(const TouchState& touch, float deltaTime);

        /**
         * @brief Reset joystick state
         */
        void Reset();

        // =====================================================================
        // Output
        // =====================================================================

        /**
         * @brief Get joystick X axis value (-1 to 1)
         */
        float GetX() const { return m_outputX; }

        /**
         * @brief Get joystick Y axis value (-1 to 1)
         */
        float GetY() const { return m_outputY; }

        /**
         * @brief Get normalized direction (length 0 to 1)
         */
        float GetMagnitude() const;

        /**
         * @brief Get angle in radians (0 = right, PI/2 = up)
         */
        float GetAngle() const;

        /**
         * @brief Check if joystick is currently being touched
         */
        bool IsActive() const { return m_isActive; }

        /**
         * @brief Get visual state for rendering
         */
        VirtualJoystickVisual GetVisual() const;

        // =====================================================================
        // Touch Filtering
        // =====================================================================

        /**
         * @brief Set the touch ID that this joystick is tracking
         * @return true if this touch was claimed
         */
        bool TryClaimTouch(const TouchPoint& point);

        /**
         * @brief Check if a touch point is within the joystick's activation area
         */
        bool IsInActivationArea(float x, float y) const;

        /**
         * @brief Get the ID of the tracked touch (0 = none)
         */
        uint32 GetTrackedTouchId() const { return m_trackedTouchId; }

    private:
        // =====================================================================
        // Internal Methods
        // =====================================================================

        void ProcessTouch(const TouchPoint* point, float deltaTime);
        void UpdateVisual(float deltaTime);
        float ApplyDeadZone(float value) const;

        // =====================================================================
        // Data
        // =====================================================================

        VirtualJoystickConfig m_config;

        // Screen dimensions
        float m_screenWidth = 1920.0f;
        float m_screenHeight = 1080.0f;

        // Current output
        float m_outputX = 0.0f;
        float m_outputY = 0.0f;

        // State
        bool m_isActive = false;
        uint32 m_trackedTouchId = 0;

        // Visual state
        float m_currentCenterX = 0.0f;
        float m_currentCenterY = 0.0f;
        float m_knobX = 0.0f;
        float m_knobY = 0.0f;
        float m_opacity = 0.0f;
        float m_fadeTimer = 0.0f;
    };

} // namespace RVX
