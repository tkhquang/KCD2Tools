--[[
UI Manager module for Loot Beacon mod
Handles UI notifications and messages
]]

LootBeacon.UIManager = {
    -- Text localization keys
    TEXT_KEYS = {
        PREFIX = "@ui_loot_beacon_prefix",
        ITEM_SUFFIX = "@ui_loot_beacon_found_item_suffix",
        ANIMAL_SUFFIX = "@ui_loot_beacon_found_animal_suffix",
        CORPSE_SUFFIX = "@ui_loot_beacon_found_corpse_suffix",
        NOT_FOUND = "@ui_loot_beacon_not_found",
        NOT_FOUND_ITEMS = "@ui_loot_beacon_not_found_items",
        NOT_FOUND_ANIMALS = "@ui_loot_beacon_not_found_animals",
        NOT_FOUND_CORPSES = "@ui_loot_beacon_not_found_corpses"
    }
}

function LootBeacon.UIManager:initialize()
    LootBeacon.Logger:debug("Initializing UI Manager")
    return self
end

function LootBeacon.UIManager:showHighlightResults(counts)
    -- Skip if notifications are disabled
    if not LootBeacon.Config.showMessage then
        LootBeacon.Logger:debug("UI notifications disabled in config")
        return
    end

    -- If nothing was found, show appropriate message
    if counts.total == 0 then
        self:showNothingFoundMessage()
        return
    end

    -- For multiple entity types, show separate notifications with a delay
    local hasMultipleTypes = false
    local typeCount = 0

    if counts.items > 0 then typeCount = typeCount + 1 end
    if counts.corpses > 0 then typeCount = typeCount + 1 end
    if counts.animals > 0 then typeCount = typeCount + 1 end
    if counts.custom > 0 then typeCount = typeCount + 1 end

    hasMultipleTypes = (typeCount > 1)

    -- Show notifications
    if hasMultipleTypes then
        self:showMultipleTypeNotifications(counts)
    else
        self:showSingleTypeNotification(counts)
    end
end

function LootBeacon.UIManager:showNothingFoundMessage()
    -- Determine the most specific "not found" message based on what's enabled
    local message

    if LootBeacon.Config.highlightItems and not LootBeacon.Config.highlightCorpses and not LootBeacon.Config.highlightAnimals then
        message = self.TEXT_KEYS.PREFIX .. " " .. self.TEXT_KEYS.NOT_FOUND_ITEMS
    elseif not LootBeacon.Config.highlightItems and LootBeacon.Config.highlightCorpses and not LootBeacon.Config.highlightAnimals then
        message = self.TEXT_KEYS.PREFIX .. " " .. self.TEXT_KEYS.NOT_FOUND_CORPSES
    elseif not LootBeacon.Config.highlightItems and not LootBeacon.Config.highlightCorpses and LootBeacon.Config.highlightAnimals then
        message = self.TEXT_KEYS.PREFIX .. " " .. self.TEXT_KEYS.NOT_FOUND_ANIMALS
    else
        -- Generic message for multiple types enabled
        message = self.TEXT_KEYS.PREFIX .. " " .. self.TEXT_KEYS.NOT_FOUND
    end

    self:showNotification(message)
end

function LootBeacon.UIManager:showSingleTypeNotification(counts)
    local message = self.TEXT_KEYS.PREFIX .. " "

    if counts.items > 0 then
        message = message .. counts.items .. " " .. self.TEXT_KEYS.ITEM_SUFFIX
    elseif counts.corpses > 0 then
        message = message .. counts.corpses .. " " .. self.TEXT_KEYS.CORPSE_SUFFIX
    elseif counts.animals > 0 then
        message = message .. counts.animals .. " " .. self.TEXT_KEYS.ANIMAL_SUFFIX
    else
        -- This should handle custom entities
        message = message .. counts.custom .. " " .. self.TEXT_KEYS.ITEM_SUFFIX
    end

    self:showNotification(message)
end

function LootBeacon.UIManager:showMultipleTypeNotifications(counts)
    -- Show items notification
    if counts.items > 0 then
        local message = self.TEXT_KEYS.PREFIX .. " " .. counts.items .. " " .. self.TEXT_KEYS.ITEM_SUFFIX
        self:showNotification(message)
    end

    -- Show corpses notification
    if counts.corpses > 0 then
        local message = self.TEXT_KEYS.PREFIX .. " " .. counts.corpses .. " " .. self.TEXT_KEYS.CORPSE_SUFFIX
        self:showNotification(message)
    end

    -- Show animals notification
    if counts.animals > 0 then
        local message = self.TEXT_KEYS.PREFIX .. " " .. counts.animals .. " " .. self.TEXT_KEYS.ANIMAL_SUFFIX
        self:showNotification(message)
    end

    -- Show custom entities notification (grouped with items)
    if counts.custom > 0 and counts.items == 0 then
        local message = self.TEXT_KEYS.PREFIX .. " " .. counts.custom .. " " .. self.TEXT_KEYS.ITEM_SUFFIX
        self:showNotification(message)
    end
end

function LootBeacon.UIManager:showNotification(message)
    LootBeacon.Logger:debug("Showing notification: %s", message)
    Game.ShowNotification(message)
end

function LootBeacon.UIManager:showNotificationWithDelay(message, delay)
    LootBeacon.Logger:debug("Scheduling notification with %dms delay: %s", delay, message)

    Script.SetTimer(delay, function()
        Game.ShowNotification(message)
    end)
end

return LootBeacon.UIManager
