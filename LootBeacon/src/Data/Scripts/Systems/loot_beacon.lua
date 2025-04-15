--[[
Loot Beacon - Never Miss a Drop or Corpse
Version: 1.3.0
Author: tkhquang
]]

-- Define the LootBeacon table with mod properties
LootBeacon = {}
LootBeacon.modname = "Loot Beacon"
LootBeacon.version = "1.3.0"

-- Configuration values with g_ prefix as per KCD coding standards
LootBeacon.g_detectionRadius = 15.0
LootBeacon.g_itemParticleEffectPath = "loot_beacon.pillar_red"
LootBeacon.g_humanCorpseParticleEffectPath = "loot_beacon.pillar_green"
LootBeacon.g_animalCorpseParticleEffectPath = "loot_beacon.pillar_blue"
LootBeacon.g_customEntityParticleEffectPath = "loot_beacon.pillar_red"
LootBeacon.g_customEntityClasses = "Nest"
LootBeacon.g_highlightDuration = 5.0
LootBeacon.g_showMessage = true
LootBeacon.g_keyBinding = "f4"
LootBeacon.g_mod_config_loaded = false

-- Configuration for what to highlight
LootBeacon.g_highlightItems = true
LootBeacon.g_highlightCorpses = true
LootBeacon.g_highlightAnimals = true

-- Configuration for other modes
LootBeacon.g_goodCitizenMode = false

-- Default to Info (1: Debug, 2: Info, 3: Warning, 4: Error)
LootBeacon.g_logLevel = 2

-- Internal state variables
LootBeacon.isHighlightActive = false
LootBeacon.timerID = nil
LootBeacon.particleEntities = {}

-- Log Levels Constants
LootBeacon.LOG_LEVEL_DEBUG = 1
LootBeacon.LOG_LEVEL_INFO = 2
LootBeacon.LOG_LEVEL_WARNING = 3
LootBeacon.LOG_LEVEL_ERROR = 4

-- Entity classes
LootBeacon.ENTITY_CLASS_PICKABLE = "PickableItem"

-- Dumps all properties of an object to console for debugging
function dump(obj)
    for key, value in pairs(obj) do
        System.LogAlways(tostring(key) .. ": " .. tostring(value))
        if type(value) == "table" then
            System.LogAlways("  Sub-table " .. tostring(key) .. ":")
            for subKey, subValue in pairs(value) do
                System.LogAlways("    " .. tostring(subKey) .. ": " .. tostring(subValue))
            end
        end
    end
end

-- Helper function to extract numeric value from command line with the format "parameter =value"
-- @param line string: The command line to parse
-- @return number: The extracted numeric value, or nil if invalid
function LootBeacon:getNumberValueFromLine(line)
    if not line then return nil end

    -- First try the specific format with space after equals sign: "parameter =value"
    local equalPattern = "=%s*([%d%.]+)"
    local match = string.match(line, equalPattern)
    if match then
        return tonumber(match)
    end

    -- Fallback - try to extract a direct number from the line
    local value = tonumber(line)
    if value ~= nil then
        return value
    end

    -- Last attempt - try generic pattern
    local pattern = "%s*([%d%.]+)"
    match = string.match(line, pattern)
    if match then
        return tonumber(match)
    end

    return nil
end

-- Helper function to extract string value from command line with the format "parameter =value"
-- @param line string: The command line to parse
-- @return string: The extracted string value, or nil if invalid
function LootBeacon:getStringValueFromLine(line)
    if not line then return nil end

    -- First try the specific format with space after equals sign: "parameter =\"value\""
    local equalPattern = "=%s*\"([^\"]*)\""
    local match = string.match(line, equalPattern)
    if match then
        return match
    end

    -- Try without quotes: "parameter =value"
    equalPattern = "=%s*([%S]+)"
    match = string.match(line, equalPattern)
    if match then
        return match
    end

    -- Fallback - try for quoted string anywhere
    local pattern = '%s*"([^"]*)"'
    match = string.match(line, pattern)
    if match then
        return match
    end

    -- Last attempt - get the rest of the line
    local simplePattern = "%s*(.+)"
    match = string.match(line, simplePattern)
    if match then
        return match
    end

    return nil
end

-- Handler for setting detection radius from config
-- @param line string: Command line containing the radius value
function LootBeacon:set_detection_radius(line)
    self:logDebug("set_detection_radius() line: " .. tostring(line))

    local value = self:getNumberValueFromLine(line)
    if value and value > 0 then
        self.g_detectionRadius = value
        self:logInfo("Detection radius set to: " .. tostring(value) .. " meters")
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid detection radius, using default: " .. self.g_detectionRadius)
    end
end

-- Handler for setting item particle effect path from config
-- @param line string: Command line containing the effect path
function LootBeacon:set_item_particle_effect_path(line)
    self:logDebug("set_item_particle_effect_path() line: " .. tostring(line))

    local value = self:getStringValueFromLine(line)
    if value and value ~= "" then
        self.g_itemParticleEffectPath = value
        self:logInfo("Item particle effect path set to: " .. value)
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid item particle effect path, using default: " .. self.g_itemParticleEffectPath)
    end
end

-- Handler for setting human corpse particle effect path from config
-- @param line string: Command line containing the effect path
function LootBeacon:set_human_corpse_particle_effect_path(line)
    self:logDebug("set_human_corpse_particle_effect_path() line: " .. tostring(line))

    local value = self:getStringValueFromLine(line)
    if value and value ~= "" then
        self.g_humanCorpseParticleEffectPath = value
        self:logInfo("Human corpse particle effect path set to: " .. value)
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid human corpse particle effect path, using default: " ..
            self.g_humanCorpseParticleEffectPath)
    end
end

-- Handler for setting animal corpse particle effect path from config
-- @param line string: Command line containing the effect path
function LootBeacon:set_animal_corpse_particle_effect_path(line)
    self:logDebug("set_animal_corpse_particle_effect_path() line: " .. tostring(line))

    local value = self:getStringValueFromLine(line)
    if value and value ~= "" then
        self.g_animalCorpseParticleEffectPath = value
        self:logInfo("Animal corpse particle effect path set to: " .. value)
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid animal corpse particle effect path, using default: " ..
            self.g_animalCorpseParticleEffectPath)
    end
end

-- Handler for setting custom entity classes
-- @param line string: Command line containing comma-separated entity class names
function LootBeacon:set_custom_entity_classes(line)
    self:logDebug("set_custom_entity_classes() line: " .. tostring(line))

    local value = self:getStringValueFromLine(line)
    if value then
        self.g_customEntityClasses = value
        self:logInfo("Custom entity classes set to: " .. value)
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid custom entity classes, using default: " .. self.g_customEntityClasses)
    end
end

-- Handler for setting custom entity particle effect path
-- @param line string: Command line containing the effect path
function LootBeacon:set_custom_entity_particle_effect_path(line)
    self:logDebug("set_custom_entity_particle_effect_path() line: " .. tostring(line))

    local value = self:getStringValueFromLine(line)
    if value and value ~= "" then
        self.g_customEntityParticleEffectPath = value
        self:logInfo("Custom entity particle effect path set to: " .. value)
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid custom entity particle effect path, using default: " ..
            self.g_customEntityParticleEffectPath)
    end
end

-- Handler for setting highlight duration from config
-- @param line string: Command line containing the duration value
function LootBeacon:set_highlight_duration(line)
    self:logDebug("set_highlight_duration() line: " .. tostring(line))

    local value = self:getNumberValueFromLine(line)
    if value and value > 0 then
        self.g_highlightDuration = value
        self:logInfo("Highlight duration set to: " .. tostring(value) .. " seconds")
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid highlight duration, using default: " .. self.g_highlightDuration)
    end
end

-- Handler for setting show message flag from config
-- @param line string: Command line containing the flag value (0 or 1)
function LootBeacon:set_show_message(line)
    self:logDebug("set_show_message() line: " .. tostring(line))

    local value = self:getNumberValueFromLine(line)
    if value == 0 or value == 1 then
        self.g_showMessage = (value == 1)
        self:logInfo("Show message set to: " .. (self.g_showMessage and "ON" or "OFF"))
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid show message value (should be 0 or 1), using default: " ..
            (self.g_showMessage and "1" or "0"))
    end
end

-- Handler for setting log level from config
-- @param line string: Command line containing the log level value
function LootBeacon:set_log_level(line)
    self:logDebug("set_log_level() line: " .. tostring(line))

    local value = self:getNumberValueFromLine(line)
    if value and value >= 1 and value <= 4 then
        self.g_logLevel = value

        local levelNames = {
            [1] = "Debug",
            [2] = "Info",
            [3] = "Warning",
            [4] = "Error"
        }

        self:logInfo("Log level set to: " .. levelNames[value])
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid log level (should be 1-4), using default: " .. self.g_logLevel)
    end
end

-- Handler for setting highlight items flag
-- @param line string: Command line containing the flag value (0 or 1)
function LootBeacon:set_highlight_items(line)
    self:logDebug("set_highlight_items() line: " .. tostring(line))

    local value = self:getNumberValueFromLine(line)
    if value == 0 or value == 1 then
        self.g_highlightItems = (value == 1)
        self:logInfo("Highlight items set to: " .. (self.g_highlightItems and "ON" or "OFF"))
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid highlight items value (should be 0 or 1), using default: " ..
            (self.g_highlightItems and "1" or "0"))
    end
end

-- Handler for setting highlight corpses flag
-- @param line string: Command line containing the flag value (0 or 1)
function LootBeacon:set_highlight_corpses(line)
    self:logDebug("set_highlight_corpses() line: " .. tostring(line))

    local value = self:getNumberValueFromLine(line)
    if value == 0 or value == 1 then
        self.g_highlightCorpses = (value == 1)
        self:logInfo("Highlight corpses set to: " .. (self.g_highlightCorpses and "ON" or "OFF"))
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid highlight corpses value (should be 0 or 1), using default: " ..
            (self.g_highlightCorpses and "1" or "0"))
    end
end

-- Handler for setting highlight animals flag
-- @param line string: Command line containing the flag value (0 or 1)
function LootBeacon:set_highlight_animals(line)
    self:logDebug("set_highlight_animals() line: " .. tostring(line))

    local value = self:getNumberValueFromLine(line)
    if value == 0 or value == 1 then
        self.g_highlightAnimals = (value == 1)
        self:logInfo("Highlight animals set to: " .. (self.g_highlightAnimals and "ON" or "OFF"))
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid highlight animals value (should be 0 or 1), using default: " ..
            (self.g_highlightAnimals and "1" or "0"))
    end
end

-- Handler for setting good citizen mode flag
-- @param line string: Command line containing the flag value (0 or 1)
function LootBeacon:set_good_citizen_mode(line)
    self:logDebug("set_good_citizen_mode() line: " .. tostring(line))

    local value = self:getNumberValueFromLine(line)
    if value == 0 or value == 1 then
        self.g_goodCitizenMode = (value == 1)
        self:logInfo("Good Citizen Mode set to: " .. (self.g_goodCitizenMode and "ON" or "OFF"))
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid Good Citizen Mode value (should be 0 or 1), using default: " ..
            (self.g_goodCitizenMode and "1" or "0"))
    end
end

-- Handler for setting key binding from config
-- @param line string: Command line containing the key name
function LootBeacon:set_key_binding(line)
    self:logDebug("set_key_binding() line: " .. tostring(line))

    local key = self:getStringValueFromLine(line)
    if key and key ~= "" then
        -- Store the configured key
        self.g_keyBinding = key
        self:logInfo("Key binding set to: " .. key)

        -- Apply the binding
        System.ExecuteCommand("bind " .. key .. " loot_beacon_activate")
        self:logDebug("Applied console command: bind " .. key .. " loot_beacon_activate")

        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid key binding, using default: f4")
        self.g_keyBinding = "f4"
        System.ExecuteCommand("bind f4 loot_beacon_activate")
    end
end

-- Loads a file safely with error handling
-- @param file string: The file path to load
function LootBeacon:loadFile(file)
    if not file then
        self:logError("Attempted to load nil file")
        return false
    end

    self:logDebug("Loading file [" .. tostring(file) .. "] ...")
    local success, err = pcall(function() Script.ReloadScript(file) end)

    if not success then
        self:logError("Failed to load file [" .. tostring(file) .. "]: " .. tostring(err))
        return false
    end

    return true
end

-- Logs a debug message
-- @param message string: The message to log
function LootBeacon:logDebug(message)
    self:log(message, self.LOG_LEVEL_DEBUG)
end

-- Logs an info message
-- @param message string: The message to log
function LootBeacon:logInfo(message)
    self:log(message, self.LOG_LEVEL_INFO)
end

-- Logs a warning message
-- @param message string: The message to log
function LootBeacon:logWarning(message)
    self:log(message, self.LOG_LEVEL_WARNING, "$6") -- $6 for yellow color
end

-- Logs an error message
-- @param message string: The message to log
function LootBeacon:logError(message)
    self:log(message, self.LOG_LEVEL_ERROR, "$4") -- $4 for red color
end

-- Logs a message with a specified level and optional color
-- @param message string: The message to log
-- @param level number: The log level (1: Debug, 2: Info, 3: Warning, 4: Error)
-- @param color string: Optional color code for the console (e.g., "$5" for default)
function LootBeacon:log(message, level, color)
    -- Only log if the message's level is >= the configured log level
    if not level then level = self.LOG_LEVEL_INFO end
    if level < self.g_logLevel then return end

    color = color or "$5" -- Default to white color

    local levelStr
    if level == self.LOG_LEVEL_DEBUG then
        levelStr = "[DEBUG]"
    elseif level == self.LOG_LEVEL_INFO then
        levelStr = "[INFO]"
    elseif level == self.LOG_LEVEL_WARNING then
        levelStr = "[WARNING]"
    else
        levelStr = "[ERROR]"
    end

    -- Safe stringify for message
    local messageStr = tostring(message or "nil")

    System.LogAlways(color .. "[" .. self.modname .. "] " .. levelStr .. " " .. messageStr)
end

-- Determines if an item is pickable by the player
-- @param pickableItem entity: The item entity to check
-- @return boolean: True if the item is pickable, false otherwise
function LootBeacon:isItemPickable(pickableItem)
    if not pickableItem or not pickableItem.item then
        self:logWarning("Invalid pickable item passed to isItemPickable")
        return false
    end

    -- Safe access to item ID
    local itemId = pickableItem.item:GetId()
    if not itemId then
        self:logDebug("Item has no ID, skipping")
        return false
    end

    local itemEntity = ItemManager.GetItem(itemId)
    if not itemEntity then
        self:logDebug("Failed to get item entity for ID: " .. tostring(itemId))
        return false
    end

    local itemName = ItemManager.GetItemName(itemEntity.class) or "unknown"
    local itemUIName = pickableItem.item:GetUIName() or ""

    self:logDebug("Checking item: " .. itemName .. ", UI name: " .. itemUIName)

    -- Check if the player can pick up the item
    if not player or not player.id then
        self:logWarning("Player reference is invalid")
        return false
    end

    if pickableItem.item:CanPickUp(player.id) == false then
        self:logDebug("Item " .. itemName .. " is not pickupable by player, skipping")
        return false
    end

    if pickableItem.item:CanSteal(player.id) and self.g_goodCitizenMode then
        self:logDebug("Item " .. itemName .. " is illegal to be picked up by player, skipping")
        return false
    end

    -- Skip items with empty UI names (NPC only items)
    if itemUIName == "" then
        self:logDebug("Item " .. itemName .. " is NPC only, skipping")
        return false
    end

    -- Skip items that are being used
    if pickableItem.item:IsUsed() == true then
        self:logDebug("Item " .. itemName .. " is being used, skipping")
        return false
    end

    -- Additional owner checks
    local ownerHandle = ItemManager.GetItemOwner(itemEntity.id)
    local ownerEntity = ownerHandle and System.GetEntity(ownerHandle) or nil

    self:logDebug("Item owner check - Handle: " .. tostring(ownerHandle) ..
        ", Entity: " .. tostring(ownerEntity) ..
        ", Player ID: " .. tostring(player.this and player.this.id or "nil"))

    return true
end

-- Gets an entity's name through various methods
-- @param entity table|nil: The entity to get the name from
-- @return string|nil: The entity's name or nil if not available
function LootBeacon:getEntityName(entity)
    if not entity then
        return nil
    end

    if (type(entity) == "userdata") then
        entity = System.GetEntity(entity)
    end

    local entityName = nil

    if not entityName and entity["soul"] then
        entityName = entity.soul:GetNameStringId()
    end

    if not entityName and entity["uiname"] then
        entityName = entity.uiname
    end

    if not entityName then
        entityName = entity:GetName()
    end

    return entityName
end

-- Helper function to check if a class is in the custom entity classes list
-- @param className string: The class name to check
-- @return boolean: True if the class is in the custom entity classes list
function LootBeacon:isCustomEntityClass(className)
    if not className or not self.g_customEntityClasses or self.g_customEntityClasses == "" then
        return false
    end

    -- Split the comma-separated string into a table
    local classNames = {}
    for class in string.gmatch(self.g_customEntityClasses, "([^,]+)") do
        -- Trim whitespace
        class = string.match(class, "^%s*(.-)%s*$")
        if class and class ~= "" then
            table.insert(classNames, class)
        end
    end

    -- Check if the className is in the list
    for _, class in ipairs(classNames) do
        if className == class then
            return true
        end
    end

    return false
end

-- Removes all active highlights from entities
function LootBeacon:removeHighlights()
    self:logInfo("Removing highlights from " .. #self.particleEntities .. " entities")

    for _, data in pairs(self.particleEntities) do
        local entity = data.entity
        local slot = data.slot

        if entity and slot then
            -- Safe FreeSlot call with error handling
            local success, err = pcall(function()
                entity:FreeSlot(slot)
            end)

            if not success then
                self:logWarning("Failed to free slot for entity: " .. tostring(err))
            end
        end
    end

    self.particleEntities = {}
    self.isHighlightActive = false
    self.timerID = nil
end

-- Applies a particle effect to an entity
-- @param entity: The entity to highlight
-- @param effectPath string: The particle effect path to use
-- @return boolean: True if successful, false otherwise
function LootBeacon:applyHighlightEffect(entity, effectPath)
    if not entity or not effectPath then
        self:logWarning("Invalid entity or effect path in applyHighlightEffect")
        return false
    end

    -- Apply particle effect to the entity
    local slot = -1
    local success, result = pcall(function()
        return entity:LoadParticleEffect(-1, effectPath, {})
    end)

    if success and result and result >= 0 then
        slot = result
        table.insert(self.particleEntities, { entity = entity, slot = slot })

        -- Adjust orientation with randomized angles for visual variety
        if entity.GetWorldAngles then
            local angSuccess, ang = pcall(function() return entity:GetWorldAngles() end)
            if angSuccess and ang then
                pcall(function()
                    -- Generate a random angle between 30 and 90 degrees for upward direction
                    local randomX = math.random(30, 90)

                    -- Counter the entity's rotation and add randomized upward angle
                    local adjustedAngles = {
                        x = -ang.x + randomX,
                        y = -ang.y,
                        z = -ang.z
                    }

                    -- If the original angles are all near zero, use a randomized orientation
                    if math.abs(ang.x) < 0.1 and math.abs(ang.y) < 0.1 and math.abs(ang.z) < 0.1 then
                        adjustedAngles = { x = randomX, y = 0, z = 0 }
                    end

                    entity:SetSlotAngles(slot, adjustedAngles)
                end)
            else
                -- If we can't get the angles, set a randomized upward orientation
                pcall(function()
                    local randomX = math.random(30, 90)
                    entity:SetSlotAngles(slot, { x = randomX, y = 0, z = 0 })
                end)
            end
        end
        return true
    else
        self:logWarning("Failed to load particle effect for entity: " .. tostring(result or "unknown error"))
        return false
    end
end

-- Activates highlights on nearby entities based on configuration
function LootBeacon:activateHighlights()
    self:logInfo("Activating highlights (radius: " ..
        self.g_detectionRadius .. "m, duration: " .. self.g_highlightDuration .. "s)")

    -- Kill existing timer if active
    if self.timerID then
        Script.KillTimer(self.timerID)
        self.timerID = nil
    end

    -- Remove existing highlights
    self:removeHighlights()

    -- Set active state
    self.isHighlightActive = true

    -- Track counts for UI message
    local totalHighlighted = 0
    local itemsCount = 0
    local corpsesCount = 0
    local animalsCount = 0

    -- Get all entities in sphere around player
    local playerPos = player:GetPos()
    local allEntities = System.GetEntitiesInSphere(playerPos, self.g_detectionRadius)

    -- Process each entity
    for _, entity in pairs(allEntities) do
        self:logDebug("=============================")
        self:logDebug("Entity: " .. tostring(entity))
        self:logDebug("Entity Name: " .. tostring(self:getEntityName(entity)))
        self:logDebug("Entity Class: " .. tostring(entity.class))
        -- Only process non-hidden entities
        if entity and entity:IsHidden() ~= true then
            -- Check if entity is an actor (NPC or Animal)
            if entity["actor"] then
                self:logDebug("Actor IsDead: " .. tostring(entity.actor:IsDead()))
                self:logDebug("Actor Name: " .. tostring(self:getEntityName(entity)))

                -- Only process dead actors
                if entity.actor:IsDead() then
                    -- Is Human
                    if entity["human"] and self.g_highlightCorpses then
                        if self:applyHighlightEffect(entity, self.g_humanCorpseParticleEffectPath) then
                            corpsesCount = corpsesCount + 1
                            totalHighlighted = totalHighlighted + 1
                        end
                        -- Is Animal (not human and is dead)
                    elseif self.g_highlightAnimals then
                        if self:applyHighlightEffect(entity, self.g_animalCorpseParticleEffectPath) then
                            animalsCount = animalsCount + 1
                            totalHighlighted = totalHighlighted + 1
                        end
                    end
                end
                -- Check for pickable items
            elseif entity.class == self.ENTITY_CLASS_PICKABLE and self.g_highlightItems then
                if self:isItemPickable(entity) then
                    if self:applyHighlightEffect(entity, self.g_itemParticleEffectPath) then
                        itemsCount = itemsCount + 1
                        totalHighlighted = totalHighlighted + 1
                    end
                end
                -- Check for custom entity classes
            elseif self.g_customEntityClasses ~= "" and self:isCustomEntityClass(entity.class) then
                if self:applyHighlightEffect(entity, self.g_customEntityParticleEffectPath) then
                    itemsCount = itemsCount + 1 -- Count these as items for the UI message
                    totalHighlighted = totalHighlighted + 1
                end
            else
                -- Other entities
                self:logDebug("IS OTHER ENTITY")
            end
        else
            self:logDebug("Entity is hidden, skipping")
        end
        self:logDebug("=============================")
    end

    self:logInfo("Highlighted " .. itemsCount .. " pickable items")
    self:logInfo("Highlighted " .. corpsesCount .. " human corpses")
    self:logInfo("Highlighted " .. animalsCount .. " animal corpses")

    -- Show notification to player
    if self.g_showMessage then
        if totalHighlighted > 0 then
            -- For a single type of highlight, use a simple message
            if itemsCount > 0 and corpsesCount == 0 and animalsCount == 0 then
                Game.ShowNotification("@ui_loot_beacon_prefix " .. itemsCount .. " @ui_loot_beacon_found_item_suffix")
            elseif itemsCount == 0 and corpsesCount > 0 and animalsCount == 0 then
                Game.ShowNotification("@ui_loot_beacon_prefix " .. corpsesCount .. " @ui_loot_beacon_found_corpse_suffix")
            elseif itemsCount == 0 and corpsesCount == 0 and animalsCount > 0 then
                Game.ShowNotification("@ui_loot_beacon_prefix " .. animalsCount .. " @ui_loot_beacon_found_animal_suffix")
                -- For multiple types, show individual counts as separate notifications
            else
                -- Display individual counts as separate notifications
                if itemsCount > 0 then
                    Game.ShowNotification("@ui_loot_beacon_prefix " .. itemsCount .. " @ui_loot_beacon_found_item_suffix")
                end

                if corpsesCount > 0 then
                    Script.SetTimer(1000, function()
                        Game.ShowNotification("@ui_loot_beacon_prefix " ..
                            corpsesCount .. " @ui_loot_beacon_found_corpse_suffix")
                    end)
                end

                if animalsCount > 0 then
                    Script.SetTimer(2000, function()
                        Game.ShowNotification("@ui_loot_beacon_prefix " ..
                            animalsCount .. " @ui_loot_beacon_found_animal_suffix")
                    end)
                end
            end
        else
            -- More specific "not found" messages based on what's enabled
            if self.g_highlightItems and not self.g_highlightCorpses and not self.g_highlightAnimals then
                Game.ShowNotification("@ui_loot_beacon_prefix @ui_loot_beacon_not_found_items")
            elseif not self.g_highlightItems and self.g_highlightCorpses and not self.g_highlightAnimals then
                Game.ShowNotification("@ui_loot_beacon_prefix @ui_loot_beacon_not_found_corpses")
            elseif not self.g_highlightItems and not self.g_highlightCorpses and self.g_highlightAnimals then
                Game.ShowNotification("@ui_loot_beacon_prefix @ui_loot_beacon_not_found_animals")
            else
                -- Generic message for multiple types
                Game.ShowNotification("@ui_loot_beacon_prefix @ui_loot_beacon_not_found")
            end
        end
    end

    -- Set timer to automatically remove highlights
    self.timerID = Script.SetTimer(self.g_highlightDuration * 1000, function()
        LootBeacon:removeHighlights()
    end)
end

-- Loads mod configuration from file
function LootBeacon:loadModConfig()
    self:logInfo("Loading mod configuration")

    -- Track whether any configuration was loaded
    self.g_mod_config_loaded = false

    -- Define potential config file paths to check
    local configPaths = {
        "Mods/LootBeacon/mod.cfg",
        "Mods/loot_beacon/mod.cfg",
        "C:/KCD2Mods/LootBeacon/mod.cfg",
        "D:/KCD2Mods/LootBeacon/mod.cfg",
    }

    -- Try each possible config path
    local configLoaded = false
    for _, path in ipairs(configPaths) do
        self:logDebug("Attempting to load config from: " .. path)

        -- Try to execute the config file
        System.ExecuteCommand("exec " .. path)

        -- Check if any parameters were successfully loaded
        if self.g_mod_config_loaded then
            self:logInfo("Configuration successfully loaded from: " .. path)
            configLoaded = true
            break
        end
    end

    -- If no config was loaded, set up defaults
    if not configLoaded then
        self:logWarning("No configuration file found. Using default settings.")
        -- Ensure F4 is bound to our command
        System.ExecuteCommand("bind " .. self.g_keyBinding .. " loot_beacon_activate")
    end

    -- Log the final configuration
    self:logInfo("Final configuration:")
    self:logInfo("- Detection radius: " .. self.g_detectionRadius .. "m")
    self:logInfo("- Item particle effect: " .. self.g_itemParticleEffectPath)
    self:logInfo("- Human corpse particle effect: " .. self.g_humanCorpseParticleEffectPath)
    self:logInfo("- Animal particle effect: " .. self.g_animalCorpseParticleEffectPath)
    self:logInfo("- Custom entity classes: " .. self.g_customEntityClasses)
    self:logInfo("- Custom entity particle effect: " .. self.g_customEntityParticleEffectPath)
    self:logInfo("- Highlight duration: " .. self.g_highlightDuration .. "s")
    self:logInfo("- Show messages: " .. (self.g_showMessage and "Yes" or "No"))
    self:logInfo("- Highlight items: " .. (self.g_highlightItems and "Yes" or "No"))
    self:logInfo("- Highlight corpses: " .. (self.g_highlightCorpses and "Yes" or "No"))
    self:logInfo("- Highlight animals: " .. (self.g_highlightAnimals and "Yes" or "No"))
    self:logInfo("- Good Citizen Mode: " .. (self.g_goodCitizenMode and "Yes" or "No"))
    self:logInfo("- Key binding: " .. (self.g_keyBinding or "f4"))
end

-- Initializes the mod
function LootBeacon:onInit()
    self:logInfo("--------------------- OnInit() -----------------------")
    self:logInfo("Initializing " .. self.modname .. " version " .. self.version)

    -- Register all console commands for configuration
    System.AddCCommand("loot_beacon_activate", "LootBeacon:activateHighlights()", "Activate entity highlighting")
    System.AddCCommand("loot_beacon_set_detection_radius", "LootBeacon:set_detection_radius(%line)",
        "Set detection radius in meters")
    System.AddCCommand("loot_beacon_set_item_particle_effect_path", "LootBeacon:set_item_particle_effect_path(%line)",
        "Set item particle effect path")
    System.AddCCommand("loot_beacon_set_human_corpse_particle_effect_path",
        "LootBeacon:set_human_corpse_particle_effect_path(%line)", "Set human corpse particle effect path")
    System.AddCCommand("loot_beacon_set_animal_corpse_particle_effect_path",
        "LootBeacon:set_animal_corpse_particle_effect_path(%line)", "Set animal corpse particle effect path")
    System.AddCCommand("loot_beacon_set_custom_entity_classes", "LootBeacon:set_custom_entity_classes(%line)",
        "Set custom entity classes to highlight (comma-separated)")
    System.AddCCommand("loot_beacon_set_custom_entity_particle_effect_path",
        "LootBeacon:set_custom_entity_particle_effect_path(%line)",
        "Set custom entity particle effect path")
    System.AddCCommand("loot_beacon_set_highlight_duration", "LootBeacon:set_highlight_duration(%line)",
        "Set highlight duration in seconds")
    System.AddCCommand("loot_beacon_set_show_message", "LootBeacon:set_show_message(%line)",
        "Set show message flag (0=off, 1=on)")
    System.AddCCommand("loot_beacon_set_log_level", "LootBeacon:set_log_level(%line)",
        "Set log level (1=Debug, 2=Info, 3=Warning, 4=Error)")
    System.AddCCommand("loot_beacon_set_highlight_items", "LootBeacon:set_highlight_items(%line)",
        "Set highlight items flag (0=off, 1=on)")
    System.AddCCommand("loot_beacon_set_highlight_corpses", "LootBeacon:set_highlight_corpses(%line)",
        "Set highlight corpses flag (0=off, 1=on)")
    System.AddCCommand("loot_beacon_set_highlight_animals", "LootBeacon:set_highlight_animals(%line)",
        "Set highlight animals flag (0=off, 1=on)")
    System.AddCCommand("loot_beacon_set_good_citizen_mode", "LootBeacon:set_good_citizen_mode(%line)",
        "Set highlight animals flag (0=off, 1=on)")
    System.AddCCommand("loot_beacon_set_key_binding", "LootBeacon:set_key_binding(%line)",
        "Set key binding for highlight activation")

    -- Load configuration
    self:loadModConfig()

    self:logInfo("Mod initialized successfully")
    self:logInfo("------------------------------------------------------")
end

-- Function called when the game system has started
function LootBeacon:onSystemStarted()
    self:logDebug("---------------- onSystemStarted() ----------------- ")
    -- Additional initialization that requires the game system to be started
end

-- Function called when the game is paused
function LootBeacon:onGamePause()
    self:logDebug("---------------- onGamePause() ----------------")
    -- Remove highlights when game is paused for safety
    if self.isHighlightActive then
        self:logDebug("Game paused - removing active highlights for safety")
        if self.timerID then
            Script.KillTimer(self.timerID)
            self.timerID = nil
        end
        self:removeHighlights()
    end
end

-- Function called when the game is resumed
function LootBeacon:onGameResume()
    self:logDebug("---------------- onGameResume() ----------------")
    -- Game has resumed, nothing specific to do here
end

-- Initialize the mod
LootBeacon:onInit()

-- Register event listeners (following KCD mod patterns)
UIAction.RegisterEventSystemListener(LootBeacon, "System", "OnSystemStarted", "onSystemStarted")
UIAction.RegisterEventSystemListener(LootBeacon, "System", "OnGamePause", "onGamePause")
UIAction.RegisterEventSystemListener(LootBeacon, "System", "OnGameResume", "onGameResume")
