--[[
    Math Bindings Example
    Demonstrates the math types available in Lua.
]]

-- Test script - not meant to be attached to an entity
-- Run directly via ScriptEngine:ExecuteFile()

print("=== RenderVerseX Math Bindings Demo ===")
print("")

-- Vec2
print("-- Vec2 --")
local v2a = Vec2(3, 4)
local v2b = Vec2(1, 2)
print("v2a = " .. tostring(v2a))
print("v2b = " .. tostring(v2b))
print("v2a + v2b = " .. tostring(v2a + v2b))
print("v2a:Length() = " .. v2a:Length())
print("v2a:Dot(v2b) = " .. v2a:Dot(v2b))
print("")

-- Vec3
print("-- Vec3 --")
local v3a = Vec3(1, 2, 3)
local v3b = Vec3(4, 5, 6)
print("v3a = " .. tostring(v3a))
print("v3b = " .. tostring(v3b))
print("v3a + v3b = " .. tostring(v3a + v3b))
print("v3a * 2 = " .. tostring(v3a * 2))
print("v3a:Cross(v3b) = " .. tostring(v3a:Cross(v3b)))
print("v3a:Normalize() = " .. tostring(v3a:Normalize()))
print("Vec3.Up = " .. tostring(Vec3.Up))
print("Vec3.Forward = " .. tostring(Vec3.Forward))
print("")

-- Vec4
print("-- Vec4 --")
local v4 = Vec4(1, 2, 3, 4)
print("v4 = " .. tostring(v4))
print("v4:XYZ() = " .. tostring(v4:XYZ()))
print("")

-- Quat
print("-- Quat --")
local q1 = Quat.Identity
local q2 = Quat.FromAxisAngle(Vec3.Up, RVX.Math.Radians(90))
print("Identity = " .. tostring(q1))
print("90° around Y = " .. tostring(q2))
print("Euler of q2 = " .. tostring(RVX.Math.Degrees(q2:ToEuler().y)) .. " degrees")
print("")

-- Mat4
print("-- Mat4 --")
local translation = Mat4.Translation(Vec3(10, 20, 30))
local rotation = Mat4.Rotation(Quat.FromEulerDegrees(0, 45, 0))
local scale = Mat4.Scale(Vec3(2, 2, 2))
local trs = Mat4.TRS(Vec3(1, 2, 3), Quat.FromEulerDegrees(0, 90, 0), Vec3(1, 1, 1))
print("Translation matrix created")
print("TRS matrix translation = " .. tostring(trs:GetTranslation()))
print("")

-- Math utilities
print("-- Math Utilities --")
print("PI = " .. RVX.Math.PI)
print("Radians(90) = " .. RVX.Math.Radians(90))
print("Degrees(PI) = " .. RVX.Math.Degrees(RVX.Math.PI))
print("Lerp(0, 10, 0.5) = " .. RVX.Math.Lerp(0, 10, 0.5))
print("Clamp(15, 0, 10) = " .. RVX.Math.Clamp(15, 0, 10))
print("Sin(PI/2) = " .. RVX.Math.Sin(RVX.Math.PI / 2))
print("")

print("=== Math Demo Complete ===")
