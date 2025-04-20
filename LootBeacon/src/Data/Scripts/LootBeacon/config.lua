--[[
Configuration module for Loot Beacon mod
Handles loading and validation of mod settings
]]

LootBeacon.Config = {
    -- Default configuration values
    detectionRadius = 15.0,
    customEntityClasses = "Nest",
    itemParticleEffectPath = "loot_beacon.pillar_orange",
    humanCorpseParticleEffectPath = "loot_beacon.pillar_cyan",
    animalCorpseParticleEffectPath = "loot_beacon.pillar_blue",
    customEntityParticleEffectPath = "loot_beacon.pillar_orange",
    highlightDuration = 5.0,
    showMessage = true,
    keyBinding = "f4",
    illegalHighlightKeyBinding = "none",
    highlightItems = true,
    highlightCorpses = true,
    highlightAnimals = true,
    goodCitizenMode = false,
    treatUnconsciousAsDead = false,
    logLevel = 2, -- INFO level

    -- Internal state
    configLoaded = false,
    configPaths = {
        "Mods/LootBeacon/mod.cfg",
        "Mods/loot_beacon/mod.cfg",
        "C:/KCD2Mods/LootBeacon/mod.cfg",
        "D:/KCD2Mods/LootBeacon/mod.cfg",
    }
}

function LootBeacon.Config:initialize()
    LootBeacon.Logger:info("Loading mod configuration")

    -- Try to load from config file
    self:loadConfigFile()

    -- Apply key binding
    self:applyKeyBinding()
    self:applyIllegalHighlightKeyBinding()

    -- Log configuration summary
    self:logConfigSummary()

    return self
end

function LootBeacon.Config:loadConfigFile()
    -- Try each possible config path
    local configLoaded = false
    for _, path in ipairs(self.configPaths) do
        LootBeacon.Logger:debug("Attempting to load config from: %s", path)

        -- Reset the loaded flag before trying this path
        self.configLoaded = false

        -- Try to execute the config file
        System.ExecuteCommand("exec " .. path)

        -- Check if any parameters were successfully loaded
        if self.configLoaded then
            LootBeacon.Logger:info("Configuration successfully loaded from: %s", path)
            configLoaded = true
            break
        end
    end

    if not configLoaded then
        LootBeacon.Logger:warning("No configuration file found. Using default settings.")
    end

    return configLoaded
end

function LootBeacon.Config:applyKeyBinding()
    -- Apply the binding
    local command = string.format("bind %s loot_beacon_activate", self.keyBinding)
    System.ExecuteCommand(command)
    LootBeacon.Logger:debug("Applied key binding: %s", command)
end

function LootBeacon.Config:applyIllegalHighlightKeyBinding()
    -- Only apply if not set to "none" or empty string
    if self.illegalHighlightKeyBinding ~= "none" and self.illegalHighlightKeyBinding ~= "" then
        local command = string.format("bind %s loot_beacon_activate_illegal", self.illegalHighlightKeyBinding)
        System.ExecuteCommand(command)
        LootBeacon.Logger:debug("Applied illegal highlight key binding: %s", command)
    else
        LootBeacon.Logger:debug("Illegal highlight key binding disabled (set to 'none' or empty)")
    end
end

function LootBeacon.Config:logConfigSummary()
    LootBeacon.Logger:info("Final configuration:")
    LootBeacon.Logger:info("- Detection radius: %gm", self.detectionRadius)
    LootBeacon.Logger:info("- Item particle effect: %s", self.itemParticleEffectPath)
    LootBeacon.Logger:info("- Human corpse particle effect: %s", self.humanCorpseParticleEffectPath)
    LootBeacon.Logger:info("- Animal particle effect: %s", self.animalCorpseParticleEffectPath)
    LootBeacon.Logger:info("- Custom entity classes: %s", self.customEntityClasses)
    LootBeacon.Logger:info("- Custom entity particle effect: %s", self.customEntityParticleEffectPath)
    LootBeacon.Logger:info("- Highlight duration: %gs", self.highlightDuration)
    LootBeacon.Logger:info("- Show messages: %s", self.showMessage and "Yes" or "No")
    LootBeacon.Logger:info("- Highlight items: %s", self.highlightItems and "Yes" or "No")
    LootBeacon.Logger:info("- Highlight corpses: %s", self.highlightCorpses and "Yes" or "No")
    LootBeacon.Logger:info("- Highlight animals: %s", self.highlightAnimals and "Yes" or "No")
    LootBeacon.Logger:info("- Good Citizen Mode: %s", self.goodCitizenMode and "Yes" or "No")
    LootBeacon.Logger:info("- Treat Unconscious as Dead: %s", self.treatUnconsciousAsDead and "Yes" or "No")
    LootBeacon.Logger:info("- Key binding: %s", self.keyBinding)

    if self.illegalHighlightKeyBinding ~= "none" and self.illegalHighlightKeyBinding ~= "" then
        LootBeacon.Logger:info("- Illegal highlight key binding: %s", self.illegalHighlightKeyBinding)
    else
        LootBeacon.Logger:info("- Illegal highlight key binding: disabled")
    end
end

-- Helper function to extract numeric value from config line
function LootBeacon.Config:parseNumberFromLine(line)
    if not line then return nil end

    -- Try to parse "parameter =value" format
    local equalPattern = "=%s*([%d%.]+)"
    local match = string.match(line, equalPattern)
    if match then
        return tonumber(match)
    end

    -- Try alternative patterns
    local value = tonumber(line)
    if value ~= nil then
        return value
    end

    local pattern = "%s*([%d%.]+)"
    match = string.match(line, pattern)
    if match then
        return tonumber(match)
    end

    return nil
end

-- Helper function to extract string value from config line
function LootBeacon.Config:parseStringFromLine(line)
    if not line then return nil end

    -- Try to parse "parameter =\"value\"" format
    local equalPattern = "=%s*\"([^\"]*)\""
    local match = string.match(line, equalPattern)
    if match then
        return match
    end

    -- Try to parse "parameter =value" format
    equalPattern = "=%s*([%S]+)"
    match = string.match(line, equalPattern)
    if match then
        return match
    end

    -- Try other patterns
    local pattern = '%s*"([^"]*)"'
    match = string.match(line, pattern)
    if match then
        return match
    end

    -- Get rest of line as fallback
    local simplePattern = "%s*(.+)"
    match = string.match(line, simplePattern)
    if match then
        return match
    end

    return nil
end

-- Configuration handlers follow, for each config option
-- These are called via console commands from the config file

function LootBeacon.Config:setDetectionRadius(line)
    local value = self:parseNumberFromLine(line)
    if value and value > 0 then
        self.detectionRadius = value
        LootBeacon.Logger:info("Detection radius set to: %g meters", value)
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid detection radius, using default: %g", self.detectionRadius)
    end
end

function LootBeacon.Config:setItemParticleEffectPath(line)
    local value = self:parseStringFromLine(line)
    if value and value ~= "" then
        self.itemParticleEffectPath = value
        LootBeacon.Logger:info("Item particle effect path set to: %s", value)
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid item particle effect path, using default: %s", self.itemParticleEffectPath)
    end
end

function LootBeacon.Config:setHumanCorpseParticleEffectPath(line)
    local value = self:parseStringFromLine(line)
    if value and value ~= "" then
        self.humanCorpseParticleEffectPath = value
        LootBeacon.Logger:info("Human corpse particle effect path set to: %s", value)
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid human corpse particle effect path, using default: %s",
            self.humanCorpseParticleEffectPath)
    end
end

function LootBeacon.Config:setAnimalCorpseParticleEffectPath(line)
    local value = self:parseStringFromLine(line)
    if value and value ~= "" then
        self.animalCorpseParticleEffectPath = value
        LootBeacon.Logger:info("Animal corpse particle effect path set to: %s", value)
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid animal corpse particle effect path, using default: %s",
            self.animalCorpseParticleEffectPath)
    end
end

function LootBeacon.Config:setCustomEntityClasses(line)
    local value = self:parseStringFromLine(line)
    if value then
        self.customEntityClasses = value
        LootBeacon.Logger:info("Custom entity classes set to: %s", value)
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid custom entity classes, using default: %s", self.customEntityClasses)
    end
end

function LootBeacon.Config:setCustomEntityParticleEffectPath(line)
    local value = self:parseStringFromLine(line)
    if value and value ~= "" then
        self.customEntityParticleEffectPath = value
        LootBeacon.Logger:info("Custom entity particle effect path set to: %s", value)
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid custom entity particle effect path, using default: %s",
            self.customEntityParticleEffectPath)
    end
end

function LootBeacon.Config:setHighlightDuration(line)
    local value = self:parseNumberFromLine(line)
    if value and value > 0 then
        self.highlightDuration = value
        LootBeacon.Logger:info("Highlight duration set to: %g seconds", value)
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid highlight duration, using default: %g", self.highlightDuration)
    end
end

function LootBeacon.Config:setShowMessage(line)
    local value = self:parseNumberFromLine(line)
    if value == 0 or value == 1 then
        self.showMessage = (value == 1)
        LootBeacon.Logger:info("Show message set to: %s", self.showMessage and "ON" or "OFF")
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid show message value (should be 0 or 1), using default: %s",
            self.showMessage and "1" or "0")
    end
end

function LootBeacon.Config:setLogLevel(line)
    local value = self:parseNumberFromLine(line)
    if value and value >= 1 and value <= 4 then
        self.logLevel = value

        local levelNames = {
            [1] = "Debug",
            [2] = "Info",
            [3] = "Warning",
            [4] = "Error"
        }

        LootBeacon.Logger:info("Log level set to: %s", levelNames[value])
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid log level (should be 1-4), using default: %d", self.logLevel)
    end
end

function LootBeacon.Config:setHighlightItems(line)
    local value = self:parseNumberFromLine(line)
    if value == 0 or value == 1 then
        self.highlightItems = (value == 1)
        LootBeacon.Logger:info("Highlight items set to: %s", self.highlightItems and "ON" or "OFF")
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid highlight items value (should be 0 or 1), using default: %s",
            self.highlightItems and "1" or "0")
    end
end

function LootBeacon.Config:setHighlightCorpses(line)
    local value = self:parseNumberFromLine(line)
    if value == 0 or value == 1 then
        self.highlightCorpses = (value == 1)
        LootBeacon.Logger:info("Highlight corpses set to: %s", self.highlightCorpses and "ON" or "OFF")
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid highlight corpses value (should be 0 or 1), using default: %s",
            self.highlightCorpses and "1" or "0")
    end
end

function LootBeacon.Config:setHighlightAnimals(line)
    local value = self:parseNumberFromLine(line)
    if value == 0 or value == 1 then
        self.highlightAnimals = (value == 1)
        LootBeacon.Logger:info("Highlight animals set to: %s", self.highlightAnimals and "ON" or "OFF")
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid highlight animals value (should be 0 or 1), using default: %s",
            self.highlightAnimals and "1" or "0")
    end
end

function LootBeacon.Config:setGoodCitizenMode(line)
    local value = self:parseNumberFromLine(line)
    if value == 0 or value == 1 then
        self.goodCitizenMode = (value == 1)
        LootBeacon.Logger:info("Good Citizen Mode set to: %s", self.goodCitizenMode and "ON" or "OFF")
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid Good Citizen Mode value (should be 0 or 1), using default: %s",
            self.goodCitizenMode and "1" or "0")
    end
end

function LootBeacon.Config:setTreatUnconsciousAsDead(line)
    local value = self:parseNumberFromLine(line)
    if value == 0 or value == 1 then
        self.treatUnconsciousAsDead = (value == 1)
        LootBeacon.Logger:info("Treat Unconscious as Dead set to: %s", self.treatUnconsciousAsDead and "ON" or "OFF")
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid Treat Unconscious as Dead value (should be 0 or 1), using default: %s",
            self.treatUnconsciousAsDead and "1" or "0")
    end
end

function LootBeacon.Config:setKeyBinding(line)
    local key = self:parseStringFromLine(line)
    if key and key ~= "" then
        self.keyBinding = key
        LootBeacon.Logger:info("Key binding set to: %s", key)
        -- Re-apply key binding
        self:applyKeyBinding()
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid key binding, using default: %s", self.keyBinding)
    end
end

function LootBeacon.Config:setIllegalHighlightKeyBinding(line)
    local key = self:parseStringFromLine(line)
    if key then
        if key == "" or key:lower() == "none" then
            -- User wants to disable the feature
            self.illegalHighlightKeyBinding = "none"
            LootBeacon.Logger:info("Illegal highlight key binding disabled")
        else
            -- User wants to enable the feature with a specific key
            self.illegalHighlightKeyBinding = key
            LootBeacon.Logger:info("Illegal highlight key binding set to: %s", key)
        end

        -- Re-apply key binding
        self:applyIllegalHighlightKeyBinding()
        self.configLoaded = true
    else
        LootBeacon.Logger:warning("Invalid illegal highlight key binding, using default: %s",
            self.illegalHighlightKeyBinding)
    end
end

return LootBeacon.Config
