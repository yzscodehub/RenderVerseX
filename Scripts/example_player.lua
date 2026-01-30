--[[
    Example Player Script
    Demonstrates how to use the RenderVerseX Lua scripting system.
    
    This script is loaded and attached to an entity via ScriptComponent.
    The entity reference is automatically available as 'self.entity'.
]]

-- Create the Player class
Player = {}

-- Called when the script is first started
function Player:OnStart()
    print("Player script started!")
    
    -- Initialize player properties
    self.speed = 5.0
    self.rotationSpeed = 90.0  -- degrees per second
    self.jumpForce = 10.0
    self.isGrounded = true
    
    -- Cache the entity for easy access
    if self.entity then
        print("Attached to entity: " .. self.entity:GetName())
        print("Initial position: " .. tostring(self.entity:GetPosition()))
    end
end

-- Called every frame
function Player:OnUpdate(deltaTime)
    if not self.entity then return end
    
    -- Get input axes
    local horizontal = RVX.Input.GetAxis("Horizontal")
    local vertical = RVX.Input.GetAxis("Vertical")
    
    -- Calculate movement
    if horizontal ~= 0 or vertical ~= 0 then
        local moveDir = Vec3(horizontal, 0, vertical)
        moveDir = moveDir:Normalize() * self.speed * deltaTime
        
        -- Move the entity
        self.entity:Translate(moveDir)
    end
    
    -- Mouse look (if holding right mouse button)
    if RVX.Input.IsMouseButtonDown(RVX.MouseButton.Right) then
        local mouseX, mouseY = RVX.Input.GetMouseDelta()
        
        if mouseX ~= 0 then
            local rotAngle = RVX.Math.Radians(mouseX * self.rotationSpeed * deltaTime)
            self.entity:RotateAround(Vec3.Up, rotAngle)
        end
    end
    
    -- Jump with space
    if RVX.Input.IsKeyPressed(RVX.Key.Space) and self.isGrounded then
        self:Jump()
    end
    
    -- Debug: print position when P is pressed
    if RVX.Input.IsKeyPressed(RVX.Key.P) then
        local pos = self.entity:GetPosition()
        print("Current position: " .. tostring(pos))
    end
end

-- Custom jump function
function Player:Jump()
    print("Jump!")
    self.isGrounded = false
    -- In a real game, you would apply physics force here
    -- For now, just move up a bit
    self.entity:Translate(Vec3(0, self.jumpForce * RVX.Time.GetDeltaTime(), 0))
end

-- Called when the script is about to be destroyed
function Player:OnDestroy()
    print("Player script destroyed")
end

-- Return the class table
return Player
