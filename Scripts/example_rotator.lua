--[[
    Example Rotator Script
    A simple script that rotates an entity continuously.
    
    Demonstrates:
    - Basic script lifecycle
    - Math bindings usage
    - Entity transform manipulation
]]

Rotator = {}

function Rotator:OnStart()
    print("Rotator started on: " .. (self.entity and self.entity:GetName() or "unknown"))
    
    -- Configurable rotation speed (degrees per second)
    self.rotationSpeed = Vec3(0, 45, 0)  -- Rotate around Y axis
    
    -- Optional: oscillate scale
    self.scaleOscillate = false
    self.scaleAmount = 0.2
    self.scaleSpeed = 2.0
    self.time = 0
end

function Rotator:OnUpdate(deltaTime)
    if not self.entity then return end
    
    self.time = self.time + deltaTime
    
    -- Rotate the entity
    local rotRadians = self.rotationSpeed * RVX.Math.DEG2RAD * deltaTime
    local rotation = Quat.FromEuler(rotRadians)
    self.entity:Rotate(rotation)
    
    -- Optional scale oscillation
    if self.scaleOscillate then
        local scaleOffset = RVX.Math.Sin(self.time * self.scaleSpeed) * self.scaleAmount
        local baseScale = 1.0 + scaleOffset
        self.entity:SetScale(Vec3(baseScale, baseScale, baseScale))
    end
end

function Rotator:SetRotationSpeed(x, y, z)
    self.rotationSpeed = Vec3(x, y, z)
end

function Rotator:EnableScaleOscillation(enable)
    self.scaleOscillate = enable
end

return Rotator
