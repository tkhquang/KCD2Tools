/**
 * @file render/silhouette_shader.cpp
 * @brief Writes the composite shader's binary into the user shader cache and loads it through the shader manager.
 */

#include "render/silhouette_shader.hpp"
#include "render/silhouette_shader_blob.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "engine/engine_env.hpp"
#include "engine/seh.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        using AdjustFileNameFn = const char *(__fastcall *)(std::uintptr_t pak,
                                                            const char *source,
                                                            char *destination,
                                                            std::uint32_t flags);
        using ShaderForNameFn = std::uintptr_t(__fastcall *)(
            std::uintptr_t shader_manager,
            const char *name,
            std::uint32_t flags,
            std::uintptr_t resources,
            std::uint64_t mask_gen
        );

        std::atomic<bool> s_tried{false};
        std::atomic<std::uintptr_t> s_shader{0};

        [[nodiscard]] bool guarded_adjust_file_name(
            std::uintptr_t fn,
            std::uintptr_t pak,
            const char *source,
            char *destination,
            std::uint32_t flags
        ) noexcept
        {
            __try
            {
                return reinterpret_cast<AdjustFileNameFn>(fn)(pak, source, destination, flags) != nullptr;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] std::uintptr_t
        guarded_shader_for_name(std::uintptr_t fn, std::uintptr_t shader_manager, const char *name) noexcept
        {
            __try
            {
                return reinterpret_cast<ShaderForNameFn>(fn)(
                    shader_manager,
                    name,
                    constants::SHADER_LOAD_FLAG_SYSTEM,
                    0,
                    0
                );
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        /**
         * @brief Resolves the shader file under the KCD2 save-data folder through the engine's alias table.
         * @details The default directory is %USERPROFILE%/Saved Games/kingdomcome2/shaders/cache/d3d12/.
         *          Resolve the engine's %USER% alias so relocated Saved Games folders and custom -user paths work.
         * @return The path, or an empty one when CryPak or its AdjustFileName did not resolve.
         */
        [[nodiscard]] std::filesystem::path cache_file_path()
        {
            const std::string source =
                std::format("{}{}.cfxb", constants::SILHOUETTE_SHADER_CACHE_DIR, SILHOUETTE_SHADER_CFXB_NAME);
            const std::uintptr_t pak = genv_interface(constants::GENV_CRY_PAK_OFFSET);
            if (pak == 0 || !object_is(GameClass::CryPak, pak))
            {
                return {};
            }
            const std::uintptr_t fn =
                validated_vtable_slot(pak, constants::CRY_PAK_VTABLE_ADJUST_FILE_NAME_OFFSET, AnchorId::AdjustFileName);
            if (fn == 0)
            {
                return {};
            }
            std::array<char, constants::CRY_PAK_MAX_PATH> buffer{};
            if (!guarded_adjust_file_name(
                    fn,
                    pak,
                    source.c_str(),
                    buffer.data(),
                    constants::CRY_PAK_FLAGS_PATH_REAL | constants::CRY_PAK_FLAGS_FOR_WRITING
                ))
            {
                return {};
            }
            buffer.back() = '\0';
            return std::filesystem::path(std::string(buffer.data()));
        }

        /**
         * @brief Writes the embedded binary to @p path unless the file there already holds exactly it.
         * @return An empty string on success, else what went wrong.
         */
        [[nodiscard]] std::string write_binary(const std::filesystem::path &path)
        {
            std::error_code error;
            if (std::filesystem::file_size(path, error) == SILHOUETTE_SHADER_CFXB.size() && !error)
            {
                std::ifstream existing(path, std::ios::binary);
                const std::vector<char> bytes{std::istreambuf_iterator<char>(existing), {}};
                if (bytes.size() == SILHOUETTE_SHADER_CFXB.size() &&
                    std::equal(
                        bytes.begin(),
                        bytes.end(),
                        SILHOUETTE_SHADER_CFXB.begin(),
                        [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }
                    ))
                {
                    return {};
                }
            }
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return std::format("cannot create {}: {}", path.parent_path().string(), error.message());
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(
                reinterpret_cast<const char *>(SILHOUETTE_SHADER_CFXB.data()),
                static_cast<std::streamsize>(SILHOUETTE_SHADER_CFXB.size())
            );
            out.close();
            return out ? std::string{} : std::format("cannot write {}", path.string());
        }

        /**
         * @brief Deletes the older versions of the shader's file next to @p path (another crc in the name).
         */
        void remove_stale_binaries(const std::filesystem::path &path)
        {
            const auto lower = [](std::string text)
            {
                std::transform(
                    text.begin(),
                    text.end(),
                    text.begin(),
                    [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); }
                );
                return text;
            };
            const std::string keep = lower(path.filename().string());
            std::error_code error;
            for (const auto &entry : std::filesystem::directory_iterator(path.parent_path(), error))
            {
                const std::string name = lower(entry.path().filename().string());
                if (name != keep && name.starts_with(constants::SILHOUETTE_SHADER_FILE_PREFIX) &&
                    name.ends_with(".cfxb"))
                {
                    std::filesystem::remove(entry.path(), error);
                }
            }
        }

        /**
         * @brief The renderer's CShaderMan, proven by its shader path string, or 0.
         */
        [[nodiscard]] std::uintptr_t shader_manager()
        {
            const std::uintptr_t renderer = genv_interface(constants::GENV_RENDERER_OFFSET);
            if (renderer == 0 || !object_is(GameClass::Renderer, renderer))
            {
                return 0;
            }
            const std::uintptr_t manager = renderer + constants::RENDERER_SHADER_MAN_OFFSET;
            const auto path =
                DMK::memory::read<std::uintptr_t>(DMK::Address{manager + constants::SHADER_MAN_SHADERS_PATH_OFFSET});
            if (!path || read_c_string(*path, 64) != constants::SHADER_MAN_SHADERS_PATH)
            {
                return 0;
            }
            return manager;
        }

        void fail(const std::string &reason)
        {
            DMK::log().warning("SilhouetteShader: {}; the stock composite stays in use", reason);
        }
    } // namespace

    void load_silhouette_shader() noexcept
    {
        if (s_tried.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        try
        {
            const std::uintptr_t for_name = gated_anchor_address(Feature::SilhouetteShader, AnchorId::ShaderForName);
            if (for_name == 0)
            {
                fail("the shader loader gate failed");
                return;
            }
            const std::filesystem::path path = cache_file_path();
            if (path.empty())
            {
                fail("the user shader cache path did not resolve (CryPak AdjustFileName)");
                return;
            }
            if (const std::string error = write_binary(path); !error.empty())
            {
                fail(error);
                return;
            }
            remove_stale_binaries(path);
            const std::uintptr_t manager = shader_manager();
            if (manager == 0)
            {
                fail("the renderer's shader manager did not check out");
                return;
            }
            const std::uintptr_t shader = guarded_shader_for_name(for_name, manager, SILHOUETTE_SHADER_CFXB_NAME);
            const auto flags = DMK::memory::read<std::uint32_t>(DMK::Address{shader + constants::SHADER_FLAGS_OFFSET});
            if (shader == 0 || !flags || (*flags & constants::SHADER_FLAG_NOT_FOUND) != 0)
            {
                fail(std::format("{} did not load from {}", SILHOUETTE_SHADER_CFXB_NAME, path.string()));
                return;
            }
            s_shader.store(shader, std::memory_order_release);
            DMK::log().info(
                "SilhouetteShader: {} requested from {} (CShader 0x{:016X}); the composite switches to it once its "
                "first draw succeeds",
                SILHOUETTE_SHADER_CFXB_NAME,
                path.string(),
                shader
            );
        }
        catch (...)
        {
            fail("an exception while loading");
        }
    }

    std::uintptr_t silhouette_shader() noexcept
    {
        return s_shader.load(std::memory_order_acquire);
    }

    void reject_silhouette_shader(const char *reason) noexcept
    {
        if (s_shader.exchange(0, std::memory_order_acq_rel) != 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "SilhouetteShader: {}; the stock composite is used for the rest of the session",
                reason
            );
        }
    }

} // namespace HenrySenses
