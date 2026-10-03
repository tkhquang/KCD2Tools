/**
 * @file rtti_types.cpp
 * @brief Construction and publication of the cached class vtable identities.
 */

#include "rtti_types.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace HenrySenses
{
    namespace
    {
        constexpr std::size_t CLASS_COUNT = static_cast<std::size_t>(GameClass::Count);

        /// MSVC decorated names, indexed by GameClass. The enumerator order IS this order.
        constexpr std::array<std::string_view, CLASS_COUNT> CLASS_NAMES = {{
            constants::C_PLAYER_RTTI_NAME,
            constants::C_ENTITY_RTTI_NAME,
            constants::C_RENDER_PROXY_RTTI_NAME,
            constants::C_ENTITY_SYSTEM_RTTI_NAME,
            constants::C_AUX_GEOM_RTTI_NAME,
            constants::C_CAMERA_COMBAT_RTTI_NAME,
            constants::C_CAMERA_DIALOG_RTTI_NAME,
            constants::C_3DENGINE_RTTI_NAME,
            constants::C_RENDERER_RTTI_NAME,
            constants::C_STD_PIPELINE_RTTI_NAME,
            constants::C_CUSTOM_STAGE_RTTI_NAME,
            constants::C_FORWARD_STAGE_RTTI_NAME,
            constants::C_MERGED_MESH_NODE_RTTI_NAME,
            constants::C_BRUSH_RTTI_NAME,
            constants::C_CHAR_INSTANCE_RTTI_NAME,
            constants::C_OWNED_BRUSH_RTTI_NAME,
            constants::C_MOVABLE_BRUSH_RTTI_NAME,
            constants::C_NPC_ACTOR_RTTI_NAME,
            constants::C_HORSE_RTTI_NAME,
            constants::C_DOG_RTTI_NAME,
            constants::C_ANIMAL_RTTI_NAME,
            constants::C_AI_NPC_RTTI_NAME,
            constants::C_SOUL_RTTI_NAME,
            constants::C_INVENTORY_RTTI_NAME,
            constants::C_ITEM_SLOT_RTTI_NAME,
            constants::C_ITEM_SLOT_PILE_RTTI_NAME,
            constants::C_ITEM_VECTOR_BORROWER_RTTI_NAME,
            constants::C_ITEM_WRAPPER_RTTI_NAME,
            constants::C_WORLD_INVENTORY_RTTI_NAME,
            constants::C_PICKABLE_ITEM_RTTI_NAME,
            constants::C_ITEM_RTTI_NAME,
            constants::C_ACTOR_SYSTEM_RTTI_NAME,
            constants::C_ITEM_SYSTEM_RTTI_NAME,
            constants::C_GAME_RTTI_NAME,
            constants::C_GAME_MODEL_RTTI_NAME,
            constants::C_SCRIPT_CONTEXT_MANAGER_RTTI_NAME,
            constants::C_ENTITY_MODULE_RTTI_NAME,
            constants::C_INVENTORY_MANAGER_RTTI_NAME,
            constants::C_SHOP_MODULE_RTTI_NAME,
            constants::C_SHOP_RTTI_NAME,
            constants::C_RPG_MODULE_RTTI_NAME,
            constants::C_STASH_RTTI_NAME,
            constants::C_XML_NODE_RTTI_NAME,
            constants::C_XML_READ_ONLY_NODE_RTTI_NAME,
            constants::C_MERGED_MESHES_MANAGER_RTTI_NAME,
            constants::C_VEGETATION_RTTI_NAME,
            constants::C_CRY_PAK_RTTI_NAME,
            constants::C_PARTICLE_EMITTER_RTTI_NAME,
            constants::C_XML_UTILS_RTTI_NAME,
            constants::C_PARTICLE_MANAGER_RTTI_NAME,
        }};

        // WHGame.dll stays mapped for the session. The table is immutable after publication, so game callbacks
        // only compare vtables. A changed image revokes the whole table until the mod restarts.
        std::array<std::uintptr_t, CLASS_COUNT> s_vtables{};
        DMK::Region s_image{};
        DMK::scan::ImageIdentity s_identity{};
        std::atomic<bool> s_ready{false};
        std::atomic<bool> s_valid{false};
    } // namespace

    void init_game_types(DMK::Region image)
    {
        if (s_ready.load(std::memory_order_acquire))
        {
            refresh_game_types();
            return;
        }
        const DMK::scan::ImageIdentity identity = DMK::scan::image_identity(image);
        std::array<std::uintptr_t, CLASS_COUNT> resolved{};
        std::size_t unresolved = 0;
        for (std::size_t i = 0; i < CLASS_COUNT; ++i)
        {
            DMK::rtti::TypeIdentity type(CLASS_NAMES[i], image);
            if (const std::optional<DMK::Address> primary = type.vtable(); primary.has_value())
            {
                resolved[i] = primary->raw();
                DMK::log().debug("RTTI: {} vtable {}", CLASS_NAMES[i], DMK::format::format_address(primary->raw()));
            }
            else
            {
                ++unresolved;
                DMK::log().warning("RTTI: {} did not resolve; identity checks report no match", CLASS_NAMES[i]);
            }
        }
        s_image = image;
        s_identity = identity;
        s_vtables = resolved;
        s_valid.store(identity.present() && DMK::scan::image_identity(image) == identity, std::memory_order_release);
        s_ready.store(true, std::memory_order_release);
        DMK::log().info("RTTI: {}/{} class identities resolved", CLASS_COUNT - unresolved, CLASS_COUNT);
    }

    void refresh_game_types() noexcept
    {
        if (!s_ready.load(std::memory_order_acquire) || !s_valid.load(std::memory_order_acquire))
        {
            return;
        }
        // An explicit image range avoids a loader query. Revoke stale evidence without a cold RTTI sweep on a
        // game thread, even if a replacement image puts its primary vtable at the same address.
        if (DMK::scan::image_identity(s_image) != s_identity && s_valid.exchange(false, std::memory_order_acq_rel))
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Warning,
                "RTTI: the game image changed; identity checks are disabled until the mod restarts"
            );
        }
    }

    bool vtable_is(GameClass klass, std::uintptr_t vtable) noexcept
    {
        return vtable != 0 && class_vtable(klass) == vtable;
    }

    bool object_is(GameClass klass, std::uintptr_t object) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{object}))
        {
            return false;
        }
        const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
        return vtable.has_value() && vtable_is(klass, *vtable);
    }

    std::uintptr_t class_vtable(GameClass klass) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(klass);
        if (index >= CLASS_COUNT || !s_ready.load(std::memory_order_acquire) ||
            !s_valid.load(std::memory_order_acquire))
        {
            return 0;
        }
        return s_vtables[index];
    }

} // namespace HenrySenses
