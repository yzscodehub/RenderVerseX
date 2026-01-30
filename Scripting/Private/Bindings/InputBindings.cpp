#include "Scripting/Bindings/InputBindings.h"
#include "Scripting/LuaState.h"
#include "Core/Services.h"
#include "Core/Log.h"

// Forward declare InputSubsystem - we'll check at runtime if it's available
namespace RVX
{
    class InputSubsystem;
}

namespace RVX::Bindings
{
    namespace
    {
        // =====================================================================
        // Input State Cache (updated each frame by ScriptEngine)
        // =====================================================================
        
        struct InputStateCache
        {
            // Keyboard
            bool keys[512] = {};
            bool keysPressed[512] = {};
            bool keysReleased[512] = {};
            
            // Mouse
            float mouseX = 0.0f;
            float mouseY = 0.0f;
            float mouseDeltaX = 0.0f;
            float mouseDeltaY = 0.0f;
            float scrollX = 0.0f;
            float scrollY = 0.0f;
            bool mouseButtons[8] = {};
            bool mouseButtonsPressed[8] = {};
            bool mouseButtonsReleased[8] = {};
        };
        
        InputStateCache s_inputCache;

        // =====================================================================
        // Common Key Codes (matching GLFW)
        // =====================================================================
        
        // These are the most commonly used key codes
        // Full list can be extended as needed
        
        enum class Key : int
        {
            // Unknown
            Unknown = -1,
            
            // Printable keys
            Space = 32,
            Apostrophe = 39,
            Comma = 44,
            Minus = 45,
            Period = 46,
            Slash = 47,
            
            Num0 = 48, Num1 = 49, Num2 = 50, Num3 = 51, Num4 = 52,
            Num5 = 53, Num6 = 54, Num7 = 55, Num8 = 56, Num9 = 57,
            
            Semicolon = 59,
            Equal = 61,
            
            A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72,
            I = 73, J = 74, K = 75, L = 76, M = 77, N = 78, O = 79, P = 80,
            Q = 81, R = 82, S = 83, T = 84, U = 85, V = 86, W = 87, X = 88,
            Y = 89, Z = 90,
            
            LeftBracket = 91,
            Backslash = 92,
            RightBracket = 93,
            GraveAccent = 96,
            
            // Function keys
            Escape = 256,
            Enter = 257,
            Tab = 258,
            Backspace = 259,
            Insert = 260,
            Delete = 261,
            Right = 262,
            Left = 263,
            Down = 264,
            Up = 265,
            PageUp = 266,
            PageDown = 267,
            Home = 268,
            End = 269,
            CapsLock = 280,
            ScrollLock = 281,
            NumLock = 282,
            PrintScreen = 283,
            Pause = 284,
            
            F1 = 290, F2 = 291, F3 = 292, F4 = 293, F5 = 294, F6 = 295,
            F7 = 296, F8 = 297, F9 = 298, F10 = 299, F11 = 300, F12 = 301,
            
            // Keypad
            KP0 = 320, KP1 = 321, KP2 = 322, KP3 = 323, KP4 = 324,
            KP5 = 325, KP6 = 326, KP7 = 327, KP8 = 328, KP9 = 329,
            KPDecimal = 330,
            KPDivide = 331,
            KPMultiply = 332,
            KPSubtract = 333,
            KPAdd = 334,
            KPEnter = 335,
            KPEqual = 336,
            
            // Modifiers
            LeftShift = 340,
            LeftControl = 341,
            LeftAlt = 342,
            LeftSuper = 343,
            RightShift = 344,
            RightControl = 345,
            RightAlt = 346,
            RightSuper = 347,
            Menu = 348
        };

        enum class MouseButton : int
        {
            Left = 0,
            Right = 1,
            Middle = 2,
            Button4 = 3,
            Button5 = 4,
            Button6 = 5,
            Button7 = 6,
            Button8 = 7
        };

        // =====================================================================
        // Input Query Functions
        // =====================================================================

        bool IsKeyDown(int keyCode)
        {
            if (keyCode < 0 || keyCode >= 512) return false;
            return s_inputCache.keys[keyCode];
        }

        bool IsKeyPressed(int keyCode)
        {
            if (keyCode < 0 || keyCode >= 512) return false;
            return s_inputCache.keysPressed[keyCode];
        }

        bool IsKeyReleased(int keyCode)
        {
            if (keyCode < 0 || keyCode >= 512) return false;
            return s_inputCache.keysReleased[keyCode];
        }

        bool IsMouseButtonDown(int button)
        {
            if (button < 0 || button >= 8) return false;
            return s_inputCache.mouseButtons[button];
        }

        bool IsMouseButtonPressed(int button)
        {
            if (button < 0 || button >= 8) return false;
            return s_inputCache.mouseButtonsPressed[button];
        }

        bool IsMouseButtonReleased(int button)
        {
            if (button < 0 || button >= 8) return false;
            return s_inputCache.mouseButtonsReleased[button];
        }

        std::tuple<float, float> GetMousePosition()
        {
            return { s_inputCache.mouseX, s_inputCache.mouseY };
        }

        std::tuple<float, float> GetMouseDelta()
        {
            return { s_inputCache.mouseDeltaX, s_inputCache.mouseDeltaY };
        }

        std::tuple<float, float> GetScrollDelta()
        {
            return { s_inputCache.scrollX, s_inputCache.scrollY };
        }

    } // anonymous namespace

    void RegisterInputBindings(LuaState& lua)
    {
        sol::state& state = lua.GetState();
        sol::table rvx = lua.GetOrCreateNamespace("RVX");

        // =====================================================================
        // Key enum
        // =====================================================================
        state.new_enum<Key>("Key",
            {
                // Common keys
                {"Unknown", Key::Unknown},
                {"Space", Key::Space},
                {"Apostrophe", Key::Apostrophe},
                {"Comma", Key::Comma},
                {"Minus", Key::Minus},
                {"Period", Key::Period},
                {"Slash", Key::Slash},
                
                // Numbers
                {"Num0", Key::Num0}, {"Num1", Key::Num1}, {"Num2", Key::Num2},
                {"Num3", Key::Num3}, {"Num4", Key::Num4}, {"Num5", Key::Num5},
                {"Num6", Key::Num6}, {"Num7", Key::Num7}, {"Num8", Key::Num8},
                {"Num9", Key::Num9},
                
                // Letters
                {"A", Key::A}, {"B", Key::B}, {"C", Key::C}, {"D", Key::D},
                {"E", Key::E}, {"F", Key::F}, {"G", Key::G}, {"H", Key::H},
                {"I", Key::I}, {"J", Key::J}, {"K", Key::K}, {"L", Key::L},
                {"M", Key::M}, {"N", Key::N}, {"O", Key::O}, {"P", Key::P},
                {"Q", Key::Q}, {"R", Key::R}, {"S", Key::S}, {"T", Key::T},
                {"U", Key::U}, {"V", Key::V}, {"W", Key::W}, {"X", Key::X},
                {"Y", Key::Y}, {"Z", Key::Z},
                
                // Function keys
                {"Escape", Key::Escape},
                {"Enter", Key::Enter},
                {"Tab", Key::Tab},
                {"Backspace", Key::Backspace},
                {"Insert", Key::Insert},
                {"Delete", Key::Delete},
                {"Right", Key::Right},
                {"Left", Key::Left},
                {"Down", Key::Down},
                {"Up", Key::Up},
                {"PageUp", Key::PageUp},
                {"PageDown", Key::PageDown},
                {"Home", Key::Home},
                {"End", Key::End},
                {"CapsLock", Key::CapsLock},
                {"ScrollLock", Key::ScrollLock},
                {"NumLock", Key::NumLock},
                {"PrintScreen", Key::PrintScreen},
                {"Pause", Key::Pause},
                
                {"F1", Key::F1}, {"F2", Key::F2}, {"F3", Key::F3}, {"F4", Key::F4},
                {"F5", Key::F5}, {"F6", Key::F6}, {"F7", Key::F7}, {"F8", Key::F8},
                {"F9", Key::F9}, {"F10", Key::F10}, {"F11", Key::F11}, {"F12", Key::F12},
                
                // Keypad
                {"KP0", Key::KP0}, {"KP1", Key::KP1}, {"KP2", Key::KP2},
                {"KP3", Key::KP3}, {"KP4", Key::KP4}, {"KP5", Key::KP5},
                {"KP6", Key::KP6}, {"KP7", Key::KP7}, {"KP8", Key::KP8},
                {"KP9", Key::KP9},
                {"KPDecimal", Key::KPDecimal},
                {"KPDivide", Key::KPDivide},
                {"KPMultiply", Key::KPMultiply},
                {"KPSubtract", Key::KPSubtract},
                {"KPAdd", Key::KPAdd},
                {"KPEnter", Key::KPEnter},
                {"KPEqual", Key::KPEqual},
                
                // Modifiers
                {"LeftShift", Key::LeftShift},
                {"LeftControl", Key::LeftControl},
                {"LeftAlt", Key::LeftAlt},
                {"LeftSuper", Key::LeftSuper},
                {"RightShift", Key::RightShift},
                {"RightControl", Key::RightControl},
                {"RightAlt", Key::RightAlt},
                {"RightSuper", Key::RightSuper},
                {"Menu", Key::Menu}
            }
        );

        // =====================================================================
        // MouseButton enum
        // =====================================================================
        state.new_enum<MouseButton>("MouseButton",
            {
                {"Left", MouseButton::Left},
                {"Right", MouseButton::Right},
                {"Middle", MouseButton::Middle},
                {"Button4", MouseButton::Button4},
                {"Button5", MouseButton::Button5},
                {"Button6", MouseButton::Button6},
                {"Button7", MouseButton::Button7},
                {"Button8", MouseButton::Button8}
            }
        );

        // =====================================================================
        // Input namespace
        // =====================================================================
        sol::table input = state.create_table();

        // Keyboard
        input["IsKeyDown"] = [](sol::object keyArg) {
            int keyCode = keyArg.is<Key>() ? static_cast<int>(keyArg.as<Key>()) : keyArg.as<int>();
            return IsKeyDown(keyCode);
        };
        input["IsKeyPressed"] = [](sol::object keyArg) {
            int keyCode = keyArg.is<Key>() ? static_cast<int>(keyArg.as<Key>()) : keyArg.as<int>();
            return IsKeyPressed(keyCode);
        };
        input["IsKeyReleased"] = [](sol::object keyArg) {
            int keyCode = keyArg.is<Key>() ? static_cast<int>(keyArg.as<Key>()) : keyArg.as<int>();
            return IsKeyReleased(keyCode);
        };

        // Mouse buttons
        input["IsMouseButtonDown"] = [](sol::object btnArg) {
            int btn = btnArg.is<MouseButton>() ? static_cast<int>(btnArg.as<MouseButton>()) : btnArg.as<int>();
            return IsMouseButtonDown(btn);
        };
        input["IsMouseButtonPressed"] = [](sol::object btnArg) {
            int btn = btnArg.is<MouseButton>() ? static_cast<int>(btnArg.as<MouseButton>()) : btnArg.as<int>();
            return IsMouseButtonPressed(btn);
        };
        input["IsMouseButtonReleased"] = [](sol::object btnArg) {
            int btn = btnArg.is<MouseButton>() ? static_cast<int>(btnArg.as<MouseButton>()) : btnArg.as<int>();
            return IsMouseButtonReleased(btn);
        };

        // Mouse position
        input["GetMousePosition"] = &GetMousePosition;
        input["GetMouseX"] = []() { return s_inputCache.mouseX; };
        input["GetMouseY"] = []() { return s_inputCache.mouseY; };
        input["GetMouseDelta"] = &GetMouseDelta;
        input["GetMouseDeltaX"] = []() { return s_inputCache.mouseDeltaX; };
        input["GetMouseDeltaY"] = []() { return s_inputCache.mouseDeltaY; };

        // Scroll
        input["GetScrollDelta"] = &GetScrollDelta;
        input["GetScrollX"] = []() { return s_inputCache.scrollX; };
        input["GetScrollY"] = []() { return s_inputCache.scrollY; };

        // Convenience functions
        input["GetAxis"] = [](const std::string& name) -> float {
            // Simple axis implementation for common cases
            if (name == "Horizontal") {
                float value = 0.0f;
                if (IsKeyDown(static_cast<int>(Key::D)) || IsKeyDown(static_cast<int>(Key::Right))) value += 1.0f;
                if (IsKeyDown(static_cast<int>(Key::A)) || IsKeyDown(static_cast<int>(Key::Left))) value -= 1.0f;
                return value;
            }
            else if (name == "Vertical") {
                float value = 0.0f;
                if (IsKeyDown(static_cast<int>(Key::W)) || IsKeyDown(static_cast<int>(Key::Up))) value += 1.0f;
                if (IsKeyDown(static_cast<int>(Key::S)) || IsKeyDown(static_cast<int>(Key::Down))) value -= 1.0f;
                return value;
            }
            else if (name == "MouseX") {
                return s_inputCache.mouseDeltaX;
            }
            else if (name == "MouseY") {
                return s_inputCache.mouseDeltaY;
            }
            return 0.0f;
        };

        // Store in RVX namespace
        rvx["Input"] = input;
        rvx["Key"] = state["Key"];
        rvx["MouseButton"] = state["MouseButton"];

        RVX_CORE_INFO("InputBindings registered");
    }

} // namespace RVX::Bindings
