--[[
Command Registry module for Loot Beacon mod
Handles registration of console commands
]]

LootBeacon.CommandRegistry = {}

function LootBeacon.CommandRegistry:initialize()
    LootBeacon.Logger:info("Registering console commands")

    -- Main command to activate highlights
    self:registerCommand("loot_beacon_activate",
        "LootBeacon.Highlighter:activateHighlights()",
        "Activate entity highlighting")

    -- Command to activate illegal item highlights only
    self:registerCommand("loot_beacon_activate_illegal",
        "LootBeacon.Highlighter:activateIllegalHighlights()",
        "Highlight only items that require stealing")

    -- Configuration commands
    self:registerCommand("loot_beacon_set_detection_radius",
        "LootBeacon.Config:setDetectionRadius(%line)",
        "Set detection radius in meters")

    self:registerCommand("loot_beacon_set_item_particle_effect_path",
        "LootBeacon.Config:setItemParticleEffectPath(%line)",
        "Set item particle effect path")

    self:registerCommand("loot_beacon_set_human_corpse_particle_effect_path",
        "LootBeacon.Config:setHumanCorpseParticleEffectPath(%line)",
        "Set human corpse particle effect path")

    self:registerCommand("loot_beacon_set_animal_corpse_particle_effect_path",
        "LootBeacon.Config:setAnimalCorpseParticleEffectPath(%line)",
        "Set animal corpse particle effect path")

    self:registerCommand("loot_beacon_set_custom_entity_classes",
        "LootBeacon.Config:setCustomEntityClasses(%line)",
        "Set custom entity classes to highlight (comma-separated)")

    self:registerCommand("loot_beacon_set_custom_entity_particle_effect_path",
        "LootBeacon.Config:setCustomEntityParticleEffectPath(%line)",
        "Set custom entity particle effect path")

    self:registerCommand("loot_beacon_set_highlight_duration",
        "LootBeacon.Config:setHighlightDuration(%line)",
        "Set highlight duration in seconds")

    self:registerCommand("loot_beacon_set_show_message",
        "LootBeacon.Config:setShowMessage(%line)",
        "Set show message flag (0=off, 1=on)")

    self:registerCommand("loot_beacon_set_log_level",
        "LootBeacon.Config:setLogLevel(%line)",
        "Set log level (1=Debug, 2=Info, 3=Warning, 4=Error)")

    self:registerCommand("loot_beacon_set_highlight_items",
        "LootBeacon.Config:setHighlightItems(%line)",
        "Set highlight items flag (0=off, 1=on)")

    self:registerCommand("loot_beacon_set_highlight_corpses",
        "LootBeacon.Config:setHighlightCorpses(%line)",
        "Set highlight corpses flag (0=off, 1=on)")

    self:registerCommand("loot_beacon_set_highlight_animals",
        "LootBeacon.Config:setHighlightAnimals(%line)",
        "Set highlight animals flag (0=off, 1=on)")

    self:registerCommand("loot_beacon_set_good_citizen_mode",
        "LootBeacon.Config:setGoodCitizenMode(%line)",
        "Set Good Citizen Mode flag (0=off, 1=on)")

    self:registerCommand("loot_beacon_set_treat_unconscious_as_dead",
        "LootBeacon.Config:setTreatUnconsciousAsDead(%line)",
        "Set Treat Unconscious as Dead flag (0=off, 1=on)")

    self:registerCommand("loot_beacon_set_key_binding",
        "LootBeacon.Config:setKeyBinding(%line)",
        "Set key binding for highlight activation")

    self:registerCommand("loot_beacon_set_illegal_highlight_key_binding",
        "LootBeacon.Config:setIllegalHighlightKeyBinding(%line)",
        "Set key binding for illegal item highlight activation")

    -- Utility commands
    self:registerCommand("loot_beacon_remove_highlights",
        "LootBeacon.Highlighter:removeAllHighlights()",
        "Manually remove all active highlights")

    self:registerCommand("loot_beacon_version",
        "LootBeacon.CommandRegistry:showVersion()",
        "Show mod version information")

    self:registerCommand("loot_beacon_help",
        "LootBeacon.CommandRegistry:showHelp()",
        "Show help information for Loot Beacon mod")

    return self
end

function LootBeacon.CommandRegistry:registerCommand(name, action, description)
    local success, err = pcall(function()
        System.AddCCommand(name, action, description)
    end)

    if success then
        LootBeacon.Logger:debug("Registered command: %s", name)
    else
        LootBeacon.Logger:error("Failed to register command %s: %s", name, tostring(err))
    end
end

function LootBeacon.CommandRegistry:showVersion()
    System.LogAlways("$5[Loot Beacon] Version: " .. LootBeacon.Core.VERSION)
    System.LogAlways("$5[Loot Beacon] Type 'loot_beacon_help' for available commands.")
end

function LootBeacon.CommandRegistry:showHelp()
    System.LogAlways("$5[Loot Beacon] Available commands:")
    System.LogAlways("$5  loot_beacon_activate - Highlight nearby lootable objects")
    System.LogAlways("$5  loot_beacon_activate_illegal - Highlight only items that require stealing")
    System.LogAlways("$5  loot_beacon_remove_highlights - Remove all active highlights")
    System.LogAlways("$5  loot_beacon_version - Show mod version")
    System.LogAlways("$5")
    System.LogAlways("$5  Default key binding: " .. LootBeacon.Config.keyBinding)

    if LootBeacon.Config.illegalHighlightKeyBinding ~= "none" then
        System.LogAlways("$5  Illegal highlight key binding: " .. LootBeacon.Config.illegalHighlightKeyBinding)
    else
        System.LogAlways("$5  Illegal highlight key binding: disabled")
    end

    System.LogAlways("$5")
    System.LogAlways("$5  For configuration options, edit mod.cfg or see the mod documentation")
end

return LootBeacon.CommandRegistry
