--[[
Logger module for Loot Beacon mod
Handles all logging with different levels and formatting
]]

LootBeacon.Logger = {
    -- Log levels
    LOG_LEVEL_DEBUG = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_WARNING = 3,
    LOG_LEVEL_ERROR = 4,

    -- Default settings
    prefix = "[Loot Beacon]",
}

function LootBeacon.Logger:initialize(modName)
    self.prefix = "[" .. (modName or "Loot Beacon") .. "]"
    return self
end

function LootBeacon.Logger:debug(message, ...)
    self:log(self.LOG_LEVEL_DEBUG, message, ...)
end

function LootBeacon.Logger:info(message, ...)
    self:log(self.LOG_LEVEL_INFO, message, ...)
end

function LootBeacon.Logger:warning(message, ...)
    self:log(self.LOG_LEVEL_WARNING, message, ...)
end

function LootBeacon.Logger:error(message, ...)
    self:log(self.LOG_LEVEL_ERROR, message, ...)
end

function LootBeacon.Logger:log(level, message, ...)
    -- Only log if the message's level is >= the configured log level
    if level < LootBeacon.Config.logLevel then
        return
    end

    -- Determine level prefix and color
    local levelPrefix, color
    if level == self.LOG_LEVEL_DEBUG then
        levelPrefix = "[DEBUG]"
        color = "$9" -- Blue
    elseif level == self.LOG_LEVEL_INFO then
        levelPrefix = "[INFO]"
        color = "$5" -- White
    elseif level == self.LOG_LEVEL_WARNING then
        levelPrefix = "[WARNING]"
        color = "$6" -- Yellow
    else
        levelPrefix = "[ERROR]"
        color = "$4" -- Red
    end

    -- Format the message with optional parameters
    local formattedMessage = message
    if ... then
        local success, result = pcall(string.format, message, ...)
        if success then
            formattedMessage = result
        end
    end

    -- Construct the full log message
    local fullMessage = string.format("%s%s %s %s",
        color,
        self.prefix,
        levelPrefix,
        tostring(formattedMessage))

    System.LogAlways(fullMessage)
end

return LootBeacon.Logger
