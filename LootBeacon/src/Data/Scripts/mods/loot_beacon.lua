--[[
Loot Beacon - Never Miss a Drop (Highlight Pickable Items)
Version: 1.0.0
Author: tkhquang
]]

-- Define the LootBeacon table with mod properties
LootBeacon = {}
LootBeacon.modname = "Loot Beacon"
LootBeacon.version = "1.0.0"

-- Configuration values with g_ prefix as per KCD coding standards
LootBeacon.g_detectionRadius = 15.0
LootBeacon.g_particleEffectPath = "loot_beacon.pillar_red"
LootBeacon.g_highlightDuration = 5.0
LootBeacon.g_showMessage = true
LootBeacon.g_mod_config_loaded = false
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

-- Helper function to extract numeric value from command line
-- @param line string: The command line to parse
-- @return number: The extracted numeric value, or nil if invalid
function LootBeacon:getNumberValueFromLine(line)
    if not line then return nil end

    -- Try to extract a number from the line
    local value = tonumber(line)
    if value ~= nil then
        return value
    end

    -- If no direct number found, try to extract from a string pattern
    local pattern = "%s*([%d%.]+)"
    local match = string.match(line, pattern)
    if match then
        return tonumber(match)
    end

    return nil
end

-- Helper function to extract string value from command line
-- @param line string: The command line to parse
-- @return string: The extracted string value, or nil if invalid
function LootBeacon:getStringValueFromLine(line)
    if not line then return nil end

    -- Try to extract a quoted string
    local pattern = '%s*"([^"]*)"'
    local match = string.match(line, pattern)
    if match then
        return match
    end

    -- If no quoted string, try to get the rest of the line
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

-- Handler for setting particle effect path from config
-- @param line string: Command line containing the effect path
function LootBeacon:set_particle_effect_path(line)
    self:logDebug("set_particle_effect_path() line: " .. tostring(line))

    local value = self:getStringValueFromLine(line)
    if value and value ~= "" then
        self.g_particleEffectPath = value
        self:logInfo("Particle effect path set to: " .. value)
        self.g_mod_config_loaded = true
    else
        self:logWarning("Invalid particle effect path, using default: " .. self.g_particleEffectPath)
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
    self:log(message, self.LOG_LEVEL_WARNING, "$6")  -- $6 for yellow color
end

-- Logs an error message
-- @param message string: The message to log
function LootBeacon:logError(message)
    self:log(message, self.LOG_LEVEL_ERROR, "$4")  -- $4 for red color
end

-- Logs a message with a specified level and optional color
-- @param message string: The message to log
-- @param level number: The log level (1: Debug, 2: Info, 3: Warning, 4: Error)
-- @param color string: Optional color code for the console (e.g., "$5" for default)
function LootBeacon:log(message, level, color)
    -- Only log if the message's level is >= the configured log level
    if not level then level = self.LOG_LEVEL_INFO end
    if level < self.g_logLevel then return end

    color = color or "$5"  -- Default to white color

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

-- Finds nearby pickable items within the detection radius
-- @return table: A list of pickable item entities
function LootBeacon:findPickableItems()
    -- Get player position with error handling
    if not player then
        self:logError("Player object is nil")
        return {}
    end

    local playerPos = player:GetWorldPos()
    if not playerPos then
        self:logError("Failed to get player position")
        return {}
    end

    -- Get nearby items
    local nearbyItems
    local success, err = pcall(function()
        nearbyItems = System.GetEntitiesInSphereByClass(playerPos, self.g_detectionRadius, "PickableItem") or {}
    end)

    if not success then
        self:logError("Failed to get entities in sphere: " .. tostring(err))
        return {}
    end

    -- Filter pickable items
    local filteredItems = {}
    self:logDebug("Found " .. #nearbyItems .. " potential items within " .. self.g_detectionRadius .. "m")

    for _, pickableItem in pairs(nearbyItems) do
        if self:isItemPickable(pickableItem) then
            table.insert(filteredItems, pickableItem)
            self:logDebug("Added pickable item to highlight list")
        end
    end

    self:logInfo("Found " .. #filteredItems .. " pickable items to highlight")
    return filteredItems
end

-- Removes all active highlights from items
function LootBeacon:removeHighlights()
    self:logInfo("Removing highlights from " .. #self.particleEntities .. " items")

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

-- Activates highlights on nearby pickable items
function LootBeacon:activateHighlights()
    self:logInfo("Activating highlights (radius: " .. self.g_detectionRadius .. "m, duration: " .. self.g_highlightDuration .. "s)")

    -- Kill existing timer if active
    if self.timerID then
        Script.KillTimer(self.timerID)
        self.timerID = nil
    end

    -- Remove existing highlights
    self:removeHighlights()

    -- Set active state and find items
    self.isHighlightActive = true
    local items = self:findPickableItems()

    -- Apply particle effects to found items
    for _, entity in pairs(items) do
        -- Safe LoadParticleEffect call with error handling
        local slot = -1
        local success, result = pcall(function()
            return entity:LoadParticleEffect(-1, self.g_particleEffectPath, {})
        end)

        if success and result and result >= 0 then
            slot = result
            table.insert(self.particleEntities, {entity = entity, slot = slot})

            -- Optional: Adjust orientation to counter entity's rotation
            if entity.GetWorldAngles then
                local angSuccess, ang = pcall(function() return entity:GetWorldAngles() end)
                if angSuccess and ang then
                    pcall(function()
                        entity:SetSlotAngles(slot, { x = -ang.x, y = -ang.y, z = -ang.z })
                    end)
                end
            end
        else
            self:logWarning("Failed to load particle effect for entity: " .. tostring(result or "unknown error"))
        end
    end

    -- Show notification to player
    local numItems = #self.particleEntities
    if self.g_showMessage then
        if numItems > 0 then
            Game.ShowNotification("@ui_loot_beacon_prefix " .. "@ui_loot_beacon_found_1 " .. numItems .. " @ui_loot_beacon_found_2")
        else
            Game.ShowNotification("@ui_loot_beacon_prefix " .. "@ui_loot_beacon_not_found")
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

    -- Execute the mod config file
    self:logInfo("Executing mod.cfg file")
    System.ExecuteCommand("exec Mods/LootBeacon/mod.cfg")

    -- If config wasn't properly loaded, set defaults
    if not self.g_mod_config_loaded then
        System.ExecuteCommand("bind f4 loot_beacon_activate")
        self:logWarning("Config file not loaded or incomplete, using default settings")
        -- Default values are already set in variable initialization
    end

    self:logInfo("Configuration loaded successfully")
    self:logInfo("Detection radius: " .. self.g_detectionRadius .. "m")
    self:logInfo("Particle effect: " .. self.g_particleEffectPath)
    self:logInfo("Highlight duration: " .. self.g_highlightDuration .. "s")
    self:logInfo("Show messages: " .. (self.g_showMessage and "Yes" or "No"))
end

-- Initializes the mod
function LootBeacon:onInit()
    self:logInfo("--------------------- OnInit() -----------------------")
    self:logInfo("Initializing " .. self.modname .. " version " .. self.version)

    -- Register all console commands for configuration
    System.AddCCommand("loot_beacon_activate", "LootBeacon:activateHighlights()", "Activate item highlighting")
    System.AddCCommand("loot_beacon_set_detection_radius", "LootBeacon:set_detection_radius(%line)", "Set detection radius in meters")
    System.AddCCommand("loot_beacon_set_particle_effect_path", "LootBeacon:set_particle_effect_path(%line)", "Set particle effect path")
    System.AddCCommand("loot_beacon_set_highlight_duration", "LootBeacon:set_highlight_duration(%line)", "Set highlight duration in seconds")
    System.AddCCommand("loot_beacon_set_show_message", "LootBeacon:set_show_message(%line)", "Set show message flag (0=off, 1=on)")
    System.AddCCommand("loot_beacon_set_log_level", "LootBeacon:set_log_level(%line)", "Set log level (1=Debug, 2=Info, 3=Warning, 4=Error)")

    -- Load configuration
    self:loadModConfig()

    self:logInfo("Mod initialized successfully")
    self:logInfo("------------------------------------------------------")
end

-- Function called when the game system has started
function LootBeacon:onSystemStarted()
    self:logInfo("---------------- onSystemStarted() ----------------- ")
    -- Additional initialization that requires the game system to be started
    self:logInfo("--------------------------------------------------------")
end

-- Function called when the game is paused
function LootBeacon:onGamePause()
    self:logInfo("---------------- onGamePause() ----------------")
    -- Remove highlights when game is paused for safety
    if self.isHighlightActive then
        self:logInfo("Game paused - removing active highlights for safety")
        if self.timerID then
            Script.KillTimer(self.timerID)
            self.timerID = nil
        end
        self:removeHighlights()
    end
end

-- Function called when the game is resumed
function LootBeacon:onGameResume()
    self:logInfo("---------------- onGameResume() ----------------")
    -- Game has resumed, nothing specific to do here
end

-- Initialize the mod
LootBeacon:onInit()

-- Register event listeners (following KCD mod patterns)
UIAction.RegisterEventSystemListener(LootBeacon, "System", "OnSystemStarted", "onSystemStarted")
UIAction.RegisterEventSystemListener(LootBeacon, "System", "OnGamePause", "onGamePause")
UIAction.RegisterEventSystemListener(LootBeacon, "System", "OnGameResume", "onGameResume")
