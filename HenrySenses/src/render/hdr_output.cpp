/**
 * @file render/hdr_output.cpp
 * @brief HDR output detection, the HDR Output Transform's scene, the scene copy and SDR white.
 */

#include "render/hdr_output.hpp"
#include "render/render_calls.hpp"
#include "render/silhouette_shader.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "engine/engine_env.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <malloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>

namespace HenrySenses
{
    namespace
    {
        using namespace render_calls;

        using HdrSceneTargetFn = std::uintptr_t(__fastcall *)(std::uintptr_t resources, std::uint32_t id);
        using FullscreenCtorFn = void *(__fastcall *)(void *self,
                                                      std::uintptr_t pipeline,
                                                      const char *name,
                                                      std::uint32_t flags);

        /**
         * @struct HdrCalls
         * @brief The engine functions the HDR path calls (0 when unresolved).
         */
        struct HdrCalls : PassCalls
        {
            std::uintptr_t hdr_scene_target{0};
            std::uintptr_t fullscreen_ctor{0};
            std::uintptr_t post_fx_render_target{0};
        };
        HdrCalls s_calls{};

        /**
         * @struct ChangeLog
         * @brief An on/off state the render thread logs when it changes, capped: a frame can composite more than one
         *        view, and the views may disagree.
         */
        struct ChangeLog
        {
            static constexpr std::uint32_t MAX_CHANGES = 8;
            int last{-1};
            std::uint32_t count{0};

            /** @brief Records @p state; true when it changed and the cap still allows logging it. */
            [[nodiscard]] bool changed(bool state) noexcept
            {
                if (static_cast<int>(state) == last || count >= MAX_CHANGES)
                {
                    return false;
                }
                last = static_cast<int>(state);
                ++count;
                return true;
            }
        };

        // The last composite went into the HDR scene: set by the render thread, read by the main thread.
        std::atomic<bool> s_hdr_output{false};
        ChangeLog s_hdr_output_log{};

        // SDR white in scene units (r_HDRUIBrightness / 80 nits): set on the main thread, read by the render thread.
        // Starts at the game's default.
        std::atomic<float> s_sdr_white{constants::HDR_DEFAULT_UI_NITS / constants::HDR_WHITE_NITS};
        // Main thread: the UI brightness last logged, and the value last read and since when, so a slider drag logs
        // once it settles instead of every tick.
        float s_ui_nits_logged = -1.0f;
        float s_ui_nits_seen = -1.0f;
        std::int64_t s_ui_nits_seen_ms = 0;
        constexpr std::int64_t UI_NITS_SETTLE_MS = 1000;

        // The scene copy: a CFullscreenPass the mod owns and a target of the scene's size and format, built on the
        // render thread for the pipeline they serve, freed there once idle, and released at shutdown.
        constexpr const char *COPY_PASS_NAME = "HenrySenses Scene Copy";
        constexpr std::uint32_t COPY_TECHNIQUE_CRC = technique_crc(constants::SCENE_COPY_TECHNIQUE);
        // A copy no HDR composite used for this long is freed (VRAM back after HDR output is turned off).
        constexpr std::int64_t COPY_IDLE_RELEASE_MS = 10000;
        void *s_copy_pass = nullptr;
        std::uintptr_t s_copy_pipeline = 0;
        std::uintptr_t s_copy = 0;
        std::array<char, 48> s_copy_name{};
        bool s_copy_off = false;
        // The copy technique drew once, and the copies it failed before (its permutation compiles on its own).
        bool s_copy_confirmed = false;
        std::uint32_t s_copy_failures = 0;
        std::int64_t s_copy_used_ms = 0;
        std::atomic<std::uint64_t> s_copies{0};
        // Copy pass bindings, rebuilt when the source, the target, the shader or the binding generation changes.
        std::uintptr_t s_bound_source = 0;
        std::uintptr_t s_bound_target = 0;
        std::uintptr_t s_bound_shader = 0;
        std::uint32_t s_bound_generation = 0;
        std::uint32_t s_copy_technique_key{COPY_TECHNIQUE_CRC};
        // Whether the composite went through the scene copy.
        ChangeLog s_copy_mode_log{};

        [[nodiscard]] std::int64_t now_ms() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        [[nodiscard]] std::uintptr_t guarded_hdr_scene_target(std::uintptr_t resources) noexcept
        {
            __try
            {
                return reinterpret_cast<HdrSceneTargetFn>(s_calls.hdr_scene_target)(
                    resources,
                    constants::HDR_SCENE_RESOURCE_ID
                );
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        [[nodiscard]] void *guarded_construct_copy_pass(void *memory, std::uintptr_t pipeline) noexcept
        {
            __try
            {
                return reinterpret_cast<FullscreenCtorFn>(s_calls.fullscreen_ctor)(
                    memory,
                    pipeline,
                    COPY_PASS_NAME,
                    constants::FULLSCREEN_PASS_FLAGS
                );
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return nullptr;
            }
        }

        /**
         * @brief Creates the scene copy at @p scene's size, format and flags, or resizes it.
         * @details A copy that already matches is kept without asking the engine, which looks the target up by name.
         * @return True when the copy exists at that size and format.
         */
        [[nodiscard]] bool guarded_ensure_copy(std::uintptr_t scene) noexcept
        {
            __try
            {
                const auto format = load<std::uint8_t>(scene + constants::TEXTURE_FORMAT_OFFSET);
                const std::int32_t width = load<std::uint16_t>(scene + constants::TEXTURE_WIDTH_OFFSET);
                const std::int32_t height = load<std::uint16_t>(scene + constants::TEXTURE_HEIGHT_OFFSET);
                const auto matches = [format, width, height]
                {
                    return s_copy != 0 && load<std::uint8_t>(s_copy + constants::TEXTURE_FORMAT_OFFSET) == format &&
                           load<std::uint16_t>(s_copy + constants::TEXTURE_WIDTH_OFFSET) == width &&
                           load<std::uint16_t>(s_copy + constants::TEXTURE_HEIGHT_OFFSET) == height;
                };
                if (matches())
                {
                    return true;
                }
                const std::uint32_t flags = load<std::uint32_t>(scene + constants::TEXTURE_FLAGS_OFFSET) &
                                            ~constants::TEXTURE_FLAG_DONT_RELEASE;
                const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (width <= 0 || height <= 0 ||
                    !reinterpret_cast<PostFxTargetFn>(s_calls.post_fx_render_target)(
                        s_copy_name.data(),
                        &s_copy,
                        width,
                        height,
                        clear_color,
                        false,
                        false,
                        format,
                        -1,
                        flags
                    ))
                {
                    return false;
                }
                return matches();
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Draws @p scene into the copy with the mod's copy technique.
         * @return 1 when the pass executed, 0 when it had nothing to draw yet (its permutation still compiling), -1 on
         *         a fault.
         */
        [[nodiscard]] int guarded_copy(std::uintptr_t shader, std::uintptr_t scene, std::uint32_t generation) noexcept
        {
            __try
            {
                const auto pass = reinterpret_cast<std::uintptr_t>(s_copy_pass);
                // Until the copy technique has drawn once it is bound every frame, as the composite's is.
                if (scene != s_bound_source || s_copy != s_bound_target || shader != s_bound_shader ||
                    generation != s_bound_generation || !s_copy_confirmed)
                {
                    reinterpret_cast<SetTechniqueFn>(s_calls.set_technique)(
                        pass + constants::FULLSCREEN_PRIMITIVE_OFFSET,
                        shader,
                        &s_copy_technique_key,
                        0,
                        false
                    );
                    (void)reinterpret_cast<SetRenderTargetFn>(s_calls.set_render_target)(
                        pass + constants::PRIMITIVE_PASS_RENDER_TARGET_THIS_OFFSET,
                        0,
                        s_copy,
                        constants::RESOURCE_VIEW_RENDER_TARGET
                    );
                    const std::uintptr_t dirty = pass + constants::FULLSCREEN_DIRTY_OFFSET;
                    if (load<std::uint64_t>(pass + constants::FULLSCREEN_RENDER_STATE_OFFSET) !=
                        constants::RENDER_STATE_OPAQUE_NO_DEPTH)
                    {
                        store<std::uint64_t>(
                            pass + constants::FULLSCREEN_RENDER_STATE_OFFSET,
                            constants::RENDER_STATE_OPAQUE_NO_DEPTH
                        );
                        store<std::uint32_t>(
                            dirty,
                            load<std::uint32_t>(dirty) | constants::FULLSCREEN_DIRTY_RENDER_STATE
                        );
                    }
                    reinterpret_cast<SetTextureFn>(s_calls.set_texture)(
                        pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                        0,
                        scene,
                        0,
                        constants::SHADER_STAGE_PIXEL
                    );
                    (void)reinterpret_cast<SetSamplerFn>(s_calls.set_sampler)(
                        pass + constants::FULLSCREEN_RESOURCE_DESC_OFFSET,
                        0,
                        constants::SAMPLER_LINEAR_CLAMP,
                        constants::SHADER_STAGE_PIXEL
                    );
                    if (load<std::uint32_t>(pass + constants::FULLSCREEN_PRIMITIVE_TYPE_OFFSET) !=
                        constants::PRIMITIVE_TYPE_PROCEDURAL_TRIANGLE)
                    {
                        store<std::uint32_t>(
                            pass + constants::FULLSCREEN_PRIMITIVE_TYPE_OFFSET,
                            constants::PRIMITIVE_TYPE_PROCEDURAL_TRIANGLE
                        );
                        store<std::uint32_t>(
                            dirty,
                            load<std::uint32_t>(dirty) | constants::FULLSCREEN_DIRTY_PRIMITIVE_TYPE
                        );
                    }
                    s_bound_source = scene;
                    s_bound_target = s_copy;
                    s_bound_shader = shader;
                    s_bound_generation = generation;
                }
                reinterpret_cast<BeginConstantUpdateFn>(s_calls.begin_constant_update)(pass);
                return reinterpret_cast<FullscreenExecuteFn>(s_calls.fullscreen_execute)(pass) ? 1 : 0;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return -1;
            }
        }

        /**
         * @brief Destroys the copy pass, and the copy unless @p keep_target (a new pipeline only needs a new pass).
         */
        void release_copy(bool keep_target) noexcept
        {
            if (s_copy_pass != nullptr)
            {
                void *const pass = s_copy_pass;
                s_copy_pass = nullptr;
                s_copy_pipeline = 0;
                // A pass whose destructor faulted is leaked rather than freed under engine references.
                if (guarded_destroy_pass(pass))
                {
                    _aligned_free(pass);
                }
            }
            if (!keep_target && s_copy != 0)
            {
                // A texture whose Release faulted is left alone rather than released twice.
                (void)guarded_release_texture(s_copy);
                s_copy = 0;
            }
            s_bound_source = 0;
            s_bound_target = 0;
            s_bound_shader = 0;
        }

        /** @brief Turns the scene copy off for the session and says why (the HDR composite adds light instead). */
        void reject_copy(const char *why) noexcept
        {
            s_copy_off = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "HdrOutput: the scene copy is off for the session: {}; HDR highlights are added as light instead",
                why
            );
        }

        /** @brief Copies @p scene; true when the copy holds this frame's scene. */
        [[nodiscard]] bool copy_scene(std::uintptr_t pipeline, std::uintptr_t scene, std::uint32_t generation) noexcept
        {
            const std::uintptr_t shader = silhouette_shader();
            if (s_copy_off || shader == 0 || s_calls.fullscreen_ctor == 0)
            {
                return false;
            }
            if (s_copy_pass != nullptr && s_copy_pipeline != pipeline)
            {
                release_copy(true);
            }
            if (s_copy_pass == nullptr)
            {
                void *memory = _aligned_malloc(constants::FULLSCREEN_PASS_SIZE, 16);
                if (memory == nullptr)
                {
                    return false;
                }
                std::memset(memory, 0, constants::FULLSCREEN_PASS_SIZE);
                if (guarded_construct_copy_pass(memory, pipeline) == nullptr)
                {
                    _aligned_free(memory);
                    reject_copy("its pass could not be built");
                    return false;
                }
                s_copy_pass = memory;
                s_copy_pipeline = pipeline;
            }
            if (!guarded_ensure_copy(scene))
            {
                reject_copy("its target could not be created");
                return false;
            }
            const int copied = guarded_copy(shader, scene, generation);
            if (copied < 0)
            {
                reject_copy("its pass faulted");
                return false;
            }
            if (copied == 0)
            {
                if (++s_copy_failures >= TECHNIQUE_WARMUP_ATTEMPTS)
                {
                    reject_copy("its technique never drew (the permutation did not compile)");
                }
                return false;
            }
            s_copies.fetch_add(1, std::memory_order_relaxed);
            s_copy_used_ms = now_ms();
            if (!s_copy_confirmed)
            {
                s_copy_confirmed = true;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "HdrOutput: the scene copy drew after {} attempt(s) ({}x{})",
                    s_copy_failures + 1,
                    DMK::memory::read<std::uint16_t>(DMK::Address{scene + constants::TEXTURE_WIDTH_OFFSET}).value_or(0),
                    DMK::memory::read<std::uint16_t>(DMK::Address{scene + constants::TEXTURE_HEIGHT_OFFSET}).value_or(0)
                );
            }
            return true;
        }

        /** @brief Logs a change of the HDR composite's mode, capped. */
        void note_copy_mode(bool through_copy) noexcept
        {
            if (!s_copy_mode_log.changed(through_copy))
            {
                return;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "HdrOutput: highlights {}",
                through_copy ? "are added as SDR adds them, through the scene copy"
                             : "are added as light (no scene copy yet)"
            );
        }
    } // namespace

    void initialize_hdr_output() noexcept
    {
        s_calls = {};
        static_cast<PassCalls &>(s_calls) = resolve_pass_calls();
        s_calls.hdr_scene_target = gated_anchor_address(Feature::HdrOutput, AnchorId::HdrSceneTarget);
        s_calls.fullscreen_ctor = gated_anchor_address(Feature::HdrSceneCopy, AnchorId::FullscreenPassCtor);
        s_calls.post_fx_render_target = gated_anchor_address(Feature::HdrSceneCopy, AnchorId::PostFxRenderTarget);
        // The copy pass drives the same pass calls as the composite.
        if (!s_calls.ready() || s_calls.post_fx_render_target == 0)
        {
            s_calls.fullscreen_ctor = 0;
        }

        // A refused unload re-runs init on the same image, whose previous teardown released the copy.
        s_hdr_output.store(false, std::memory_order_relaxed);
        s_hdr_output_log = {};
        s_sdr_white.store(constants::HDR_DEFAULT_UI_NITS / constants::HDR_WHITE_NITS, std::memory_order_relaxed);
        s_ui_nits_logged = -1.0f;
        s_ui_nits_seen = -1.0f;
        s_ui_nits_seen_ms = 0;
        s_copy_pass = nullptr;
        s_copy_pipeline = 0;
        s_copy = 0;
        s_copy_off = false;
        s_copy_confirmed = false;
        s_copy_failures = 0;
        s_copy_used_ms = 0;
        s_copies.store(0, std::memory_order_relaxed);
        s_bound_source = 0;
        s_bound_target = 0;
        s_bound_shader = 0;
        s_copy_mode_log = {};

        // A per-load token in the name: a reloaded generation never asks for a texture its predecessor released.
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        const auto token = static_cast<std::uint32_t>(counter.QuadPart ^ (counter.QuadPart >> 32));
        const auto written =
            std::format_to_n(s_copy_name.data(), s_copy_name.size() - 1, "$HenrySensesSceneCopy_{:08X}", token);
        *written.out = '\0';
    }

    bool hdr_output_active(std::uintptr_t stage) noexcept
    {
        // The walk does not read its last hop, so the trailing 0 reads the render output pointer itself.
        constexpr std::array<std::ptrdiff_t, 3> output_chain{
            constants::STAGE_RENDER_VIEW_OFFSET,
            constants::RENDER_VIEW_RENDER_OUTPUT_OFFSET,
            0
        };
        const auto output = DMK::memory::walk(DMK::Address{stage}, output_chain);
        if (!output || !DMK::memory::is_plausible_ptr(*output))
        {
            return false;
        }
        const auto flag =
            DMK::memory::read<std::uint8_t>(DMK::Address{output->raw() + constants::RENDER_OUTPUT_HDR_FLAG_OFFSET});
        return flag && *flag != 0;
    }

    void note_hdr_output(bool hdr, bool from_render_output) noexcept
    {
        s_hdr_output.store(hdr, std::memory_order_relaxed);
        if (!s_hdr_output_log.changed(hdr))
        {
            return;
        }
        (void)DMK::log().try_log(
            hdr && s_calls.hdr_scene_target == 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
            "HdrOutput: HDR output {} ({}); {}",
            hdr ? "on" : "off",
            from_render_output ? "render output" : "HdrOutput setting",
            !hdr                            ? "highlights composite after the LDR chain"
            : s_calls.hdr_scene_target != 0 ? "highlights composite into the HDR Output Transform's scene"
                                            : "the HdrOutput gate failed, so highlights cannot show"
        );
    }

    std::uintptr_t hdr_scene_target(std::uintptr_t stage) noexcept
    {
        if (s_calls.hdr_scene_target == 0)
        {
            return 0;
        }
        const auto resources =
            DMK::memory::read<std::uintptr_t>(DMK::Address{stage + constants::STAGE_RESOURCES_OFFSET});
        if (!resources || !DMK::memory::is_plausible_ptr(DMK::Address{*resources}))
        {
            return 0;
        }
        const std::uintptr_t texture = guarded_hdr_scene_target(*resources);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{texture}))
        {
            return 0;
        }
        const auto device = DMK::memory::read<std::uintptr_t>(DMK::Address{texture + constants::TEXTURE_DEVICE_OFFSET});
        return device && *device != 0 ? texture : 0;
    }

    std::uintptr_t
    copy_hdr_scene(std::uintptr_t pipeline, std::uintptr_t scene, std::uint32_t binding_generation) noexcept
    {
        const bool copied = copy_scene(pipeline, scene, binding_generation);
        note_copy_mode(copied);
        return copied ? s_copy : 0;
    }

    void release_idle_hdr_scene_copy() noexcept
    {
        if ((s_copy != 0 || s_copy_pass != nullptr) && now_ms() - s_copy_used_ms >= COPY_IDLE_RELEASE_MS)
        {
            release_copy(false);
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "HdrOutput: the idle scene copy was freed");
        }
    }

    float hdr_sdr_white() noexcept
    {
        return s_sdr_white.load(std::memory_order_relaxed);
    }

    void update_hdr_sdr_white() noexcept
    {
        if (!s_hdr_output.load(std::memory_order_relaxed))
        {
            return;
        }
        const float ui_nits =
            std::max(hdr_ui_brightness().value_or(constants::HDR_DEFAULT_UI_NITS), constants::HDR_MIN_UI_NITS);
        s_sdr_white.store(ui_nits / constants::HDR_WHITE_NITS, std::memory_order_relaxed);
        const std::int64_t now = now_ms();
        if (ui_nits != s_ui_nits_seen)
        {
            s_ui_nits_seen = ui_nits;
            s_ui_nits_seen_ms = now;
        }
        // The first value logs at once; a change logs once it has held for the settle time.
        if (ui_nits != s_ui_nits_logged && (s_ui_nits_logged < 0.0f || now - s_ui_nits_seen_ms >= UI_NITS_SETTLE_MS))
        {
            s_ui_nits_logged = ui_nits;
            (void)DMK::log()
                .try_log(DMK::LogLevel::Info, "HdrOutput: SDR white sits at the UI brightness, {:.0f} nits", ui_nits);
        }
    }

    std::uint64_t hdr_scene_copies() noexcept
    {
        return s_copies.load(std::memory_order_relaxed);
    }

    void release_hdr_output() noexcept
    {
        release_copy(false);
    }

} // namespace HenrySenses
