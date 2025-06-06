--[[
Event Handler module for Loot Beacon mod
Registers and handles game events
]]

LootBeacon.EventHandler = {
    registered = false
}

function LootBeacon.EventHandler:registerEvents()
    if self.registered then
        return
    end

    LootBeacon.Logger:info("Registering event handlers")

    -- Register system event listeners
    UIAction.RegisterEventSystemListener(self, "System", "OnSystemStarted", "onSystemStarted")
    UIAction.RegisterEventSystemListener(self, "System", "OnGamePause", "onGamePause")
    UIAction.RegisterEventSystemListener(self, "System", "OnGameResume", "onGameResume")

    self.registered = true
    return self
end

function LootBeacon.EventHandler:unregisterEvents()
    if not self.registered then
        return
    end

    LootBeacon.Logger:info("Unregistering event handlers")

    -- Unregister system event listeners
    UIAction.UnregisterEventSystemListener(self, "OnSystemStarted")
    UIAction.UnregisterEventSystemListener(self, "OnGamePause")
    UIAction.UnregisterEventSystemListener(self, "OnGameResume")

    self.registered = false
    return self
end

function LootBeacon.EventHandler:onSystemStarted()
    LootBeacon.Logger:debug("System started event received")

    -- Additional initialization that requires the game system to be started
    -- Currently nothing specific needed
end

function LootBeacon.EventHandler:onGamePause()
    LootBeacon.Logger:debug("Game pause event received")

    -- Remove highlights when game is paused for safety
    if LootBeacon.Highlighter.isActive then
        LootBeacon.Logger:debug("Game paused - removing active highlights for safety")
        LootBeacon.Highlighter:removeAllHighlights()
    end
end

function LootBeacon.EventHandler:onGameResume()
    LootBeacon.Logger:debug("Game resume event received")
    -- Game has resumed, nothing specific to do here
end

return LootBeacon.EventHandler
