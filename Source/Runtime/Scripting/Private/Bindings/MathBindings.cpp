#include "Scripting/Bindings/MathBindings.h"
#include "Scripting/LuaState.h"
#include "Core/MathTypes.h"
#include "Core/Log.h"

#include <sstream>
#include <cmath>

namespace RVX::Bindings
{
    void RegisterMathBindings(LuaState& lua)
    {
        sol::state& state = lua.GetState();
        sol::table rvx = lua.GetOrCreateNamespace("RVX");

        // =====================================================================
        // Vec2
        // =====================================================================
        state.new_usertype<Vec2>("Vec2",
            sol::constructors<
                Vec2(),
                Vec2(float),
                Vec2(float, float)
            >(),
            
            // Members
            "x", &Vec2::x,
            "y", &Vec2::y,
            
            // Operators
            sol::meta_function::addition, [](const Vec2& a, const Vec2& b) { return a + b; },
            sol::meta_function::subtraction, [](const Vec2& a, const Vec2& b) { return a - b; },
            sol::meta_function::multiplication, sol::overload(
                [](const Vec2& a, const Vec2& b) { return a * b; },
                [](const Vec2& a, float s) { return a * s; },
                [](float s, const Vec2& a) { return s * a; }
            ),
            sol::meta_function::division, sol::overload(
                [](const Vec2& a, const Vec2& b) { return a / b; },
                [](const Vec2& a, float s) { return a / s; }
            ),
            sol::meta_function::unary_minus, [](const Vec2& a) { return -a; },
            sol::meta_function::equal_to, [](const Vec2& a, const Vec2& b) { return a == b; },
            sol::meta_function::to_string, [](const Vec2& v) {
                std::ostringstream oss;
                oss << "Vec2(" << v.x << ", " << v.y << ")";
                return oss.str();
            },
            
            // Methods
            "Length", [](const Vec2& v) { return glm::length(v); },
            "LengthSquared", [](const Vec2& v) { return glm::dot(v, v); },
            "Normalize", [](const Vec2& v) { return glm::normalize(v); },
            "Dot", [](const Vec2& a, const Vec2& b) { return glm::dot(a, b); },
            
            // Static constructors
            "Zero", sol::var(Vec2(0.0f)),
            "One", sol::var(Vec2(1.0f)),
            "UnitX", sol::var(Vec2(1.0f, 0.0f)),
            "UnitY", sol::var(Vec2(0.0f, 1.0f))
        );

        // =====================================================================
        // Vec3
        // =====================================================================
        state.new_usertype<Vec3>("Vec3",
            sol::constructors<
                Vec3(),
                Vec3(float),
                Vec3(float, float, float),
                Vec3(const Vec2&, float)
            >(),
            
            // Members
            "x", &Vec3::x,
            "y", &Vec3::y,
            "z", &Vec3::z,
            
            // Operators
            sol::meta_function::addition, [](const Vec3& a, const Vec3& b) { return a + b; },
            sol::meta_function::subtraction, [](const Vec3& a, const Vec3& b) { return a - b; },
            sol::meta_function::multiplication, sol::overload(
                [](const Vec3& a, const Vec3& b) { return a * b; },
                [](const Vec3& a, float s) { return a * s; },
                [](float s, const Vec3& a) { return s * a; }
            ),
            sol::meta_function::division, sol::overload(
                [](const Vec3& a, const Vec3& b) { return a / b; },
                [](const Vec3& a, float s) { return a / s; }
            ),
            sol::meta_function::unary_minus, [](const Vec3& a) { return -a; },
            sol::meta_function::equal_to, [](const Vec3& a, const Vec3& b) { return a == b; },
            sol::meta_function::to_string, [](const Vec3& v) {
                std::ostringstream oss;
                oss << "Vec3(" << v.x << ", " << v.y << ", " << v.z << ")";
                return oss.str();
            },
            
            // Methods
            "Length", [](const Vec3& v) { return glm::length(v); },
            "LengthSquared", [](const Vec3& v) { return glm::dot(v, v); },
            "Normalize", [](const Vec3& v) { return glm::normalize(v); },
            "Dot", [](const Vec3& a, const Vec3& b) { return glm::dot(a, b); },
            "Cross", [](const Vec3& a, const Vec3& b) { return glm::cross(a, b); },
            "Distance", [](const Vec3& a, const Vec3& b) { return glm::distance(a, b); },
            
            // Static constructors
            "Zero", sol::var(Vec3(0.0f)),
            "One", sol::var(Vec3(1.0f)),
            "UnitX", sol::var(Vec3(1.0f, 0.0f, 0.0f)),
            "UnitY", sol::var(Vec3(0.0f, 1.0f, 0.0f)),
            "UnitZ", sol::var(Vec3(0.0f, 0.0f, 1.0f)),
            "Up", sol::var(Vec3(0.0f, 1.0f, 0.0f)),
            "Down", sol::var(Vec3(0.0f, -1.0f, 0.0f)),
            "Forward", sol::var(Vec3(0.0f, 0.0f, -1.0f)),
            "Back", sol::var(Vec3(0.0f, 0.0f, 1.0f)),
            "Right", sol::var(Vec3(1.0f, 0.0f, 0.0f)),
            "Left", sol::var(Vec3(-1.0f, 0.0f, 0.0f))
        );

        // =====================================================================
        // Vec4
        // =====================================================================
        state.new_usertype<Vec4>("Vec4",
            sol::constructors<
                Vec4(),
                Vec4(float),
                Vec4(float, float, float, float),
                Vec4(const Vec3&, float),
                Vec4(const Vec2&, float, float)
            >(),
            
            // Members
            "x", &Vec4::x,
            "y", &Vec4::y,
            "z", &Vec4::z,
            "w", &Vec4::w,
            
            // Operators
            sol::meta_function::addition, [](const Vec4& a, const Vec4& b) { return a + b; },
            sol::meta_function::subtraction, [](const Vec4& a, const Vec4& b) { return a - b; },
            sol::meta_function::multiplication, sol::overload(
                [](const Vec4& a, const Vec4& b) { return a * b; },
                [](const Vec4& a, float s) { return a * s; },
                [](float s, const Vec4& a) { return s * a; }
            ),
            sol::meta_function::division, sol::overload(
                [](const Vec4& a, const Vec4& b) { return a / b; },
                [](const Vec4& a, float s) { return a / s; }
            ),
            sol::meta_function::unary_minus, [](const Vec4& a) { return -a; },
            sol::meta_function::equal_to, [](const Vec4& a, const Vec4& b) { return a == b; },
            sol::meta_function::to_string, [](const Vec4& v) {
                std::ostringstream oss;
                oss << "Vec4(" << v.x << ", " << v.y << ", " << v.z << ", " << v.w << ")";
                return oss.str();
            },
            
            // Methods
            "Length", [](const Vec4& v) { return glm::length(v); },
            "LengthSquared", [](const Vec4& v) { return glm::dot(v, v); },
            "Normalize", [](const Vec4& v) { return glm::normalize(v); },
            "Dot", [](const Vec4& a, const Vec4& b) { return glm::dot(a, b); },
            
            // Swizzle helpers
            "XYZ", [](const Vec4& v) { return Vec3(v.x, v.y, v.z); },
            "XY", [](const Vec4& v) { return Vec2(v.x, v.y); },
            
            // Static constructors
            "Zero", sol::var(Vec4(0.0f)),
            "One", sol::var(Vec4(1.0f))
        );

        // =====================================================================
        // Quat (Quaternion)
        // =====================================================================
        state.new_usertype<Quat>("Quat",
            sol::constructors<
                Quat(),
                Quat(float, float, float, float)  // w, x, y, z
            >(),
            
            // Members
            "w", &Quat::w,
            "x", &Quat::x,
            "y", &Quat::y,
            "z", &Quat::z,
            
            // Operators
            sol::meta_function::multiplication, sol::overload(
                [](const Quat& a, const Quat& b) { return a * b; },
                [](const Quat& q, const Vec3& v) { return q * v; }
            ),
            sol::meta_function::equal_to, [](const Quat& a, const Quat& b) { return a == b; },
            sol::meta_function::to_string, [](const Quat& q) {
                std::ostringstream oss;
                oss << "Quat(" << q.w << ", " << q.x << ", " << q.y << ", " << q.z << ")";
                return oss.str();
            },
            
            // Methods
            "Normalize", [](const Quat& q) { return glm::normalize(q); },
            "Inverse", [](const Quat& q) { return glm::inverse(q); },
            "Conjugate", [](const Quat& q) { return glm::conjugate(q); },
            "Dot", [](const Quat& a, const Quat& b) { return glm::dot(a, b); },
            "ToEuler", [](const Quat& q) { return glm::eulerAngles(q); },
            "ToMat4", [](const Quat& q) { return glm::mat4_cast(q); },
            
            // Static constructors
            "Identity", sol::var(Quat(1.0f, 0.0f, 0.0f, 0.0f)),
            "FromAxisAngle", [](const Vec3& axis, float angleRadians) {
                return glm::angleAxis(angleRadians, axis);
            },
            "FromEuler", [](const Vec3& eulerRadians) {
                return Quat(eulerRadians);
            },
            "FromEulerDegrees", [](float pitch, float yaw, float roll) {
                return Quat(Vec3(glm::radians(pitch), glm::radians(yaw), glm::radians(roll)));
            },
            "Slerp", [](const Quat& a, const Quat& b, float t) {
                return glm::slerp(a, b, t);
            },
            "LookRotation", [](const Vec3& forward, const Vec3& up) {
                Mat4 m = glm::lookAt(Vec3(0.0f), forward, up);
                return glm::conjugate(glm::quat_cast(m));
            }
        );

        // =====================================================================
        // Mat4 (4x4 Matrix)
        // =====================================================================
        state.new_usertype<Mat4>("Mat4",
            sol::constructors<
                Mat4(),
                Mat4(float)  // Diagonal
            >(),
            
            // Operators
            sol::meta_function::multiplication, sol::overload(
                [](const Mat4& a, const Mat4& b) { return a * b; },
                [](const Mat4& m, const Vec4& v) { return m * v; }
            ),
            sol::meta_function::equal_to, [](const Mat4& a, const Mat4& b) { return a == b; },
            sol::meta_function::to_string, [](const Mat4&) { return "Mat4(...)"; },
            
            // Element access
            "Get", [](const Mat4& m, int col, int row) { return m[col][row]; },
            "Set", [](Mat4& m, int col, int row, float val) { m[col][row] = val; },
            "GetColumn", [](const Mat4& m, int col) { return m[col]; },
            
            // Methods
            "Inverse", [](const Mat4& m) { return glm::inverse(m); },
            "Transpose", [](const Mat4& m) { return glm::transpose(m); },
            "Determinant", [](const Mat4& m) { return glm::determinant(m); },
            
            // Decomposition
            "GetTranslation", [](const Mat4& m) { return Vec3(m[3]); },
            "GetScale", [](const Mat4& m) {
                return Vec3(
                    glm::length(Vec3(m[0])),
                    glm::length(Vec3(m[1])),
                    glm::length(Vec3(m[2]))
                );
            },
            "GetRotation", [](const Mat4& m) { return glm::quat_cast(m); },
            
            // Transform point/direction
            "TransformPoint", [](const Mat4& m, const Vec3& p) {
                Vec4 result = m * Vec4(p, 1.0f);
                return Vec3(result) / result.w;
            },
            "TransformDirection", [](const Mat4& m, const Vec3& d) {
                return Vec3(m * Vec4(d, 0.0f));
            },
            
            // Static constructors
            "Identity", sol::var(Mat4(1.0f)),
            "Translation", [](const Vec3& t) { return glm::translate(Mat4(1.0f), t); },
            "Rotation", [](const Quat& q) { return glm::mat4_cast(q); },
            "RotationAxisAngle", [](const Vec3& axis, float angleRadians) {
                return glm::rotate(Mat4(1.0f), angleRadians, axis);
            },
            "Scale", sol::overload(
                [](const Vec3& s) { return glm::scale(Mat4(1.0f), s); },
                [](float s) { return glm::scale(Mat4(1.0f), Vec3(s)); }
            ),
            "LookAt", [](const Vec3& eye, const Vec3& target, const Vec3& up) {
                return glm::lookAt(eye, target, up);
            },
            "Perspective", [](float fovRadians, float aspect, float nearZ, float farZ) {
                return glm::perspective(fovRadians, aspect, nearZ, farZ);
            },
            "Ortho", [](float left, float right, float bottom, float top, float nearZ, float farZ) {
                return glm::ortho(left, right, bottom, top, nearZ, farZ);
            },
            "TRS", [](const Vec3& translation, const Quat& rotation, const Vec3& scale) {
                Mat4 m = glm::translate(Mat4(1.0f), translation);
                m = m * glm::mat4_cast(rotation);
                m = glm::scale(m, scale);
                return m;
            }
        );

        // =====================================================================
        // Math namespace with utility functions
        // =====================================================================
        sol::table math = state.create_table();
        
        // Constants
        math["PI"] = glm::pi<float>();
        math["TAU"] = glm::two_pi<float>();
        math["E"] = glm::e<float>();
        math["DEG2RAD"] = glm::pi<float>() / 180.0f;
        math["RAD2DEG"] = 180.0f / glm::pi<float>();
        
        // Angle conversion
        math["Radians"] = [](float degrees) { return glm::radians(degrees); };
        math["Degrees"] = [](float radians) { return glm::degrees(radians); };
        
        // Interpolation
        math["Lerp"] = [](float a, float b, float t) { return glm::mix(a, b, t); };
        math["LerpVec3"] = [](const Vec3& a, const Vec3& b, float t) { return glm::mix(a, b, t); };
        math["SmoothStep"] = [](float edge0, float edge1, float x) { return glm::smoothstep(edge0, edge1, x); };
        math["InverseLerp"] = [](float a, float b, float value) { return (value - a) / (b - a); };
        
        // Clamping
        math["Clamp"] = [](float x, float minVal, float maxVal) { return glm::clamp(x, minVal, maxVal); };
        math["Clamp01"] = [](float x) { return glm::clamp(x, 0.0f, 1.0f); };
        math["Min"] = sol::overload(
            [](float a, float b) { return std::min(a, b); },
            [](const Vec3& a, const Vec3& b) { return glm::min(a, b); }
        );
        math["Max"] = sol::overload(
            [](float a, float b) { return std::max(a, b); },
            [](const Vec3& a, const Vec3& b) { return glm::max(a, b); }
        );
        math["Abs"] = sol::overload(
            [](float x) { return std::abs(x); },
            [](const Vec3& v) { return glm::abs(v); }
        );
        
        // Trigonometry
        math["Sin"] = [](float x) { return std::sin(x); };
        math["Cos"] = [](float x) { return std::cos(x); };
        math["Tan"] = [](float x) { return std::tan(x); };
        math["Asin"] = [](float x) { return std::asin(x); };
        math["Acos"] = [](float x) { return std::acos(x); };
        math["Atan"] = [](float x) { return std::atan(x); };
        math["Atan2"] = [](float y, float x) { return std::atan2(y, x); };
        
        // Power/Exponential
        math["Sqrt"] = [](float x) { return std::sqrt(x); };
        math["Pow"] = [](float base, float exp) { return std::pow(base, exp); };
        math["Exp"] = [](float x) { return std::exp(x); };
        math["Log"] = [](float x) { return std::log(x); };
        math["Log10"] = [](float x) { return std::log10(x); };
        
        // Rounding
        math["Floor"] = [](float x) { return std::floor(x); };
        math["Ceil"] = [](float x) { return std::ceil(x); };
        math["Round"] = [](float x) { return std::round(x); };
        math["Fract"] = [](float x) { return glm::fract(x); };
        math["Mod"] = [](float x, float y) { return std::fmod(x, y); };
        
        // Sign
        math["Sign"] = [](float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); };
        
        rvx["Math"] = math;

        RVX_CORE_INFO("MathBindings registered");
    }

} // namespace RVX::Bindings
