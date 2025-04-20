--[[
Entity Detector module for Loot Beacon mod
Handles entity scanning, filtering, and classification
]]

LootBeacon.EntityDetector = {
    -- Entity class constants
    ENTITY_CLASS_PICKABLE = "PickableItem",

    -- Result containers
    results = {
        items = {},
        corpses = {},
        animals = {},
        custom = {}
    }
}

function LootBeacon.EntityDetector:initialize()
    LootBeacon.Logger:debug("Initializing Entity Detector")
    return self
end

function LootBeacon.EntityDetector:detectEntities()
    -- Reset previous results
    self:resetResults()

    -- Get player position
    local playerPos = player:GetPos()
    if not playerPos then
        LootBeacon.Logger:error("Failed to get player position")
        return self.results
    end

    -- Get all entities within detection radius
    local radius = LootBeacon.Config.detectionRadius
    local allEntities = System.GetEntitiesInSphere(playerPos, radius)
    LootBeacon.Logger:debug("Found %d total entities within %gm radius", #allEntities, radius)

    -- Process and filter entities
    for _, entity in pairs(allEntities) do
        self:processEntity(entity)
    end

    -- Log counts
    LootBeacon.Logger:info("Detected: %d items, %d corpses, %d animals, %d custom entities",
        #self.results.items,
        #self.results.corpses,
        #self.results.animals,
        #self.results.custom)

    return self.results
end

function LootBeacon.EntityDetector:resetResults()
    self.results = {
        items = {},
        corpses = {},
        animals = {},
        custom = {}
    }
end

function LootBeacon.EntityDetector:processEntity(entity)
    -- Debug separator start
    LootBeacon.Logger:debug("==============1===============")
    LootBeacon.Logger:debug("Entity: %s", tostring(entity))
    LootBeacon.Logger:debug("Entity Name: %s", self:getEntityName(entity))
    LootBeacon.Logger:debug("Entity Class: %s", tostring(entity.class))

    -- Skip invalid or hidden entities
    if not entity or entity:IsHidden() == true then
        LootBeacon.Logger:debug("Entity is hidden, skipping")
    else
        -- First check if it's a custom entity class
        if self:isCustomEntityClass(entity.class) then
            LootBeacon.Logger:debug("Found custom entity class: %s", entity.class)
            table.insert(self.results.custom, entity)
            -- Check if entity is an actor (NPC or Animal)
        elseif entity["actor"] then
            if entity.actor:IsDead() or (LootBeacon.Config.treatUnconsciousAsDead and entity.actor:IsUnconscious()) then
                if LootBeacon.Config.goodCitizenMode and entity["soul"] and not entity.soul:IsLegalToLoot() then
                    LootBeacon.Logger:debug("Good citizens don't rob, skipping")
                else
                    -- First check if it's a human
                    if entity["human"] then
                        LootBeacon.Logger:debug("Entity is a dead human")
                        -- Only add to corpses if human corpse highlighting is enabled
                        if LootBeacon.Config.highlightCorpses then
                            LootBeacon.Logger:debug("Adding to corpses list")
                            table.insert(self.results.corpses, entity)
                        else
                            LootBeacon.Logger:debug("Corpse highlighting disabled, skipping")
                        end
                    else
                        LootBeacon.Logger:debug("Entity is a dead animal")
                        -- It's an animal - only add if animal highlighting is enabled
                        if LootBeacon.Config.highlightAnimals then
                            LootBeacon.Logger:debug("Adding to animals list")
                            table.insert(self.results.animals, entity)
                        else
                            LootBeacon.Logger:debug("Skipping: animal highlighting disabled")
                        end
                    end
                end
            else
                LootBeacon.Logger:debug("Actor is alive, skipping")
            end
            -- Check for pickable items
        elseif entity.class == self.ENTITY_CLASS_PICKABLE and LootBeacon.Config.highlightItems then
            LootBeacon.Logger:debug("Found pickable item")
            if self:isItemPickable(entity) then
                LootBeacon.Logger:debug("Item is pickable, adding to items list")
                table.insert(self.results.items, entity)
            else
                LootBeacon.Logger:debug("Item not pickable, skipping")
            end
        else
            -- If we got here, it's an entity that doesn't match any category
            LootBeacon.Logger:debug("IS OTHER ENTITY")
        end
    end

    -- Debug separator end - only one place
    LootBeacon.Logger:debug("==============1===============")
end

function LootBeacon.EntityDetector:isItemPickable(pickableItem)
    if not pickableItem or not pickableItem.item then
        LootBeacon.Logger:warning("Invalid pickable item passed to isItemPickable")
        return false
    end

    -- Get item ID safely
    local itemId = pickableItem.item:GetId()
    if not itemId then
        LootBeacon.Logger:debug("Item has no ID, skipping")
        return false
    end

    -- Get item entity
    local itemEntity = ItemManager.GetItem(itemId)
    if not itemEntity then
        LootBeacon.Logger:debug("Failed to get item entity for ID: %s", tostring(itemId))
        return false
    end

    -- Get item name for logs
    local itemName = ItemManager.GetItemName(itemEntity.class) or "unknown"
    local itemUIName = pickableItem.item:GetUIName() or ""

    LootBeacon.Logger:debug("Checking item: %s, UI name: %s", itemName, itemUIName)

    -- Check if the player can pick up the item
    if not player or not player.id then
        LootBeacon.Logger:warning("Player reference is invalid")
        return false
    end

    -- Check if item can be picked up
    if pickableItem.item:CanPickUp(player.id) == false then
        LootBeacon.Logger:debug("Item %s is not pickupable by player, skipping", itemName)
        return false
    end

    -- Check for "good citizen mode" - skip items that require stealing
    if pickableItem.item:CanSteal(player.id) and LootBeacon.Config.goodCitizenMode then
        LootBeacon.Logger:debug("Item %s is illegal to be picked up by player, skipping", itemName)
        return false
    end

    -- Skip items with empty UI names (NPC only items)
    if itemUIName == "" then
        LootBeacon.Logger:debug("Item %s is NPC only, skipping", itemName)
        return false
    end

    -- Skip items that are being used
    if pickableItem.item:IsUsed() == true then
        LootBeacon.Logger:debug("Item %s is being used, skipping", itemName)
        return false
    end

    return true
end

function LootBeacon.EntityDetector:getEntityName(entity)
    if not entity then
        return "unknown"
    end

    if (type(entity) == "userdata") then
        entity = System.GetEntity(entity)
    end

    -- Try various methods to get name
    if entity["soul"] then
        local nameId = entity.soul:GetNameStringId()
        if nameId and nameId ~= "" then
            return nameId
        end
    end

    if entity["uiname"] and entity.uiname ~= "" then
        return entity.uiname
    end

    local name = entity:GetName()
    if name and name ~= "" then
        return name
    end

    return "unnamed_" .. tostring(entity.id)
end

function LootBeacon.EntityDetector:isCustomEntityClass(className)
    if not className or not LootBeacon.Config.customEntityClasses or LootBeacon.Config.customEntityClasses == "" then
        return false
    end

    -- Split the comma-separated string into a table
    for class in string.gmatch(LootBeacon.Config.customEntityClasses, "([^,]+)") do
        -- Trim whitespace
        class = string.match(class, "^%s*(.-)%s*$")
        if class and class ~= "" and className == class then
            return true
        end
    end

    return false
end

return LootBeacon.EntityDetector
