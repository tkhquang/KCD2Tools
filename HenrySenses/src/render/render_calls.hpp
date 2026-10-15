/**
 * @file render/render_calls.hpp
 * @brief The engine render-pass call shapes, the technique key and the raw, SEH-scoped access the render path shares.
 *
 * engine_silhouette.cpp and hdr_output.cpp drive engine passes through the same AOB-resolved functions, so their
 * signatures live here once. The raw load and store helpers are for use inside a caller's __try only.
 */
#ifndef HENRYSENSES_RENDER_CALLS_HPP
#define HENRYSENSES_RENDER_CALLS_HPP

#include "aob_resolver.hpp"
#include "constants.hpp"
#include "engine/seh.hpp"

#include <windows.h>

#include <cstdint>
#include <string_view>

namespace HenrySenses::render_calls
{
    using SetTechniqueFn = void(__fastcall *)(
        std::uintptr_t primitive,
        std::uintptr_t shader,
        const std::uint32_t *technique_crc,
        std::uint64_t rt_mask,
        bool apply_quality
    );
    using SetRenderTargetFn =
        bool(__fastcall *)(std::uintptr_t pass, std::uint32_t slot, std::uintptr_t texture, std::uint8_t view);
    using SetTextureFn = void(__fastcall *)(
        std::uintptr_t desc,
        std::uint8_t slot,
        std::uintptr_t texture,
        std::uint8_t view,
        std::uint16_t stages
    );
    using SetSamplerFn = std::uint32_t(__fastcall *)(
        std::uintptr_t desc,
        std::uint8_t slot,
        std::uint16_t sampler,
        std::uint32_t stages
    );
    using BeginConstantUpdateFn = void(__fastcall *)(std::uintptr_t pass);
    using FullscreenExecuteFn = bool(__fastcall *)(std::uintptr_t pass);
    // SPostEffectsUtils::GetOrCreateRenderTarget / GetOrCreateDepthStencil: creates the named target into *slot, or
    // resizes the one already there; true when it has a device texture.
    using PostFxTargetFn = bool(__fastcall *)(
        const char *name,
        std::uintptr_t *slot,
        std::int32_t width,
        std::int32_t height,
        const float *clear,
        bool use_alpha,
        bool mips,
        std::uint8_t format,
        std::int32_t custom_id,
        std::uint32_t flags
    );
    using TextureReleaseFn = std::int32_t(__fastcall *)(std::uintptr_t texture);
    using ScalarDtorFn = void *(__fastcall *)(void *self, unsigned int flags);

    /**
     * @struct PassCalls
     * @brief The engine pass calls the composite and the scene copy drive (0 when unresolved).
     */
    struct PassCalls
    {
        std::uintptr_t set_technique{0};
        std::uintptr_t set_render_target{0};
        std::uintptr_t set_texture{0};
        std::uintptr_t set_sampler{0};
        std::uintptr_t begin_constant_update{0};
        std::uintptr_t fullscreen_execute{0};

        /** @brief Every call resolved. */
        [[nodiscard]] bool ready() const noexcept
        {
            return set_technique != 0 && set_render_target != 0 && set_texture != 0 && set_sampler != 0 &&
                   begin_constant_update != 0 && fullscreen_execute != 0;
        }
    };

    /** @brief Resolves the pass calls from their anchors (init, after the anchors resolve). */
    [[nodiscard]] inline PassCalls resolve_pass_calls() noexcept
    {
        return {
            anchor_address(AnchorId::SetTechnique),
            anchor_address(AnchorId::SetRenderTarget),
            anchor_address(AnchorId::SetTexture),
            anchor_address(AnchorId::SetSampler),
            anchor_address(AnchorId::BeginConstantUpdate),
            anchor_address(AnchorId::FullscreenExecute),
        };
    }

    // Draws the mod's own technique may fail (its permutation still compiling) before it has drawn once; past them the
    // technique is given up for the session.
    inline constexpr std::uint32_t TECHNIQUE_WARMUP_ATTEMPTS = 900;

    /**
     * @brief Standard CRC-32 of the lower-cased text: the engine's CCryNameTSCRC technique key.
     * @param text Technique name.
     * @return The key.
     */
    [[nodiscard]] constexpr std::uint32_t technique_crc(std::string_view text) noexcept
    {
        std::uint32_t crc = 0xFFFFFFFFu;
        for (const char c : text)
        {
            const auto byte = static_cast<std::uint8_t>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
        return ~crc;
    }

    /**
     * @brief Raw typed load for use inside the SEH helpers only.
     */
    template <typename T> [[nodiscard]] T load(std::uintptr_t address) noexcept
    {
        return *reinterpret_cast<const volatile T *>(address);
    }

    /**
     * @brief Raw typed store for use inside the SEH helpers only.
     */
    template <typename T> void store(std::uintptr_t address, T value) noexcept
    {
        *reinterpret_cast<volatile T *>(address) = value;
    }

    /** @brief Releases one of the mod's textures (CTexture vtable Release); false on a fault. */
    [[nodiscard]] inline bool guarded_release_texture(std::uintptr_t texture) noexcept
    {
        __try
        {
            const std::uintptr_t vtable = load<std::uintptr_t>(texture);
            (void)reinterpret_cast<
                TextureReleaseFn>(load<std::uintptr_t>(vtable + constants::TEXTURE_VTABLE_RELEASE_OFFSET))(texture);
            return true;
        }
        __except (engine_fault_filter(GetExceptionCode()))
        {
            return false;
        }
    }

    /** @brief Destroys one of the mod's engine passes in place (vtable slot 0, flag 0: do not free); false on a fault.
     */
    [[nodiscard]] inline bool guarded_destroy_pass(void *pass) noexcept
    {
        __try
        {
            const auto vtable = *static_cast<const std::uintptr_t *>(pass);
            reinterpret_cast<ScalarDtorFn>(*reinterpret_cast<const std::uintptr_t *>(vtable))(pass, 0);
            return true;
        }
        __except (engine_fault_filter(GetExceptionCode()))
        {
            return false;
        }
    }

} // namespace HenrySenses::render_calls

#endif // HENRYSENSES_RENDER_CALLS_HPP
