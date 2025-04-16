--[[
Highlighter module for Loot Beacon mod
Manages particle effects and highlighting behavior
]]

LootBeacon.Highlighter = {
    -- State tracking
    isActive = false,
    timerID = nil,
    highlightedEntities = {} -- Format: {{entity=entity, slot=slot}, ...}
}

function LootBeacon.Highlighter:initialize()
    LootBeacon.Logger:debug("Initializing Highlighter")
    return self
end

function LootBeacon.Highlighter:activateHighlights()
    LootBeacon.Logger:info("Activating highlights")

    -- Cancel existing highlight if active
    if self.isActive then
        self:removeAllHighlights()
    end

    -- Set state to active
    self.isActive = true

    -- Find entities to highlight
    local entitiesFound = LootBeacon.EntityDetector:detectEntities()

    -- Apply highlight effects to detected entities
    local counts = self:applyHighlightEffects(entitiesFound)

    -- Show UI notifications
    LootBeacon.UIManager:showHighlightResults(counts)

    -- Set timer to automatically remove highlights
    self.timerID = Script.SetTimer(LootBeacon.Config.highlightDuration * 1000, function()
        LootBeacon.Highlighter:removeAllHighlights()
    end)

    return counts
end

function LootBeacon.Highlighter:applyHighlightEffects(entities)
    -- Initialize counts for UI
    local counts = {
        items = 0,
        corpses = 0,
        animals = 0,
        custom = 0,
        total = 0
    }

    -- Apply effects to pickable items
    if LootBeacon.Config.highlightItems then
        for _, entity in ipairs(entities.items) do
            if self:applyEffectToEntity(entity, LootBeacon.Config.itemParticleEffectPath) then
                counts.items = counts.items + 1
                counts.total = counts.total + 1
            end
        end
    end

    -- Apply effects to human corpses
    if LootBeacon.Config.highlightCorpses then
        for _, entity in ipairs(entities.corpses) do
            if self:applyEffectToEntity(entity, LootBeacon.Config.humanCorpseParticleEffectPath) then
                counts.corpses = counts.corpses + 1
                counts.total = counts.total + 1
            end
        end
    end

    -- Apply effects to animal corpses
    if LootBeacon.Config.highlightAnimals then
        for _, entity in ipairs(entities.animals) do
            if self:applyEffectToEntity(entity, LootBeacon.Config.animalCorpseParticleEffectPath) then
                counts.animals = counts.animals + 1
                counts.total = counts.total + 1
            end
        end
    end

    -- Apply effects to custom entities
    for _, entity in ipairs(entities.custom) do
        if self:applyEffectToEntity(entity, LootBeacon.Config.customEntityParticleEffectPath) then
            counts.custom = counts.custom + 1
            counts.total = counts.total + 1
        end
    end

    LootBeacon.Logger:info("Applied highlights to %d entities", counts.total)
    return counts
end

function LootBeacon.Highlighter:applyEffectToEntity(entity, effectPath)
    if not entity or not effectPath then
        LootBeacon.Logger:warning("Invalid entity or effect path")
        return false
    end

    -- Apply particle effect to the entity
    local slot = -1
    local success, result = pcall(function()
        return entity:LoadParticleEffect(-1, effectPath, {})
    end)

    if success and result and result >= 0 then
        slot = result
        table.insert(self.highlightedEntities, { entity = entity, slot = slot })

        -- Adjust orientation with randomized angles for visual variety
        self:adjustParticleOrientation(entity, slot)
        return true
    else
        LootBeacon.Logger:warning("Failed to load particle effect for entity: %s",
            tostring(result or "unknown error"))
        return false
    end
end

function LootBeacon.Highlighter:adjustParticleOrientation(entity, slot)
    -- Skip if no entity or slot
    if not entity or not slot or slot < 0 then
        return
    end

    -- Try to adjust orientation to make effect vertical
    pcall(function()
        -- First try to get entity's world angles
        local hasAngles, ang = false, nil

        if entity.GetWorldAngles then
            local angSuccess, angles = pcall(function() return entity:GetWorldAngles() end)
            if angSuccess and angles then
                hasAngles = true
                ang = angles
            end
        end

        -- Generate a random angle for upward direction
        local randomX = math.random(30, 90)

        if hasAngles and ang then
            -- Counter the entity's rotation and add randomized upward angle
            local adjustedAngles = {
                x = -ang.x + randomX,
                y = -ang.y,
                z = -ang.z
            }

            -- If original angles are all near zero, use a simpler orientation
            if math.abs(ang.x) < 0.1 and math.abs(ang.y) < 0.1 and math.abs(ang.z) < 0.1 then
                adjustedAngles = { x = randomX, y = 0, z = 0 }
            end

            entity:SetSlotAngles(slot, adjustedAngles)
        else
            -- If we can't get angles, set a default upward orientation
            entity:SetSlotAngles(slot, { x = randomX, y = 0, z = 0 })
        end
    end)
end

function LootBeacon.Highlighter:removeAllHighlights()
    if not self.isActive then
        return
    end

    LootBeacon.Logger:info("Removing highlights from %d entities", #self.highlightedEntities)

    -- Kill existing timer if active
    if self.timerID then
        Script.KillTimer(self.timerID)
        self.timerID = nil
    end

    -- Free all particle effect slots
    for _, data in ipairs(self.highlightedEntities) do
        local entity = data.entity
        local slot = data.slot

        -- Safe free slot call
        if entity and slot then
            pcall(function() entity:FreeSlot(slot) end)
        end
    end

    -- Reset state
    self.highlightedEntities = {}
    self.isActive = false
end

return LootBeacon.Highlighter
