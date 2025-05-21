--[[
Core module for Loot Beacon mod
Handles initialization and provides access to other modules
]]

LootBeacon.Core = {
    MOD_NAME = "Loot Beacon",
    VERSION = "1.4.3",
    initialized = false
}

function LootBeacon.Core:initialize()
    if self.initialized then
        return
    end

    -- Initialize the logger first to enable logging
    LootBeacon.Logger:initialize(LootBeacon.Core.MOD_NAME)
    LootBeacon.Logger:info("Initializing %s v%s", self.MOD_NAME, self.VERSION)

    -- Register console commands
    LootBeacon.CommandRegistry:initialize()

    -- Load configuration
    LootBeacon.Config:initialize()

    -- Initialize the highlighter
    LootBeacon.Highlighter:initialize()

    -- Initialize entity detector
    LootBeacon.EntityDetector:initialize()

    -- Initialize UI manager
    LootBeacon.UIManager:initialize()

    -- Register game event listeners
    LootBeacon.EventHandler:registerEvents()

    -- Mark as initialized
    self.initialized = true
    LootBeacon.Logger:info("Mod initialized successfully")
end

function LootBeacon.Core:shutdown()
    if not self.initialized then
        return
    end

    LootBeacon.Logger:info("Shutting down %s", self.MOD_NAME)

    -- Clean up highlights if active
    LootBeacon.Highlighter:removeAllHighlights()

    -- Unregister events
    LootBeacon.EventHandler:unregisterEvents()

    -- Mark as uninitialized
    self.initialized = false
end

return LootBeacon.Core
