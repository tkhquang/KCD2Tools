--[[
Loot Beacon - Never Miss a Drop or Corpse
Author: tkhquang
Source: https://github.com/tkhquang/KCD2Tools
]]

-- Bootstrap file for Loot Beacon mod
-- This file is loaded by the game engine and initializes the mod

LootBeacon = {}

-- Load all module files
function LootBeacon_LoadModules()
    local modPath = "Scripts/LootBeacon"
    local moduleFiles = {
        "/core.lua",
        "/logger.lua",
        "/config.lua",
        "/entity_detector.lua",
        "/highlighter.lua",
        "/command_registry.lua",
        "/event_handler.lua",
        "/ui_manager.lua"
    }

    -- Load modules in specific order (dependencies first)
    for _, file in ipairs(moduleFiles) do
        local fullPath = modPath .. file
        local success = Script.LoadScript(fullPath)
        if success ~= 1 then
            System.LogAlways("$4[Loot Beacon] Failed to load module: " .. fullPath)
            return false
        end
    end

    return true
end

-- Initialize the mod when this file is loaded
if LootBeacon_LoadModules() then
    if LootBeacon.Core then
        LootBeacon.Core:initialize()
    else
        System.LogAlways("$4[Loot Beacon] Failed to initialize: Core module not found")
    end
else
    System.LogAlways("$4[Loot Beacon] Failed to load required modules")
end

return LootBeacon
