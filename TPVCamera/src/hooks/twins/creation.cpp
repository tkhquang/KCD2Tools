/**
 * @file hooks/twins/creation.cpp
 * @brief Shader twin creation: the engine preparation, the candidates, the loading boundary, the creation in still
 * frames, the cache recovery and the level inventory, and the material, save-load and system-state detours.
 */

#include "hooks/twins/twin_state.hpp"

namespace TPVCamera::Twins
{
    namespace
    {

        /** @brief Calls CShaderMan::mfForName under SEH. 0 on a fault. */
        [[nodiscard]] std::uintptr_t for_name_guarded(std::uintptr_t fn, std::uintptr_t manager, const char *name,
                                                      std::uintptr_t resources, std::uint64_t mask) noexcept
        {
            __try
            {
                return reinterpret_cast<ForNameFn>(fn)(manager, name, Constants::SHADER_LOAD_FLAGS_MATERIAL, resources,
                                                       mask);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief Adds a reference to a CShader (what IShader::AddRef, vtable slot 2, does) under SEH. */
        [[nodiscard]] bool add_shader_ref_guarded(std::uintptr_t shader) noexcept
        {
            __try
            {
                _InterlockedIncrement(reinterpret_cast<volatile long *>(shader + Constants::SHADER_REF_COUNT_OFFSET));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief Calls ICryPak::AdjustFileName under SEH. False on a fault or a null result. */
        [[nodiscard]] bool adjust_file_name_guarded(std::uintptr_t fn, std::uintptr_t pak, const char *source,
                                                    char *destination, std::uint32_t flags) noexcept
        {
            __try
            {
                return reinterpret_cast<AdjustFileNameFn>(fn)(pak, source, destination, flags) != nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief The ICryPak calls the stock shader headers are read through. */
        struct PakCalls
        {
            std::uintptr_t open = 0;
            std::uintptr_t read = 0;
            std::uintptr_t close = 0;
        };

        /** @brief The outcome of a guarded header read. */
        enum class HeaderRead : std::uint8_t
        {
            Read,
            Missing,
            Fault,
        };

        /**
         * @brief Reads the first Constants::SHADER_BIN_HEADER_SIZE bytes of @p path through ICryPak, the way the
         *        shader-binary loader reads a header (FOpen "rb" with the engine flags 0, FReadRaw, FClose).
         * @note Main thread. SEH-guarded, POD body.
         */
        [[nodiscard]] HeaderRead read_pak_header_guarded(const PakCalls &calls, std::uintptr_t pak, const char *path,
                                                         std::uint8_t *out) noexcept
        {
            __try
            {
                void *file = reinterpret_cast<PakOpenFn>(calls.open)(pak, path, "rb", 0);
                if (file == nullptr)
                {
                    return HeaderRead::Missing;
                }
                (void)reinterpret_cast<PakReadRawFn>(calls.read)(pak, out, 1, Constants::SHADER_BIN_HEADER_SIZE, file);
                (void)reinterpret_cast<PakCloseFn>(calls.close)(pak, file);
                return HeaderRead::Read;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return HeaderRead::Fault;
            }
        }

        /** @brief True when @p address lies inside the WHGame.dll image. */
        [[nodiscard]] bool in_game_image(std::uintptr_t address) noexcept
        {
            const ModuleInfo &mod = module_info();
            return mod.base != 0 && address >= mod.base && address < mod.base + mod.size;
        }

        /**
         * @brief The function in @p object's vtable slot at @p slot_offset when it is @p expected, or a jmp thunk that
         *        lands on it. 0 otherwise.
         */
        [[nodiscard]] std::uintptr_t validated_slot(std::uintptr_t object, std::ptrdiff_t slot_offset,
                                                    std::uintptr_t expected) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            if (!vtable || expected == 0)
            {
                return 0;
            }
            const auto fn = DMK::memory::read<std::uintptr_t>(DMK::Address{*vtable + slot_offset});
            if (!fn)
            {
                return 0;
            }
            if (*fn == expected)
            {
                return *fn;
            }
            // A jmp thunk in the slot (another layer's hook, or an incremental-link stub) that lands on the function.
            const auto opcode = DMK::memory::read<std::uint8_t>(DMK::Address{*fn});
            if (opcode && *opcode == std::to_integer<std::uint8_t>(DMK::scan::PREFIX_JMP_REL32[0]))
            {
                const auto target =
                    DMK::scan::resolve_rip_relative(DMK::Address{*fn}, DMK::scan::PREFIX_JMP_REL32.size(),
                                                    DMK::scan::PREFIX_JMP_REL32.size() + sizeof(std::int32_t));
                if (target && target->raw() == expected)
                {
                    return *fn;
                }
            }
            return 0;
        }

        /** @brief The function in @p object's vtable slot at @p slot_offset when it lies in the game image, else 0. */
        [[nodiscard]] std::uintptr_t image_slot(std::uintptr_t object, std::ptrdiff_t slot_offset) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            if (!vtable || !in_game_image(*vtable))
            {
                return 0;
            }
            const auto fn = DMK::memory::read<std::uintptr_t>(DMK::Address{*vtable + slot_offset});
            return fn && in_game_image(*fn) ? *fn : 0;
        }

        /** @brief True when @p object (an IShader pointer) is a CShader. Any thread. */
        [[nodiscard]] bool is_shader(std::uintptr_t object) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            return vtable && vtable_is(GameClass::Shader, *vtable);
        }

        /** @brief True when @p object (an IRenderShaderResources pointer) is a CShaderResources. Any thread. */
        [[nodiscard]] bool is_shader_resources(std::uintptr_t object) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            return vtable && vtable_is(GameClass::ShaderResources, *vtable);
        }

        /** @brief Calls a verified resource reference method. Keep the SEH body free of C++ owners. */
        [[nodiscard]] bool resource_ref_guarded(std::uintptr_t fn, std::uintptr_t object) noexcept
        {
            __try
            {
                reinterpret_cast<void(__fastcall *)(std::uintptr_t)>(fn)(object);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /**
         * @brief Drops the references @p candidate owns and empties it.
         * @note A Release here can be the last one unless a callback still owns the item: such a drop runs on the
         *       engine main thread (or the teardown thread), outside every mod mutex.
         */
        void drop_candidate(TwinCandidate &candidate) noexcept
        {
            const TwinRequest request = std::exchange(candidate.request, TwinRequest{});
            if (request.resources != 0 && is_shader_resources(request.resources))
            {
                const auto fn = image_slot(request.resources, Constants::SHADER_RESOURCES_VTABLE_RELEASE_OFFSET);
                if (fn == 0 || !resource_ref_guarded(fn, request.resources))
                {
                    s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                }
            }
            release_candidate_shader(request.shader);
        }

        /** @brief The engine main thread's id as @p renderer's render thread records it, or 0 when unreadable. */
        [[nodiscard]] DWORD engine_main_thread_id(std::uintptr_t renderer) noexcept
        {
            const auto rt =
                DMK::memory::read<std::uintptr_t>(DMK::Address{renderer + Constants::RENDERER_RENDER_THREAD_OFFSET});
            if (!rt || *rt == 0)
            {
                return 0;
            }
            const auto id =
                DMK::memory::read<DWORD>(DMK::Address{*rt + Constants::RENDER_THREAD_MAIN_THREAD_ID_OFFSET});
            return id ? *id : 0;
        }

        /** @brief True on the engine main thread (g_env's renderer says which it is). Any thread. */
        [[nodiscard]] bool on_engine_main_thread() noexcept
        {
            const auto renderer =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_RENDERER_OFFSET});
            if (!renderer || *renderer == 0)
            {
                return false;
            }
            const DWORD main_thread = engine_main_thread_id(*renderer);
            return main_thread != 0 && main_thread == GetCurrentThreadId();
        }

        /**
         * @brief The address of the engine's shader reload count (CShaderMan m_nFrameForceReload), resolved once from
         *        g_env's renderer and proven like prepare_engine() proves the manager, or 0 while it does not check out.
         * @details Resolved apart from prepare_engine(), which a hot reload whose twins come back from the cache never
         *          runs.
         * @note Main thread.
         */
        [[nodiscard]] std::uintptr_t shader_reload_count_address() noexcept
        {
            if (s_reload_count_address != 0 || s_genv == 0)
            {
                return s_reload_count_address;
            }
            const auto renderer =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_RENDERER_OFFSET});
            if (!renderer || *renderer == 0)
            {
                return 0;
            }
            const auto renderer_vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*renderer});
            if (!renderer_vtable || !vtable_is(GameClass::Renderer, *renderer_vtable))
            {
                return 0;
            }
            const std::uintptr_t manager = *renderer + Constants::RENDERER_SHADER_MAN_OFFSET;
            std::array<char, 32> shaders_path{};
            copy_text_field_guarded(manager, Constants::SHADER_MAN_SHADERS_PATH_OFFSET, shaders_path.data(),
                                    shaders_path.size());
            if (std::string_view{shaders_path.data()} != Constants::SHADER_MAN_SHADERS_PATH)
            {
                return 0;
            }
            s_reload_count_address = manager + Constants::SHADER_MAN_FRAME_FORCE_RELOAD_OFFSET;
            return s_reload_count_address;
        }

        /** @brief How far take_candidate_refs() got. */
        enum class Ownership : std::uint8_t
        {
            Owned,      // a reference to the shader, and one to the resources when there are any, are held
            Rejected,   // another type, or the shader's reference could not be taken: nothing is held
            ShaderOnly, // the shader's reference is held and the resources' could not be taken: release the shader
        };

        /**
         * @brief Checks that @p item's shader is a CShader and its resources a CShaderResources (or none), and takes
         *        one reference to each: the pair a TwinCandidate owns until drop_candidate().
         * @note The caller keeps the item alive for the call (its callback owns it, or the level tables hold it). No
         *       engine Release runs here, so a caller may hold a mod mutex.
         */
        [[nodiscard]] Ownership take_candidate_refs(const ShaderItem &item) noexcept
        {
            if (!is_shader(item.shader) || (item.resources != 0 && !is_shader_resources(item.resources)))
            {
                return Ownership::Rejected;
            }
            const auto add_resource =
                item.resources != 0 ? image_slot(item.resources, Constants::SHADER_RESOURCES_VTABLE_ADD_REF_OFFSET) : 0;
            if ((item.resources != 0 && add_resource == 0) || !add_shader_ref_guarded(item.shader))
            {
                return Ownership::Rejected;
            }
            if (item.resources != 0 && !resource_ref_guarded(add_resource, item.resources))
            {
                return Ownership::ShaderOnly;
            }
            return Ownership::Owned;
        }

        /**
         * @brief Puts in @p out an owned candidate of @p item for @p kind (take_candidate_refs()).
         * @return False, holding nothing, when the item's types or references did not check out.
         * @note Main thread, outside the candidate mutex: a failed resources reference releases the shader's here.
         */
        [[nodiscard]] bool own_candidate(const ShaderItem &item, int kind, std::uint8_t priority,
                                         TwinCandidate &out) noexcept
        {
            const Ownership owned = take_candidate_refs(item);
            if (owned == Ownership::ShaderOnly)
            {
                // The level tables still hold the item, so this cannot delete the shader.
                release_candidate_shader(item.shader);
            }
            if (owned != Ownership::Owned)
            {
                return false;
            }
            out = TwinCandidate{
                .request = {item.shader, item.resources, kind}, .priority = priority, .first_ms = GetTickCount64()};
            return true;
        }

        /** @brief True for a plant or frame kind (TwinScope::ListedPlants or ListedFrames). */
        [[nodiscard]] bool listed_kind(int kind) noexcept
        {
            return kind >= 0 && kind < k_kinds && listed_scope(blobs::k_twins[kind].scope);
        }

        /**
         * @brief How a candidate of @p kind with @p priority ranks for a pool slot or a source slot: every plant or
         *        frame candidate above every character one, then by priority (a drawn item's, a character material's,
         *        a loaded material's).
         */
        [[nodiscard]] int candidate_rank(int kind, std::uint8_t priority) noexcept
        {
            return (listed_kind(kind) ? 4 : 0) + priority;
        }

        [[nodiscard]] int candidate_rank(const TwinCandidate &candidate) noexcept
        {
            return candidate_rank(candidate.request.kind, candidate.priority);
        }

        /**
         * @brief Calls @p fn with each kind @p item's shader is a source of for the twin scope bits @p scopes: a
         *        kind whose scope those bits name, or with @p character_shaders any Dissolving kind that is not
         *        transparent-only (the character's Eye and Hair shaders, whatever the asset), and that the item fits
         *        (item_fits_kind()).
         * @note Reads the shader's name (and, for a transparent-only kind, the item's opacity through its resources),
         *       nothing else. Without Rules::TWIN_CHARACTER and @p character_shaders, only listed kinds come out,
         *       whose fit needs no call.
         */
        template <typename Fn>
        void for_each_twin_kind(const ShaderItem &item, std::uintptr_t scopes, bool character_shaders, Fn &&fn)
        {
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const auto scope = blobs::k_twins[kind].scope;
                const bool character_shader = character_shaders && scope == blobs::TwinScope::Dissolving &&
                                              !blobs::k_twins[kind].transparent_only;
                const auto needed = scope == blobs::TwinScope::ListedPlants   ? k_scope_plants
                                    : scope == blobs::TwinScope::ListedFrames ? k_scope_frames
                                                                              : k_scope_character;
                if (((scopes & needed) != 0 || character_shader) && source_kind_guarded(item.shader, scope) == kind &&
                    item_fits_kind(kind, item))
                {
                    fn(kind);
                }
            }
        }

        /** @brief Validates the renderer, public flush slot, and engine main-thread ID before engine work. */
        [[nodiscard]] std::uintptr_t loading_renderer() noexcept
        {
            const auto renderer =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_RENDERER_OFFSET});
            if (!renderer || *renderer == 0)
            {
                return 0;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*renderer});
            if (!vtable || !vtable_is(GameClass::Renderer, *vtable) ||
                validated_slot(*renderer, Constants::RENDERER_FLUSH_RT_COMMANDS_VTABLE_OFFSET, s_flush_rt_commands) ==
                    0)
            {
                return 0;
            }
            const DWORD main_thread = engine_main_thread_id(*renderer);
            return main_thread != 0 && main_thread == GetCurrentThreadId() ? *renderer : 0;
        }

        [[nodiscard]] bool flush_loading_commands(std::uintptr_t renderer) noexcept
        {
            __try
            {
                return reinterpret_cast<FlushRTCommandsFn>(s_flush_rt_commands)(renderer, true, true, true);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        [[nodiscard]] std::string lower_text(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), ascii_lower);
            return text;
        }

        /**
         * @brief @p path in UTF-8, as the engine's paths and the log are, whatever the system code page; a profile
         *        folder named with letters outside that code page neither throws nor comes out garbled.
         */
        [[nodiscard]] std::string utf8_text(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return {text.begin(), text.end()};
        }

        /**
         * @brief Checks the stock shader binary @p file in ShadersBin.pak against the header CRC the twins were
         *        built from.
         * @return An empty string when it matches, else what differs.
         */
        [[nodiscard]] std::string check_stock_header(const PakCalls &calls, std::uintptr_t pak, std::string_view file,
                                                     std::uint32_t expected_crc)
        {
            const std::string path = std::string(Constants::SHADER_ENGINE_CACHE_DIR) + std::string(file);
            std::array<std::uint8_t, Constants::SHADER_BIN_HEADER_SIZE> header{};
            switch (read_pak_header_guarded(calls, pak, path.c_str(), header.data()))
            {
            case HeaderRead::Missing:
                return std::format("{} is not in the game's shader binaries", path);
            case HeaderRead::Fault:
                return std::format("reading {} through CryPak faulted", path);
            case HeaderRead::Read:
            default:
                break;
            }
            std::uint32_t magic = 0;
            std::uint32_t crc = 0;
            std::uint16_t version_low = 0;
            std::uint16_t version_high = 0;
            std::memcpy(&magic, header.data(), sizeof(magic));
            std::memcpy(&crc, header.data() + 4, sizeof(crc));
            std::memcpy(&version_low, header.data() + 8, sizeof(version_low));
            std::memcpy(&version_high, header.data() + 10, sizeof(version_high));
            if (magic != Constants::SHADER_BIN_MAGIC || version_low != Constants::SHADER_BIN_VERSION_LOW ||
                version_high != Constants::SHADER_BIN_VERSION_HIGH)
            {
                return std::format("{} is not an FXB0 3.4 shader binary (magic {:#010x}, version {}.{})", path, magic,
                                   version_low, version_high);
            }
            if (crc != expected_crc)
            {
                return std::format("the game's {} changed (header CRC {:#010x}, the twins were built from {:#010x}); "
                                   "rebuild the twins with scripts/build_shader_twins.py",
                                   path, crc, expected_crc);
            }
            return {};
        }

        /**
         * @brief Writes @p blob to @p path unless the file there already starts with exactly it.
         * @details After it parses a shader permutation the engine appends its parse cache (the local info: for each
         *          gen mask, the parameters, samplers, textures and functions the parse kept) to the binary in the user
         *          cache, past the string table the header points it to, and leaves the header and the tokens as they
         *          were. A file that holds the blob followed by that cache is kept, so a later session's twin parses
         *          with it: rewriting the file dropped it, and every twin instance was parsed from scratch again,
         *          which stalled the render thread about 190 ms each.
         * @param written Set when the file was (re)written.
         * @return An empty string on success, else what went wrong.
         */
        [[nodiscard]] std::string write_blob(const std::filesystem::path &path, const blobs::Blob &blob, bool &written)
        {
            written = false;
            std::error_code error;
            if (const std::uintmax_t size = std::filesystem::file_size(path, error); !error && size >= blob.size)
            {
                std::ifstream existing(path, std::ios::binary);
                std::vector<char> bytes(blob.size);
                if (existing.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) &&
                    std::equal(bytes.begin(), bytes.end(), blob.data,
                               [](char a, unsigned char b) { return static_cast<unsigned char>(a) == b; }))
                {
                    return {};
                }
            }
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return std::format("cannot create {}: {}", utf8_text(path.parent_path()), error.message());
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(blob.data), static_cast<std::streamsize>(blob.size));
            out.close();
            if (!out)
            {
                return std::format("cannot write {}", utf8_text(path));
            }
            written = true;
            return {};
        }

        /**
         * @brief The lower-case name of @p entry when it is a regular file whose name starts with blobs::k_file_prefix,
         *        else nothing.
         * @details The mod's files have printable ASCII names, so a name with any other character is not one of them,
         *          and the test reads the native wide name with no code-page conversion.
         */
        [[nodiscard]] std::optional<std::string> mod_file_name(const std::filesystem::directory_entry &entry)
        {
            std::error_code error;
            if (!entry.is_regular_file(error))
            {
                return std::nullopt;
            }
            const std::wstring wide = entry.path().filename().wstring();
            if (std::ranges::any_of(wide, [](wchar_t c) { return c < 0x20 || c > 0x7E; }))
            {
                return std::nullopt;
            }
            std::string name(wide.size(), '\0');
            std::ranges::transform(wide, name.begin(), [](wchar_t c) { return ascii_lower(static_cast<char>(c)); });
            if (!name.starts_with(blobs::k_file_prefix))
            {
                return std::nullopt;
            }
            return name;
        }

        /**
         * @brief Deletes the mod's shader binaries in @p directory that this build does not ship (another CRC in the
         *        name).
         * @details Only files of the directory itself that start with blobs::k_file_prefix and end in .cfxb or .cfib;
         *          the engine's compiled caches sit in subfolders and are left alone.
         * @return The number of files removed.
         */
        [[nodiscard]] int remove_stale_blobs(const std::filesystem::path &directory)
        {
            int removed = 0;
            std::error_code error;
            // Iterated with error codes, so a folder that cannot be read ends the sweep instead of throwing.
            for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
                 it.increment(error))
            {
                const std::optional<std::string> name = mod_file_name(*it);
                if (!name)
                {
                    continue;
                }
                const bool ours = name->ends_with(".cfxb") || name->ends_with(".cfib");
                const bool shipped = std::ranges::any_of(blobs::k_blobs, [&name](const blobs::Blob &blob)
                                                         { return *name == blob.file_name; });
                std::error_code remove_error;
                if (ours && !shipped && std::filesystem::remove(it->path(), remove_error))
                {
                    DMK::log().info("ShaderTwins: removed the stale twin file {}", utf8_text(it->path()));
                    ++removed;
                }
            }
            return removed;
        }

        /**
         * @brief Deletes the engine's compiled entries of the mod's twins that this build does not ship, in the
         *        compiled-shader folders under @p directory (Constants::SHADER_COMPILED_CACHE_DIRS).
         * @details An entry is "<shader name>@<entry>"; only one whose shader name starts with blobs::k_file_prefix
         *          and is no current twin's (another CRC: an older build of the mod, or a game patch the twins were
         *          rebuilt for) is deleted. Those names are never loaded again, so their compiled permutations only
         *          take room. An entry the engine still holds open (a twin of an older build loaded earlier in this
         *          session) cannot be deleted and is left for the next start.
         * @param stale_twins Set to the distinct twin names removed, comma-separated, for the log.
         * @return The number of entries removed.
         */
        [[nodiscard]] int remove_stale_compiled(const std::filesystem::path &directory, std::string &stale_twins)
        {
            std::array<std::string, k_kinds> current{};
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                current[static_cast<std::size_t>(kind)] = lower_text(std::string(blobs::k_twins[kind].twin_name));
            }
            int removed = 0;
            for (const std::string_view folder : Constants::SHADER_COMPILED_CACHE_DIRS)
            {
                // Iterated with error codes, so a folder that cannot be read is skipped instead of throwing.
                std::error_code error;
                for (std::filesystem::directory_iterator it(directory / folder, error), end; !error && it != end;
                     it.increment(error))
                {
                    const std::optional<std::string> name = mod_file_name(*it);
                    const std::size_t at = name ? name->find('@') : std::string::npos;
                    if (at == std::string::npos)
                    {
                        continue;
                    }
                    const std::string twin = name->substr(0, at);
                    std::error_code remove_error;
                    if (std::ranges::find(current, twin) != current.end() ||
                        !std::filesystem::remove(it->path(), remove_error))
                    {
                        continue;
                    }
                    ++removed;
                    if (stale_twins.find(twin) == std::string::npos)
                    {
                        stale_twins += std::format("{}{}", stale_twins.empty() ? "" : ", ", twin);
                    }
                }
            }
            return removed;
        }

        /**
         * @brief Reads the dissolve's RT-mask bit (the DissolveRtBit global) into s_dissolve_rt_bit, which the sets
         *        built ahead for the fade add to their RT mask.
         * @details The renderer fills the global at startup, long before a third-person frame. It must be one bit of
         *          the low 32 (AddRenderObject ORs its low dword into the render object's 32-bit mask); anything else,
         *          or no global, leaves it 0, and the sets built ahead then carry the dissolve in their flags only: the
         *          stages add the same bit for FOB_DISSOLVE, so the PSOs they ask for are the same, and only the
         *          resources' local PSO cache keys them apart from the fading build's.
         * @note Main thread, before any set is built ahead.
         */
        void read_dissolve_rt_bit() noexcept
        {
            if (s_dissolve_rt_global == 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "ShaderTwins: the dissolve's RT-mask bit did not resolve; the character twins' "
                    "sets for the fade are built ahead with the dissolve in their flags only");
                return;
            }
            const auto bit = DMK::memory::read<std::uint64_t>(DMK::Address{s_dissolve_rt_global});
            if (!bit || *bit == 0 || *bit > 0xFFFFFFFFull || (*bit & (*bit - 1)) != 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "ShaderTwins: the dissolve's RT-mask bit at {:#x} reads {:#x}, not one bit of the low 32; the "
                    "character twins' sets for the fade are built ahead with the dissolve in their flags only",
                    s_dissolve_rt_global, bit ? *bit : 0);
                return;
            }
            s_dissolve_rt_bit.store(*bit, std::memory_order_relaxed);
            (void)DMK::log().try_log(
                DMK::LogLevel::Trace,
                "ShaderTwins: the character twins' sets for the fade are built ahead with FOB_DISSOLVE {:#x} and "
                "FOB_DISSOLVE_OUT {:#x} in the flags and the dissolve bit {:#x} in the RT mask",
                Constants::RENDER_OBJECT_FLAG_DISSOLVE, Constants::RENDER_OBJECT_FLAG_DISSOLVE_OUT, *bit);
        }

        /**
         * @brief Proves the engine interfaces, checks the stock shaders the twins were built from, and writes the
         *        twins' files to the user shader cache.
         * @return An empty string on success, else why the feature cannot run.
         * @throws std::bad_alloc, std::filesystem::filesystem_error on allocation or file-system failure.
         */
        [[nodiscard]] std::string prepare_engine(std::uintptr_t pak, std::uintptr_t renderer)
        {
            DMK_PROFILE_SCOPE("twins.load_prepare");
            DMK::Logger &logger = DMK::log();
            const auto renderer_vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{renderer});
            if (!renderer_vtable || !vtable_is(GameClass::Renderer, *renderer_vtable))
            {
                return "g_env->pRenderer is not a CD3D9Renderer";
            }
            const std::uintptr_t manager = renderer + Constants::RENDERER_SHADER_MAN_OFFSET;
            std::array<char, 32> shaders_path{};
            copy_text_field_guarded(manager, Constants::SHADER_MAN_SHADERS_PATH_OFFSET, shaders_path.data(),
                                    shaders_path.size());
            if (std::string_view{shaders_path.data()} != Constants::SHADER_MAN_SHADERS_PATH)
            {
                return std::format("the renderer's shader manager did not check out (m_ShadersPath \"{}\")",
                                   shaders_path.data());
            }
            if (!object_has_type(pak, Constants::CRY_PAK_RTTI_NAME))
            {
                return "g_env->pCryPak is not a CCryPak";
            }
            const std::uintptr_t adjust =
                validated_slot(pak, Constants::CRY_PAK_VTABLE_ADJUST_FILE_NAME_OFFSET, s_adjust_file_name);
            const PakCalls calls{
                .open = image_slot(pak, s_pak_open_slot),
                .read = image_slot(pak, s_pak_read_slot),
                .close = image_slot(pak, s_pak_close_slot),
            };
            if (adjust == 0 || calls.open == 0 || calls.read == 0 || calls.close == 0)
            {
                return "a CryPak vtable slot did not check out (AdjustFileName, FOpen, FReadRaw or FClose)";
            }

            // The generated header must ship every twin's binary.
            for (const blobs::Twin &twin : blobs::k_twins)
            {
                const std::string twin_file = lower_text(std::string(twin.twin_name)) + ".cfxb";
                if (std::ranges::none_of(blobs::k_blobs,
                                         [&twin_file](const blobs::Blob &blob) { return twin_file == blob.file_name; }))
                {
                    return std::format("the generated shader header has no {}", twin_file);
                }
            }

            // Every plant and frame twin copies commonzpass.cfib and commonzprepass.cfib, and every twin its own stock
            // shader. A game patch that changed one of them would leave a twin out of step with the gen flags it
            // borrows: a changed Z pass turns the plant and frame twins off, and a changed stock shader only its own
            // twin, whose items keep the stock shader. The character twins keep the stock includes, which their own
            // source CRCs cover.
            const std::array<std::pair<std::string_view, std::uint32_t>, 2> passes{{
                {"commonzpass.cfib", blobs::k_source_commonzpass_crc},
                {"commonzprepass.cfib", blobs::k_source_commonzprepass_crc},
            }};
            for (const auto &[file, crc] : passes)
            {
                if (std::string error = check_stock_header(calls, pak, file, crc); !error.empty())
                {
                    for (int kind = 0; kind < k_kinds; ++kind)
                    {
                        if (listed_scope(blobs::k_twins[kind].scope))
                        {
                            s_kind_failed[static_cast<std::size_t>(kind)] = true;
                        }
                    }
                    logger.warning("ShaderTwins: {}; the plants and frames keep the stock shaders (no see-through, no "
                                   "near cut)",
                                   error);
                }
            }
            std::string twin_names;
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                const blobs::Twin &twin = blobs::k_twins[kind];
                if (s_kind_failed[static_cast<std::size_t>(kind)])
                {
                    continue;
                }
                if (std::string error = check_stock_header(calls, pak, twin.source_file, twin.source_crc);
                    !error.empty())
                {
                    s_kind_failed[static_cast<std::size_t>(kind)] = true;
                    logger.warning("ShaderTwins: {}; the {} twin is off and its items keep the stock shader", error,
                                   twin.label);
                    continue;
                }
                twin_names += std::format("{}{} ({})", twin_names.empty() ? "" : ", ", twin.twin_name, twin.label);
            }
            if (twin_names.empty())
            {
                return "no stock shader the twins were built from matches the game's";
            }

            // The user shader cache, through the engine's %USER% alias, so a relocated Saved Games folder works.
            std::filesystem::path directory;
            int written_count = 0;
            for (const blobs::Blob &blob : blobs::k_blobs)
            {
                const std::string source = std::string(Constants::SHADER_USER_CACHE_DIR) + blob.file_name;
                std::array<char, Constants::CRY_PAK_MAX_PATH> resolved{};
                if (!adjust_file_name_guarded(adjust, pak, source.c_str(), resolved.data(),
                                              Constants::CRY_PAK_FLAGS_PATH_REAL |
                                                  Constants::CRY_PAK_FLAGS_FOR_WRITING))
                {
                    return std::format("{} did not resolve (CryPak AdjustFileName)", source);
                }
                resolved.back() = '\0';
                // CryPak resolves %USER% to UTF-8 (the known folder through its UTF-8 encoder) and opens files by
                // converting UTF-8 to wide, so the path is read as UTF-8, not in the system code page.
                const std::filesystem::path path{
                    std::u8string_view{reinterpret_cast<const char8_t *>(resolved.data())}};
                bool written = false;
                if (std::string error = write_blob(path, blob, written); !error.empty())
                {
                    return error;
                }
                if (written)
                {
                    ++written_count;
                    logger.info("ShaderTwins: wrote {} ({} bytes)", utf8_text(path), blob.size);
                }
                directory = path.parent_path();
            }
            const int removed = remove_stale_blobs(directory);
            // The compiled permutations of the twins an older build shipped: before any twin of this build is created,
            // so none of its own entries is open yet.
            std::string stale_twins;
            const int removed_compiled = remove_stale_compiled(directory, stale_twins);
            if (removed_compiled != 0)
            {
                logger.info("ShaderTwins: removed {} compiled shader entr{} of twin(s) this build does not ship ({})",
                            removed_compiled, removed_compiled == 1 ? "y" : "ies", stale_twins);
            }

            s_shader_manager = manager;
            logger.info(
                "ShaderTwins: {} twin file(s) in {} ({} written, {} stale removed); twins {}, whose stock shaders "
                "(and Z passes, for the plants) match the game's",
                std::size(blobs::k_blobs), utf8_text(directory), written_count, removed, twin_names);
            read_dissolve_rt_bit();
            return {};
        }

        /** @brief The source index of @p shader as a source of @p kind, or -1. */
        [[nodiscard]] int find_source(std::uintptr_t shader, int kind) noexcept
        {
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                if (source.shader == shader && source.kind == kind)
                {
                    return i;
                }
            }
            return -1;
        }

        /** @brief Drops the records of @p node, whose compiled objects are about to be rebuilt. */
        void drop_records(std::uintptr_t node) noexcept
        {
            int kept = 0;
            for (int i = 0; i < s_pending_count; ++i)
            {
                const PendingPsos &pending = s_pending[static_cast<std::size_t>(i)];
                if (pending.record.node == node)
                {
                    trace_event(trace::Event::RecordDropped, node, pending.record.object, pending.record.twin_shader, 0,
                                0, pending.first_ms);
                    drop_record(pending.record);
                    continue;
                }
                if (kept != i)
                {
                    s_pending[static_cast<std::size_t>(kept)] = pending;
                }
                ++kept;
            }
            s_pending_count = kept;
        }

        /** @brief Turns the feature off for the session, swaps every node back and says why, once. */
        void turn_off(std::uint64_t now, const char *why) noexcept
        {
            if (s_disabled.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            s_eye_ready.store(false, std::memory_order_release);
            s_character_ready.store(false, std::memory_order_release);
            release_all(now, "feature off");
            (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                     "ShaderTwins: {}; trees and the character keep the stock shaders (no see-through, "
                                     "no character fade twins) for the session",
                                     why);
        }

        /** @brief True while a plant or a frame kind has not failed, so the level inventory has pairs to queue. */
        [[nodiscard]] bool listed_scopes_usable() noexcept
        {
            return scope_usable(blobs::TwinScope::ListedPlants) || scope_usable(blobs::TwinScope::ListedFrames);
        }

        /**
         * @brief True for a candidate's kind that gets no twin (s_kind_failed, or no kind at all): it is never queued
         *        from the level inventory nor created during a pause, and at a loading boundary it ranks below every
         *        other candidate.
         * @note Main thread: s_kind_failed is written only there.
         */
        [[nodiscard]] bool kind_off(int kind) noexcept
        {
            return kind < 0 || kind >= k_kinds || s_kind_failed[static_cast<std::size_t>(kind)];
        }

        /**
         * @brief Prepares the twins inside a verified loading transaction, once the engine is up.
         * @return True once prepared; false while waiting for the engine (retried every
         *         SHADER_TWIN_PREPARE_RETRY_MS) or after a failure (the feature is then off).
         */
        [[nodiscard]] bool prepare(std::uint64_t now) noexcept
        {
            if (s_prepare != Prepare::Pending)
            {
                return s_prepare == Prepare::Ready;
            }
            if (now < s_prepare_retry_ms)
            {
                return false;
            }
            s_prepare_retry_ms = now + Constants::SHADER_TWIN_PREPARE_RETRY_MS;
            const auto pak = DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_CRY_PAK_OFFSET});
            const auto renderer =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_RENDERER_OFFSET});
            if (!pak || !renderer || *pak == 0 || *renderer == 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: waiting for g_env's CryPak and renderer");
                return false;
            }
            std::string error;
            try
            {
                error = prepare_engine(*pak, *renderer);
            }
            catch (const std::exception &e)
            {
                error = std::string("preparing the twins threw: ") + e.what();
            }
            catch (...)
            {
                error = "preparing the twins threw";
            }
            if (!error.empty())
            {
                s_prepare = Prepare::Failed;
                turn_off(now, error.c_str());
                return false;
            }
            s_prepare = Prepare::Ready;
            return true;
        }

        /**
         * @brief Creates an owned candidate's twin inside the loading transaction, or in a frame the game stands still
         *        in (create_during_pause()).
         * @details The twin is created the way the material path creates its source (EF_LoadShaderItem -> mfForName):
         *          flags 0, the material's CShaderResources and the source's gen mask. A request whose shader or
         *          resources are no longer what they were takes no slot, so a live shader at that address later still
         *          gets its twin. A source that takes a slot keeps one more reference for the session, whatever becomes
         *          of its twin, so its address can never be reused by another shader while the swap set names it, and
         *          the twin's reference is never released, because persistent render objects name it by id and pooled
         *          compiled objects keep pointers to it. A source of a failed kind (s_kind_failed) keeps its stock
         *          shader. One shader instance can be the source of a kind of each scope (an Illum instance of a plant
         *          twin and of the IllumFade twin), each with an entry of its own.
         */
        void create_twin(const TwinRequest &request, std::uint64_t now) noexcept
        {
            // Only finalize_loading(), or create_during_pause() in a frame the game stands still in, may create a
            // shader, on the verified engine main thread. In particular a retained gameplay request is not a permit,
            // and a source still parsing must not reserve a Failed entry permanently.
            if ((!s_loading_finalizing.load(std::memory_order_acquire) && !s_pause_permit) || loading_renderer() == 0)
            {
                return;
            }
            if (request.kind < 0 || request.kind >= k_kinds || find_source(request.shader, request.kind) >= 0)
            {
                return;
            }
            DMK_PROFILE_SCOPE("twins.create");
            if (s_source_count >= k_max_sources)
            {
                if (!s_sources_full_logged)
                {
                    s_sources_full_logged = true;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} source shader instances have twins already; items with "
                                             "another material keep the stock shader",
                                             k_max_sources);
                }
                return;
            }
            decltype(Source::name) name{};
            copy_text_field_guarded(request.shader, Constants::SHADER_RESOURCE_NAME_OFFSET, name.data(), name.size());

            const auto flags =
                DMK::memory::read<std::uint32_t>(DMK::Address{request.shader + Constants::SHADER_FLAGS_OFFSET});
            const auto mask =
                DMK::memory::read<std::uint64_t>(DMK::Address{request.shader + Constants::SHADER_GEN_MASK_OFFSET});
            const auto techniques = DMK::memory::read<std::uint32_t>(
                DMK::Address{request.shader + Constants::SHADER_TECHNIQUE_COUNT_OFFSET});
            if (!flags || (*flags & Constants::SHADER_FLAG_LOADED) == 0 ||
                (*flags & Constants::SHADER_FLAG_NOT_FOUND) != 0 || !mask || !techniques || *techniques == 0)
            {
                return;
            }
            // The candidate owns both pointers; the source table takes its own shader reference before it can ever
            // publish the identity. Resource identity is checked again before mfForName uses its texture flags.
            const int kind = is_shader(request.shader)
                                 ? source_kind_guarded(request.shader, blobs::k_twins[request.kind].scope)
                                 : -1;
            if (kind != request.kind || (request.resources != 0 && !is_shader_resources(request.resources)) ||
                !add_shader_ref_guarded(request.shader))
            {
                ++s_stale_requests;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: shader {:#x} ({}) is no longer a source instance with its "
                                         "resources; no twin",
                                         request.shader, name.data());
                return;
            }
            Source &source = s_sources[static_cast<std::size_t>(s_source_count++)];
            source = Source{
                .shader = request.shader, .kind = kind, .created_ms = now, .state = TwinState::Failed, .name = name};
            const blobs::Twin &twin = blobs::k_twins[kind];
            if (s_kind_failed[static_cast<std::size_t>(kind)])
            {
                return;
            }
            source.mask = *mask;
            source.techniques = *techniques;
            trace_event(trace::Event::SourceRequest, 0, 0, source.shader, request.resources, source.mask, kind);
            source.twin =
                for_name_guarded(s_for_name, s_shader_manager, twin.twin_name, request.resources, source.mask);
            trace_event(trace::Event::SourceResult, 0, source.twin, source.shader, request.resources, source.mask,
                        kind);
            if (source.twin == 0)
            {
                s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                (void)DMK::log().try_log(DMK::LogLevel::Warning, "ShaderTwins: mfForName({}) failed for {}; no twin",
                                         twin.twin_name, source.name.data());
                return;
            }
            source.state = TwinState::Pending;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: {} twin {:#x} requested for {} (CShader {:#x}, gen mask {:#x}, "
                                     "{} techniques, resources {:#x})",
                                     twin.label, source.twin, source.name.data(), source.shader, source.mask,
                                     source.techniques, request.resources);
        }

        struct CachedTwin
        {
            std::uintptr_t shader = 0;
            int kind = 0;
            std::array<char, 128> name{};
        };

        /** @brief Under the engine's shared resource lock, retain only loaded current-version twin permutations. */
        [[nodiscard]] bool snapshot_twins_guarded(CachedTwin *entries, int &count) noexcept
        {
            __try
            {
                const auto container = *reinterpret_cast<const std::uintptr_t *>(s_shader_container_global);
                if (container == 0)
                {
                    return true;
                }
                const auto begin =
                    *reinterpret_cast<const std::uintptr_t *>(container + Constants::RESOURCE_CONTAINER_BEGIN_OFFSET);
                const auto end =
                    *reinterpret_cast<const std::uintptr_t *>(container + Constants::RESOURCE_CONTAINER_END_OFFSET);
                if (end < begin || (end - begin) % sizeof(std::uintptr_t) != 0 || (begin == 0 && end != 0))
                {
                    return false;
                }
                // A persistent render-item key has 11 shader-ID bits; vector index is ID minus one.
                const auto size = std::min<std::size_t>((end - begin) / sizeof(std::uintptr_t), 2047);
                for (std::size_t i = 0; i < size && count < k_max_sources; ++i)
                {
                    const auto base = reinterpret_cast<const std::uintptr_t *>(begin)[i];
                    if (base < Constants::SHADER_BASE_RESOURCE_OFFSET)
                    {
                        continue;
                    }
                    const auto shader = base - Constants::SHADER_BASE_RESOURCE_OFFSET;
                    const auto flags =
                        *reinterpret_cast<const std::uint32_t *>(shader + Constants::SHADER_FLAGS_OFFSET);
                    if ((flags & Constants::SHADER_FLAG_LOADED) == 0 || (flags & Constants::SHADER_FLAG_NOT_FOUND) != 0)
                    {
                        continue;
                    }
                    const char *name =
                        *reinterpret_cast<const char *const *>(shader + Constants::SHADER_RESOURCE_NAME_OFFSET);
                    if (name == nullptr)
                    {
                        continue;
                    }
                    for (int kind = 0; kind < k_kinds; ++kind)
                    {
                        const auto &twin = blobs::k_twins[kind];
                        if (s_kind_failed[static_cast<std::size_t>(kind)] || !starts_with_nocase(name, twin.twin_name))
                        {
                            continue;
                        }
                        CachedTwin &entry = entries[count];
                        std::size_t length = 0;
                        for (; length + 1 < entry.name.size() && name[length] != '\0'; ++length)
                        {
                            entry.name[length] = name[length];
                        }
                        entry.name[length] = '\0';
                        if (name[length] != '\0')
                        {
                            continue;
                        }
                        const char *suffix = entry.name.data() + std::strlen(twin.twin_name);
                        const char *platform_end = std::strchr(suffix, ')');
                        const char *mask_end = platform_end != nullptr ? std::strchr(platform_end + 1, ')') : nullptr;
                        if (*suffix != '(' || platform_end == nullptr || platform_end[1] != '(' ||
                            mask_end == nullptr || mask_end[1] != '\0')
                        {
                            continue; // A template name has no permutation mask and must never be recovered.
                        }
                        const auto add_ref = image_slot(base, Constants::BASE_RESOURCE_VTABLE_ADD_REF_OFFSET);
                        if (add_ref != 0 && resource_ref_guarded(add_ref, base))
                        {
                            entry.shader = shader;
                            entry.kind = kind;
                            ++count;
                        }
                        break;
                    }
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        [[nodiscard]] std::uintptr_t lookup_cached_shader_guarded(const ResourceName &name) noexcept
        {
            __try
            {
                const auto base = s_resource_lookup(&Constants::CSHADER_RESOURCE_CLASS_CRC, &name, true);
                return base >= Constants::SHADER_BASE_RESOURCE_OFFSET ? base - Constants::SHADER_BASE_RESOURCE_OFFSET
                                                                      : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief Owns a detached candidate batch, including every early-return/exception path. */
        struct CandidateBatch
        {
            std::array<TwinCandidate, k_max_candidates> entries{};
            std::size_t count = 0;
            ~CandidateBatch() noexcept
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    drop_candidate(entries[i]);
                }
            }
        };

        /** @brief Owns one loading epoch's level-inventory candidates, outside the 512-entry pool, on every path. */
        struct InventoryBatch
        {
            std::vector<TwinCandidate> entries;
            ~InventoryBatch() noexcept
            {
                for (TwinCandidate &entry : entries)
                {
                    drop_candidate(entry);
                }
            }
        };

        /** @brief What one loading epoch's level inventory found and queued, for its log lines. */
        struct InventoryOutcome
        {
            bool ran = false;        // the inventory is on and was taken
            bool failed = false;     // its lists could not allocate; the capture-only twins go on
            bool listed_off = false; // not taken: every plant and frame kind is off
            TwinInventoryStats walk{};
            int plant_pairs = 0; // distinct listed pairs found, by scope
            int frame_pairs = 0;
            int off_pairs = 0; // of them, of a kind that is off (kind_off()), neither queued nor counted for coverage
            int existing = 0;  // of them, already a source
            int captured = 0;  // already a candidate this load captured
            int queued = 0;    // queued as an inventory candidate
            int rejected = 0;  // whose shader or resources did not check out
        };

        /** @brief True when @p batch holds a candidate of @p shader for @p kind. */
        [[nodiscard]] bool batch_holds(const CandidateBatch &batch, std::uintptr_t shader, int kind) noexcept
        {
            for (std::size_t i = 0; i < batch.count; ++i)
            {
                const TwinRequest &request = batch.entries[i].request;
                if (request.shader == shader && request.kind == kind)
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief A listed pair the level inventory found, with the resources of the first material found with it. */
        struct InventoryFound
        {
            InventoryKey key{};
            std::uintptr_t resources = 0;
        };

        /**
         * @brief Walks the level tables (hooks/twin_inventory.hpp) and puts in @p found, sorted and once each, every
         *        listed (shader, kind) pair their drawn materials name, counted by scope in the outcome it returns.
         * @details The walk calls no engine code. Each drawn material's shader item is read the way a captured one is
         *          (load_material_shader_detour()) and goes through the same kind test for the scope its asset path
         *          gives it, the character's kinds excepted; a pair keeps the resources of the first material found
         *          with it.
         * @return The outcome, not run (and @p found empty) when the inventory is off, or when every plant and frame
         *         kind is off (its pairs could only take source slots with no twin).
         * @throws std::bad_alloc If the lists cannot allocate.
         * @note Main thread, inside finalize_loading() after the first render-command barrier, or in a frame the game
         *       stands still in (pause_inventory()).
         */
        [[nodiscard]] InventoryOutcome walk_inventory(std::vector<InventoryFound> &found)
        {
            InventoryOutcome outcome{};
            if (s_inventory_objman_global == 0)
            {
                return outcome;
            }
            if (!listed_scopes_usable())
            {
                outcome.listed_off = true;
                return outcome;
            }
            outcome.ran = true;
            std::vector<TwinInventoryMaterial> materials;
            materials.reserve(4096);
            outcome.walk = enumerate_level_twin_materials(s_genv, s_inventory_objman_global, materials);
            found.reserve(512);
            for (const TwinInventoryMaterial &drawn : materials)
            {
                ShaderItem item{};
                if (!DMK::memory::read_into(DMK::Address{drawn.material + Constants::MATERIAL_SHADER_ITEM_OFFSET},
                                            std::as_writable_bytes(std::span{&item, 1})) ||
                    item.shader == 0)
                {
                    continue;
                }
                for_each_twin_kind(
                    item, drawn.scopes, false, [&found, &item](int kind)
                    { found.push_back(InventoryFound{.key = {item.shader, kind}, .resources = item.resources}); });
            }
            // One entry per pair, the first material's resources kept.
            std::stable_sort(found.begin(), found.end(),
                             [](const InventoryFound &a, const InventoryFound &b) { return a.key < b.key; });
            found.erase(std::unique(found.begin(), found.end(),
                                    [](const InventoryFound &a, const InventoryFound &b) { return a.key == b.key; }),
                        found.end());
            for (const InventoryFound &pair : found)
            {
                const bool frame = blobs::k_twins[pair.key.kind].scope == blobs::TwinScope::ListedFrames;
                ++(frame ? outcome.frame_pairs : outcome.plant_pairs);
            }
            return outcome;
        }

        /**
         * @brief Queues in @p inventory an owned candidate for every pair of @p found that is neither a source nor one
         *        @p held(shader, kind) says is a candidate already, and makes s_inventory_pairs the pairs found.
         * @details Nothing is dropped for room: the inventory holds the level's own pairs, outside the 512-entry pool.
         *          A pair of a kind that is off (kind_off()) is left out of both: it would take a source slot with no
         *          twin, ahead of the character's, and its plants keep the stock shader anyway.
         * @throws std::bad_alloc If the lists cannot allocate (what was queued so far stays owned by @p inventory).
         * @note Main thread, under s_main_mutex.
         */
        template <typename Held>
        void queue_inventory(const std::vector<InventoryFound> &found, Held &&held, InventoryBatch &inventory,
                             InventoryOutcome &outcome)
        {
            std::vector<InventoryKey> pairs;
            pairs.reserve(found.size());
            for (const InventoryFound &pair : found)
            {
                if (!kind_off(pair.key.kind))
                {
                    pairs.push_back(pair.key);
                }
            }
            // Room for every pair first, so no owned candidate can be lost to a failed push.
            inventory.entries.reserve(inventory.entries.size() + found.size());
            for (const InventoryFound &pair : found)
            {
                if (kind_off(pair.key.kind))
                {
                    ++outcome.off_pairs;
                    continue;
                }
                if (find_source(pair.key.shader, pair.key.kind) >= 0)
                {
                    ++outcome.existing;
                    continue;
                }
                if (held(pair.key.shader, pair.key.kind))
                {
                    ++outcome.captured;
                    continue;
                }
                ShaderItem item{};
                item.shader = pair.key.shader;
                item.resources = pair.resources;
                TwinCandidate candidate{};
                if (!own_candidate(item, pair.key.kind, 0, candidate))
                {
                    ++outcome.rejected;
                    continue;
                }
                inventory.entries.push_back(candidate);
                ++outcome.queued;
            }
            s_inventory_pairs = std::move(pairs);
        }

        /**
         * @brief Takes the level inventory (hooks/twin_inventory.hpp) and queues in @p inventory, outside the 512-entry
         *        batch, an owned candidate for every listed (shader, kind) pair it found that is neither a source nor a
         *        candidate @p batch captured; s_inventory_pairs becomes the pairs it found.
         * @details The walk reads the level tables with no mod mutex held (walk_inventory()), then the pairs are queued
         *          under s_main_mutex (queue_inventory()). A pair whose stock shader still parses moves on to the pause
         *          inventory (finalize_loading()). An inventory not taken leaves no pairs of an earlier load behind.
         * @throws std::bad_alloc If the inventory's lists cannot allocate (the caller goes on with the captured ones).
         * @note Main thread, inside finalize_loading() after the first render-command barrier.
         */
        [[nodiscard]] InventoryOutcome take_inventory(const CandidateBatch &batch, InventoryBatch &inventory)
        {
            std::vector<InventoryFound> found;
            InventoryOutcome outcome = walk_inventory(found);
            const std::lock_guard lock(s_main_mutex);
            if (!outcome.ran)
            {
                s_inventory_pairs.clear();
                return outcome;
            }
            queue_inventory(
                found, [&batch](std::uintptr_t shader, int kind) { return batch_holds(batch, shader, kind); },
                inventory, outcome);
            return outcome;
        }

        /** @brief The level inventory's line for the epoch log: what the walk read and what it queued. */
        void log_inventory(const InventoryOutcome &outcome) noexcept
        {
            if (outcome.listed_off)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info, "ShaderTwins: level inventory not taken: every plant and "
                                                              "frame twin kind is off, so trees and frames keep the "
                                                              "stock shader");
            }
            if (!outcome.ran)
            {
                return;
            }
            const TwinInventoryStats &walk = outcome.walk;
            const double ms = static_cast<double>(walk.micros) / 1000.0;
            if (outcome.failed)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: level inventory could not allocate its lists; this load "
                                         "prepares only the captured twins");
                return;
            }
            if (!walk.level)
            {
                (void)DMK::log().try_log(walk.faults != 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                                         "ShaderTwins: level inventory: {} ({:.1f} ms); this load prepares only the "
                                         "captured twins",
                                         walk.reason, ms);
                return;
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "ShaderTwins: level inventory in {:.1f} ms: {} vegetation group(s), {} statobj(s) with {} compound "
                "part(s), {} brush material(s); {} plant or frame asset(s), {} distinct material(s), {} drawn -> {} "
                "plant + {} frame pair(s): {} of a twin kind that is off, {} with a twin source already, {} captured "
                "by this load, {} queued, {} rejected",
                ms, walk.groups, walk.statobjs, walk.children, walk.brush_materials, walk.scoped_assets,
                walk.distinct_materials, walk.drawn_materials, outcome.plant_pairs, outcome.frame_pairs,
                outcome.off_pairs, outcome.existing, outcome.captured, outcome.queued, outcome.rejected);
            if (walk.faults != 0 || walk.not_materials != 0 || walk.skipped_submaterials != 0 || walk.dedupe_full)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: level inventory: {} faulted read(s) or tables that did not "
                                         "check out{}{}, {} pointer(s) that are not a CMatInfo, {} sub-material(s) "
                                         "past the {} read per material{}; pairs behind them come from captures only",
                                         walk.faults, walk.reason[0] != '\0' ? "; first: " : "", walk.reason,
                                         walk.not_materials, walk.skipped_submaterials,
                                         Constants::TWIN_INVENTORY_MAX_SUBMATERIALS,
                                         walk.dedupe_full ? "; the dedupe set filled" : "");
            }
        }

        /** @brief Where the level inventory stands on a render miss of a listed node (classify_render_miss()). */
        enum class MissScope : std::uint8_t
        {
            NotChecked,  // not a listed node's render miss, or no inventory found a level
            Inventoried, // the inventory found its pair
            OtherScope,  // the inventory found its shader only for the other listed scope
            NotInLevel,  // no level table holds a material of a plant or frame asset with it
        };

        /**
         * @brief Whether a render miss of a listed node is a pair the last level inventory found. A miss outside it
         *        has a material scoped otherwise than its node (an override material named outside the plant folders,
         *        a plant shader on a frame) or one the level tables do not hold (an entity's, content spawned later).
         * @note Main thread, under s_main_mutex. Compares identities only.
         */
        [[nodiscard]] MissScope classify_render_miss(const TwinCandidate &candidate) noexcept
        {
            const int kind = candidate.request.kind;
            // A kind that is off gets no twin wherever its pair is (the inventory leaves its pairs out).
            if (s_inventory_pairs.empty() || candidate.priority != 2 || kind_off(kind) ||
                !listed_scope(blobs::k_twins[kind].scope))
            {
                return MissScope::NotChecked;
            }
            const auto found = [](std::uintptr_t shader, int of)
            { return std::ranges::binary_search(s_inventory_pairs, InventoryKey{shader, of}); };
            if (found(candidate.request.shader, kind))
            {
                return MissScope::Inventoried;
            }
            for (int other = 0; other < k_kinds; ++other)
            {
                if (other != kind && listed_scope(blobs::k_twins[other].scope) &&
                    found(candidate.request.shader, other))
                {
                    return MissScope::OtherScope;
                }
            }
            return MissScope::NotInLevel;
        }

        /** @brief A render miss outside the level inventory, copied for a log line. */
        struct MissReport
        {
            int kind = 0;
            MissScope scope = MissScope::NotChecked;
            std::uint64_t mask = 0;
            std::array<char, 64> name{};
        };

        /** @brief Copies @p candidate's identity for log_render_miss() while its references are held. */
        [[nodiscard]] MissReport miss_report(const TwinCandidate &candidate, MissScope scope) noexcept
        {
            MissReport report{.kind = candidate.request.kind, .scope = scope};
            copy_text_field_guarded(candidate.request.shader, Constants::SHADER_RESOURCE_NAME_OFFSET,
                                    report.name.data(), report.name.size());
            report.mask = DMK::memory::read<std::uint64_t>(
                              DMK::Address{candidate.request.shader + Constants::SHADER_GEN_MASK_OFFSET})
                              .value_or(0);
            return report;
        }

        /**
         * @brief Logs a render miss outside the level inventory by name and gen mask: a variant whose twin only its
         *        render miss prepares, @p at_boundary this loading boundary, else the next pause or boundary.
         */
        void log_render_miss(const MissReport &report, bool at_boundary) noexcept
        {
            const blobs::Twin &twin = blobs::k_twins[report.kind];
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "ShaderTwins: a listed {} draws {} {} (gen mask {:#x}), which the level inventory {}; its render miss "
                "prepares its twin {}",
                twin.scope == blobs::TwinScope::ListedFrames ? "frame" : "plant", twin.label, report.name.data(),
                report.mask,
                report.scope == MissScope::OtherScope
                    ? "found only for the other listed scope (a material named outside its node's folders)"
                    : "did not find in the level tables (an entity's or override material, or content loaded "
                      "after the boundary)",
                at_boundary                                       ? "at this loading boundary"
                : s_pause_service.load(std::memory_order_relaxed) ? "at the next pause or loading boundary"
                                                                  : "at the next loading boundary");
        }

        /**
         * @brief Logs, once each, the render misses of @p batch outside the level inventory just taken.
         * @return How many there were.
         * @note Main thread, under s_main_mutex, inside finalize_loading() while the batch holds its references.
         */
        int report_boundary_misses(CandidateBatch &batch) noexcept
        {
            int outside = 0;
            for (std::size_t i = 0; i < batch.count; ++i)
            {
                TwinCandidate &candidate = batch.entries[i];
                if (candidate.request.shader == 0 || candidate.priority != 2 || candidate.reported)
                {
                    continue;
                }
                const MissScope scope = classify_render_miss(candidate);
                candidate.reported = scope != MissScope::NotChecked;
                if (scope == MissScope::OtherScope || scope == MissScope::NotInLevel)
                {
                    ++outside;
                    log_render_miss(miss_report(candidate, scope), true);
                }
            }
            return outside;
        }

        /** @brief The level inventory's pairs that have a ready twin after a loading epoch, by scope. */
        struct Coverage
        {
            int plant = 0;
            int plant_ready = 0;
            int frame = 0;
            int frame_ready = 0;
            int missing = 0;
            std::array<char, 640> missing_text{};
        };

        /**
         * @brief Counts the last level inventory's pairs that have a ready twin, and names the others (their kind,
         *        name with gen mask, and why: no source, still parsing, failed, stale, or their kind off), each also in
         *        a Debug line.
         * @note Main thread, under s_main_mutex, inside finalize_loading() while the inventory, the batch and the
         *       source table hold every pair's shader.
         */
        [[nodiscard]] Coverage compute_coverage() noexcept
        {
            Coverage coverage{};
            std::size_t used = 0;
            for (const InventoryKey &pair : s_inventory_pairs)
            {
                const blobs::Twin &twin = blobs::k_twins[pair.kind];
                const bool frame = twin.scope == blobs::TwinScope::ListedFrames;
                ++(frame ? coverage.frame : coverage.plant);
                const int at = find_source(pair.shader, pair.kind);
                const Source *source = at >= 0 ? &s_sources[static_cast<std::size_t>(at)] : nullptr;
                const bool kind_failed = s_kind_failed[static_cast<std::size_t>(pair.kind)];
                if (source != nullptr && source->state == TwinState::Ready && !source->stale && !kind_failed)
                {
                    ++(frame ? coverage.frame_ready : coverage.plant_ready);
                    continue;
                }
                ++coverage.missing;
                const char *why = kind_failed                           ? "kind off"
                                  : source == nullptr                   ? "no source"
                                  : source->state == TwinState::Pending ? "parsing"
                                  : source->state == TwinState::Failed  ? "failed"
                                                                        : "stale";
                decltype(Source::name) name{};
                if (source != nullptr)
                {
                    name = source->name;
                }
                else
                {
                    copy_text_field_guarded(pair.shader, Constants::SHADER_RESOURCE_NAME_OFFSET, name.data(),
                                            name.size());
                }
                const auto mask =
                    DMK::memory::read<std::uint64_t>(DMK::Address{pair.shader + Constants::SHADER_GEN_MASK_OFFSET});
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: coverage: {} {} (CShader {:#x}, gen mask {:#x}) has no ready "
                                         "twin ({})",
                                         twin.label, name.data(), pair.shader, mask.value_or(0), why);
                if (used + 1 < coverage.missing_text.size())
                {
                    const auto written =
                        std::format_to_n(coverage.missing_text.data() + used, coverage.missing_text.size() - 1 - used,
                                         "{}{} {} ({})", used == 0 ? "" : ", ", twin.label, name.data(), why);
                    used = std::min(used + static_cast<std::size_t>(written.size), coverage.missing_text.size() - 1);
                }
            }
            coverage.missing_text[used] = '\0';
            return coverage;
        }

        /** @brief Where a candidate came from, for the capacity log line. */
        [[nodiscard]] const char *candidate_origin(const TwinCandidate &candidate, bool from_inventory) noexcept
        {
            if (from_inventory)
            {
                return "the level inventory";
            }
            return candidate.priority == 2   ? "a render miss"
                   : candidate.priority == 1 ? "a character material"
                                             : "a material load";
        }

#if defined(TPVCAMERA_DEV_BUILD)
        // The s_usable_generation the DEV clone dry-run was last handed its pairs at (dry_run_pairs()), so a paused
        // frame hands them over again only once a twin became ready or checked out again since; -1 until the first
        // hand-over of this module, so the first pause after a DEV hot reload hands over the twins its recovery took.
        // Main thread only.
        int s_dry_run_generation = -1;
        // Set by create_during_pause() in a frame whose pause had nothing left to create (no candidate it could take,
        // or every source slot in use): only such a frame runs a step of the DEV clone dry-run
        // (service_paused_dry_run()). Main thread only.
        bool s_pause_drained = false;

        /**
         * @brief The ready plant and frame twins of a kind the DEV clone can derive, for its dry-run (which leaves out
         *        the pairs it handled already this session).
         * @param changed_only Hand back none when no twin became ready or checked out again since the last call.
         * @throws std::bad_alloc If the list cannot allocate.
         * @note Main thread, under s_main_mutex. The source table holds every pair's shaders for the session.
         */
        [[nodiscard]] std::vector<TwinClone::DryRunPair> dry_run_pairs(bool changed_only)
        {
            const auto wanted = [](const Source &source)
            {
                return source.state == TwinState::Ready && !source.stale && !kind_off(source.kind) &&
                       listed_scope(blobs::k_twins[source.kind].scope) && TwinClone::clone_capable(source.kind);
            };
            std::vector<TwinClone::DryRunPair> pairs;
            if (changed_only && s_usable_generation == s_dry_run_generation)
            {
                return pairs;
            }
            const int ready =
                static_cast<int>(std::count_if(s_sources.begin(), s_sources.begin() + s_source_count, wanted));
            pairs.reserve(static_cast<std::size_t>(ready));
            for (int i = 0; i < s_source_count; ++i)
            {
                const Source &source = s_sources[static_cast<std::size_t>(i)];
                if (wanted(source))
                {
                    pairs.push_back(TwinClone::DryRunPair{.stock = source.shader,
                                                          .twin = source.twin,
                                                          .kind = source.kind,
                                                          .mask = source.mask,
                                                          .name = source.name,
                                                          .recovered = source.recovered});
                }
            }
            s_dry_run_generation = s_usable_generation;
            return pairs;
        }
#endif

        /**
         * @brief The loading screen's preparation and creation, called while an engine loading operation still owns the
         *        screen (the only other creation is one twin at a time in a frame the game stands still in,
         *        create_during_pause()).
         * @details No mod mutex spans a render flush: the engine's wait services callbacks. Closing admission first
         *          makes racing loader/render callbacks stay stock. Sources still pending after the first barrier
         *          remain owned for a later load rather than occupying a permanently failed source slot. After the
         *          first barrier the level inventory (take_inventory()) adds, outside the 512-entry batch, every plant
         *          and frame pair the level's tables can draw that has no source yet, so every boundary covers the
         *          level's content whatever this load happened to load (a same-level save load, the first load after
         *          a DEV hot reload). The plant and frame candidates go before the character's, and those of a kind
         *          that is off after every other, so a full source table turns a character twin away first, and the
         *          pairs that miss a slot are logged by name and gen mask (up to SHADER_TWIN_CAPACITY_LOG_LINES). An
         *          inventory pair whose stock shader still parses after the first barrier moves on to the pause
         *          inventory, so the next pause creates it. The epoch closes with the inventory's coverage: its pairs
         *          with a ready twin, and the others by name.
         */
        void finalize_loading(const char *reason) noexcept
        {
            DMK_PROFILE_SCOPE("twins.load_finalize");
            const std::uintptr_t renderer = loading_renderer();
            if (renderer == 0 || !s_available.load(std::memory_order_acquire) ||
                s_disabled.load(std::memory_order_acquire) ||
                s_loading_finalizing.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            struct FinalizingScope
            {
                ~FinalizingScope() noexcept { s_loading_finalizing.store(false, std::memory_order_release); }
            } finalizing_scope;
            drop_evicted();
            CandidateBatch batch;
            std::array<bool, k_max_candidates> deferred{};
            std::size_t deferred_count = 0;
            int created = 0;
            int ready = 0;
            int outstanding = 0;
            int capacity_skipped = 0;
            int total_sources = 0;
            int inventory_created = 0;
            int inventory_deferred = 0;
            int inventory_unplaced = 0;
            int misses_outside = 0;
            bool issued = false;
#if defined(TPVCAMERA_DEV_BUILD)
            // The ready plant and frame twins after the creations, for the DEV clone dry-run.
            std::vector<TwinClone::DryRunPair> dry_run;
#endif
            const auto started = GetTickCount64();
            try
            {
                {
                    const std::lock_guard lock(s_candidate_mutex);
                    batch.count = std::exchange(s_candidate_count, 0);
                    std::copy_n(s_candidates.begin(), batch.count, batch.entries.begin());
                    std::fill_n(s_candidates.begin(), batch.count, TwinCandidate{});
                }
                bool prepared = false;
                // The level inventory a pause took after a DEV hot reload: this boundary's own inventory supersedes
                // it, and its references are dropped when this scope ends, outside every mod mutex.
                InventoryBatch pause_left;
                InventoryBatch pause_done;
                {
                    const std::lock_guard lock(s_main_mutex);
                    pause_left.entries.swap(s_pause_inventory);
                    pause_done.entries.swap(s_pause_inventory_done);
                    prepared = s_available.load(std::memory_order_acquire) && prepare(started);
                }
                if (!prepared)
                {
                    // A temporarily unavailable engine may become ready at the save-load boundary of this load.
                    const std::lock_guard lock(s_candidate_mutex);
                    if (s_available.load(std::memory_order_acquire) && !s_disabled.load(std::memory_order_acquire))
                    {
                        // An entry that does not fit stays in the batch, whose destructor drops it.
                        for (std::size_t i = 0; i < batch.count && s_candidate_count < s_candidates.size(); ++i)
                        {
                            s_candidates[s_candidate_count++] = batch.entries[i];
                            batch.entries[i].request = {};
                        }
                    }
                    return;
                }
                if (!flush_loading_commands(renderer))
                {
                    const std::lock_guard lock(s_main_mutex);
                    turn_off(GetTickCount64(), "loading screen could not flush the stock shader commands");
                    return;
                }
                // The level inventory, after the barrier, so its stock shaders are parsed like the captured ones.
                InventoryBatch inventory;
                InventoryOutcome inventoried{};
                const std::uint64_t rules = Rules::current_generation();
                try
                {
                    inventoried = take_inventory(batch, inventory);
                }
                catch (...)
                {
                    inventoried.ran = true;
                    inventoried.failed = true;
                }
                if ((inventoried.ran && !inventoried.failed) || inventoried.listed_off)
                {
                    // This epoch has its inventory (or needs none) under these rules: the next pause takes none of its
                    // own unless the rules change.
                    s_inventory_epoch.store(s_load_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    s_inventory_rules.store(rules, std::memory_order_relaxed);
                }
                log_inventory(inventoried);
                // The plant and frame candidates first, a drawn item's before a loaded material's, then the
                // character's, and last those of a kind that is off (kind_off(): a source slot with no twin and no
                // parse, so a later capture of theirs is not retained again): the batch's listed ones, the
                // inventory's, then the batch's others.
                const auto rank = [](const TwinCandidate &candidate)
                { return kind_off(candidate.request.kind) ? -1 : candidate_rank(candidate); };
                std::sort(batch.entries.begin(), batch.entries.begin() + static_cast<std::ptrdiff_t>(batch.count),
                          [&rank](const TwinCandidate &a, const TwinCandidate &b) { return rank(a) > rank(b); });
                const std::size_t listed_count = static_cast<std::size_t>(std::count_if(
                    batch.entries.begin(), batch.entries.begin() + static_cast<std::ptrdiff_t>(batch.count),
                    [&rank](const TwinCandidate &candidate) { return rank(candidate) >= 4; }));
                {
                    const std::lock_guard lock(s_main_mutex);
                    if (s_available.load(std::memory_order_acquire) && !s_disabled.load(std::memory_order_acquire))
                    {
                        const int before = s_source_count;
                        const std::size_t inventory_count = inventory.entries.size();
                        // Room in both pause lists for every inventory pair first, so one whose stock shader still
                        // parses moves on to the next pause (create_during_pause()) without an allocation that could
                        // lose it; when there is none, such a pair waits for the next boundary instead.
                        bool pause_room = false;
                        try
                        {
                            s_pause_inventory.reserve(s_pause_inventory.size() + inventory_count);
                            s_pause_inventory_done.reserve(s_pause_inventory_done.size() + s_pause_inventory.size() +
                                                           inventory_count);
                            pause_room = true;
                        }
                        catch (...)
                        {
                        }
                        for (std::size_t n = 0; n < batch.count + inventory_count; ++n)
                        {
                            const bool from_inventory = n >= listed_count && n < listed_count + inventory_count;
                            const std::size_t i = n < listed_count ? n
                                                  : from_inventory ? n - listed_count
                                                                   : n - inventory_count;
                            TwinCandidate &candidate = from_inventory ? inventory.entries[i] : batch.entries[i];
                            const auto &request = candidate.request;
                            if (find_source(request.shader, request.kind) >= 0)
                            {
                                continue;
                            }
                            if (s_source_count >= k_max_sources)
                            {
                                if (kind_off(request.kind))
                                {
                                    continue; // It would get no twin anyway.
                                }
                                // Each by name up to a bound per load; the epoch line counts them all.
                                if (++capacity_skipped > Constants::SHADER_TWIN_CAPACITY_LOG_LINES)
                                {
                                    continue;
                                }
                                decltype(Source::name) name{};
                                copy_text_field_guarded(request.shader, Constants::SHADER_RESOURCE_NAME_OFFSET,
                                                        name.data(), name.size());
                                const auto mask = DMK::memory::read<std::uint64_t>(
                                    DMK::Address{request.shader + Constants::SHADER_GEN_MASK_OFFSET});
                                (void)DMK::log().try_log(
                                    DMK::LogLevel::Info,
                                    "ShaderTwins: no source slot left ({} in use) for the {} twin of {} (gen mask "
                                    "{:#x}, CShader {:#x}, resources {:#x}) from {}; its items keep the stock shader{}",
                                    k_max_sources, blobs::k_twins[request.kind].label, name.data(), mask.value_or(0),
                                    request.shader, request.resources, candidate_origin(candidate, from_inventory),
                                    capacity_skipped == Constants::SHADER_TWIN_CAPACITY_LOG_LINES
                                        ? " (any more this load are only counted)"
                                        : "");
                                continue;
                            }
                            const auto flags = DMK::memory::read<std::uint32_t>(
                                DMK::Address{request.shader + Constants::SHADER_FLAGS_OFFSET});
                            if (flags && (*flags & Constants::SHADER_FLAG_NOT_FOUND) == 0 &&
                                (*flags & Constants::SHADER_FLAG_LOADED) == 0)
                            {
                                // A captured one stays in the pool; an inventory pair moves on to the pause
                                // inventory, which owns it from here (this epoch's inventory is taken, so no pause
                                // would find it again).
                                if (!from_inventory)
                                {
                                    deferred[i] = true;
                                    ++deferred_count;
                                }
                                else if (pause_room)
                                {
                                    // Its room was reserved above, so this does not allocate.
                                    s_pause_inventory.push_back(std::exchange(candidate, TwinCandidate{}));
                                    ++inventory_deferred;
                                }
                                else
                                {
                                    ++inventory_unplaced;
                                }
                                continue;
                            }
                            issued = true; // Even a guarded engine fault may have submitted a parse command.
                            const int sources_before = s_source_count;
                            create_twin(request, GetTickCount64());
                            inventory_created += from_inventory && s_source_count > sources_before ? 1 : 0;
                        }
                        created = s_source_count - before;
                        total_sources = s_source_count;
                    }
                }
                // Also flush when every name was already cached. This keeps the close protocol independent of
                // mfForName's internal cache behavior, including partial failures.
                const bool completed = flush_loading_commands(renderer);
                issued = false;
                Coverage coverage{};
                {
                    const std::lock_guard lock(s_main_mutex);
                    if (!completed)
                    {
                        turn_off(GetTickCount64(), "loading screen could not flush the twin shader commands");
                    }
                    else if (s_available.load(std::memory_order_acquire))
                    {
                        refresh_twins(GetTickCount64());
                        for (int i = 0; i < s_source_count; ++i)
                        {
                            ready += s_sources[static_cast<std::size_t>(i)].state == TwinState::Ready ? 1 : 0;
                            outstanding += s_sources[static_cast<std::size_t>(i)].state == TwinState::Pending ? 1 : 0;
                        }
                        // While the batch and the inventory still hold their shaders. A failed inventory leaves pairs
                        // of an earlier load, which nothing holds any more.
                        if (inventoried.failed)
                        {
                            s_inventory_pairs.clear();
                        }
                        misses_outside = report_boundary_misses(batch);
                        coverage = compute_coverage();
#if defined(TPVCAMERA_DEV_BUILD)
                        if constexpr (TwinClone::k_stage == TwinClone::Stage::DryRun)
                        {
                            try
                            {
                                dry_run = dry_run_pairs(false);
                            }
                            catch (...)
                            {
                                dry_run.clear();
                            }
                        }
#endif
                    }
                }
                {
                    const std::lock_guard lock(s_candidate_mutex);
                    if (s_available.load(std::memory_order_acquire) && !s_disabled.load(std::memory_order_acquire))
                    {
                        // An entry that does not fit stays in the batch, whose destructor drops it.
                        for (std::size_t i = 0; i < batch.count && s_candidate_count < s_candidates.size(); ++i)
                        {
                            if (deferred[i])
                            {
                                s_candidates[s_candidate_count++] = batch.entries[i];
                                batch.entries[i].request = {};
                            }
                        }
                    }
                }
                (void)DMK::log().try_log(
                    capacity_skipped > 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                    "ShaderTwins: loading epoch {} closed at {} on thread {}: {} candidate(s) and {} from the level "
                    "inventory, {} new source(s) ({} of them the inventory's), {}/{} source slots, {} ready twin(s), "
                    "{} outstanding parse(s), {} source(s) deferred, {} inventory pair(s) still parsing "
                    "(created at the next pause; {} more found again at the next boundary), {} candidate(s) skipped at "
                    "capacity, {} render miss(es) outside the level inventory; render commands {}; total preparation "
                    "{} ms; unpaused gameplay creates no twins",
                    s_load_epoch.load(std::memory_order_relaxed), reason, GetCurrentThreadId(), batch.count,
                    inventory.entries.size(), created, inventory_created, total_sources, k_max_sources, ready,
                    outstanding, deferred_count, inventory_deferred, inventory_unplaced, capacity_skipped,
                    misses_outside, completed ? "flushed" : "failed", GetTickCount64() - started);
                if (coverage.plant + coverage.frame > 0)
                {
                    (void)DMK::log().try_log(coverage.missing > 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                                             "ShaderTwins: coverage: plant {}/{} ready, frame {}/{} ready{}{}",
                                             coverage.plant_ready, coverage.plant, coverage.frame_ready, coverage.frame,
                                             coverage.missing > 0 ? "; missing " : "", coverage.missing_text.data());
                }
#if defined(TPVCAMERA_DEV_BUILD)
                // The DEV clone dry-run of the ready plant and frame twins, inside the boundary and with every mod
                // mutex released (it takes the HW shader factory's lock without waiting and calls the resource lookup).
                // What its budget leaves goes on in the next pause with nothing left to create. A failed flush that
                // just turned the twins off runs none of it.
                if constexpr (TwinClone::k_stage == TwinClone::Stage::DryRun)
                {
                    if (!s_disabled.load(std::memory_order_acquire))
                    {
                        std::array<char, 48> occasion{};
                        (void)std::format_to_n(occasion.data(), occasion.size() - 1, "loading epoch {}",
                                               s_load_epoch.load(std::memory_order_relaxed));
                        (void)TwinClone::queue_dry_run(dry_run, occasion.data());
                        TwinClone::run_dry_run(Constants::TWIN_CLONE_DRY_RUN_LOADING_US, 0, occasion.data());
                    }
                }
#endif
            }
            catch (...)
            {
                // Stack unwinding has released every mod mutex before the engine wait below.
                if (issued)
                {
                    (void)flush_loading_commands(renderer);
                }
                s_disabled.store(true, std::memory_order_release);
                s_set_published.store(nullptr, std::memory_order_release);
                s_rows_published.store(nullptr, std::memory_order_release);
                (void)DMK::log().try_log(DMK::LogLevel::Error,
                                         "ShaderTwins: loading preparation failed; remaining items keep stock shaders");
            }
        }

        [[nodiscard]] const char *pause_text(PauseReason reason) noexcept
        {
            switch (reason)
            {
            case PauseReason::Menu:
                return "the game menu";
            case PauseReason::Overlay:
                return "a UI overlay over the stopped game clock";
            case PauseReason::Clock:
                return "the stopped game clock (no game menu or UI overlay seen)";
            case PauseReason::None:
            default:
                return "no pause";
            }
        }

        /**
         * @brief Puts in @p pooled the (shader, kind) pairs the pool holds, sorted, so the pause inventory does not
         *        queue them twice.
         * @return False, with @p pooled empty, when the candidate mutex is busy (a loader or a render job holds it for
         *         a moment): the caller tries again in a later frame.
         * @throws std::bad_alloc If the list cannot allocate.
         * @note Main thread, under s_main_mutex in a camera frame, so it never waits for the candidate mutex. Holds it
         *       only to copy the identities.
         */
        [[nodiscard]] bool pooled_pairs(std::vector<InventoryKey> &pooled)
        {
            pooled.clear();
            pooled.reserve(k_max_candidates);
            {
                const std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
                if (!lock.owns_lock())
                {
                    return false;
                }
                for (std::size_t i = 0; i < s_candidate_count; ++i)
                {
                    pooled.push_back(InventoryKey{s_candidates[i].request.shader, s_candidates[i].request.kind});
                }
            }
            std::ranges::sort(pooled);
            return true;
        }

        /**
         * @brief Takes the level inventory in a frame the game stands still in, for a loading epoch whose boundary did
         *        not take one in this module (the first pause after a DEV hot reload) or whose inventory scoped its
         *        assets with camera rules edited since, and keeps the pairs it queued in s_pause_inventory for
         *        create_during_pause().
         * @details Runs once per epoch and rules generation, whatever comes of it: a walk that finds no level or fails
         *          is not repeated until the next boundary takes its own or the rules change again. Only a busy
         *          candidate mutex leaves it for a later frame, before anything is read. Pairs with a source already,
         *          a candidate in the pool (a render miss, a play-time capture) or a place in the pause inventory are
         *          not queued. A pair the new rules no longer scope keeps its twin, which only the scan's classes use.
         * @note Main thread, under s_main_mutex, in a frame the game stands still in (the level tables are written
         *       only while a level loads).
         */
        void pause_inventory(PauseReason reason) noexcept
        {
            const std::uint64_t epoch = s_load_epoch.load(std::memory_order_relaxed);
            const std::uint64_t rules = Rules::current_generation();
            // The epoch has an inventory already: this walk follows a rules edit.
            const bool rules_changed = s_inventory_epoch.load(std::memory_order_relaxed) == epoch;
            InventoryBatch inventory;
            InventoryOutcome outcome{};
            try
            {
                std::vector<InventoryKey> pooled;
                if (!pooled_pairs(pooled))
                {
                    return;
                }
                for (const TwinCandidate &waiting : s_pause_inventory)
                {
                    pooled.push_back(InventoryKey{waiting.request.shader, waiting.request.kind});
                }
                std::ranges::sort(pooled);
                s_inventory_epoch.store(epoch, std::memory_order_relaxed);
                s_inventory_rules.store(rules, std::memory_order_relaxed);
                std::vector<InventoryFound> found;
                outcome = walk_inventory(found);
                if (!outcome.ran)
                {
                    // Not taken: no pairs of an earlier load stay behind.
                    s_inventory_pairs.clear();
                }
                else
                {
                    queue_inventory(
                        found, [&pooled](std::uintptr_t shader, int kind)
                        { return std::ranges::binary_search(pooled, InventoryKey{shader, kind}); }, inventory, outcome);
                    // Room for every entry in both lists first, so moving one along never allocates.
                    const std::size_t total = s_pause_inventory.size() + inventory.entries.size();
                    s_pause_inventory.reserve(total);
                    s_pause_inventory_done.reserve(s_pause_inventory_done.size() + total);
                    s_pause_inventory.insert(s_pause_inventory.end(), inventory.entries.begin(),
                                             inventory.entries.end());
                    inventory.entries.clear();
                }
            }
            catch (...)
            {
                s_inventory_epoch.store(epoch, std::memory_order_relaxed);
                s_inventory_rules.store(rules, std::memory_order_relaxed);
                s_inventory_pairs.clear();
                outcome.ran = true;
                outcome.failed = true;
            }
            log_inventory(outcome);
            if (!outcome.failed && outcome.walk.level)
            {
                if (rules_changed)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "ShaderTwins: the camera rules changed (generation {}), so this pause "
                                             "({}) took the level inventory again: {} pair(s) wait for their twin, "
                                             "created one at a time while the pause lasts",
                                             rules, pause_text(reason), s_pause_inventory.size());
                }
                else
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "ShaderTwins: the first pause of loading epoch {} ({}) took the level "
                                             "inventory no loading screen took for this module (a DEV hot reload): {} "
                                             "pair(s) wait for their twin, created one at a time while the pause lasts",
                                             epoch, pause_text(reason), s_pause_inventory.size());
                }
            }
        }

        /** @brief What take_pause_pick() takes out besides its pick: entries to drop once every mod mutex is free. */
        struct PauseStale
        {
            std::array<TwinCandidate, 16> entries{};
            std::size_t count = 0;
            ~PauseStale() noexcept
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    drop_candidate(entries[i]);
                }
            }
        };

        /**
         * @brief True when @p candidate is done with: its kind is off (kind_off(): a pause spends no barrier on a
         *        source slot with no twin), its pair has a source already, or its stock shader did not load (or reads
         *        back as anything but a loaded or loading shader). Sets @p loaded to whether it finished its parse.
         * @note Main thread, under s_main_mutex, while the candidate holds its shader.
         */
        [[nodiscard]] bool pause_candidate_done(const TwinCandidate &candidate, bool &loaded) noexcept
        {
            loaded = false;
            if (kind_off(candidate.request.kind) || find_source(candidate.request.shader, candidate.request.kind) >= 0)
            {
                return true;
            }
            const auto flags = DMK::memory::read<std::uint32_t>(
                DMK::Address{candidate.request.shader + Constants::SHADER_FLAGS_OFFSET});
            if (!flags || (*flags & Constants::SHADER_FLAG_NOT_FOUND) != 0)
            {
                return true;
            }
            loaded = (*flags & Constants::SHADER_FLAG_LOADED) != 0;
            return false;
        }

        /**
         * @brief Takes out the candidate a pause creates next: a variant a render job drew without its twin (a listed
         *        node's, then the character's), else the pause inventory's next one, else a plant or frame variant a
         *        material loaded during play; within each, the one waiting longest. Only a candidate whose stock
         *        shader finished its own parse is taken; one still parsing stays.
         * @details The pool's other characters' candidates (their eyes and hair from a loading screen) and those of a
         *          kind that is off are left for the next boundary. A pool candidate that is done with
         *          (pause_candidate_done()) leaves the pool into @p stale, at most its capacity per call; an inventory
         *          one (its kind off included) moves to s_pause_inventory_done, which keeps its references for the
         *          coverage line.
         * @param out Receives the pick, owned by the caller.
         * @param from_inventory Set when the pick is the pause inventory's.
         * @param left Receives how many candidates a pause could still create after this one.
         * @param stale Receives the pool's candidates that are done with, which the caller drops once it released
         *        every mod mutex.
         * @return True when a candidate was taken.
         * @note Main thread, under s_main_mutex. Takes the candidate mutex without waiting: a busy one leaves the pool
         *       for the next frame. Releases nothing.
         */
        [[nodiscard]] bool take_pause_pick(TwinCandidate &out, bool &from_inventory, int &left,
                                           PauseStale &stale) noexcept
        {
            from_inventory = false;
            left = 0;
            try
            {
                // Held through both lists, so the pool's best keeps its index; without it only the inventory is read.
                const std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
                int pool_rank = -1;
                std::size_t pool_at = k_max_candidates;
                std::uint64_t pool_first = 0;
                for (std::size_t i = 0; lock.owns_lock() && i < s_candidate_count;)
                {
                    TwinCandidate &candidate = s_candidates[i];
                    const bool listed = listed_kind(candidate.request.kind);
                    // One of a kind that is off stays for the next boundary, which gives it a source slot after
                    // every other candidate: dropped here, a render job would retain it again the next frame.
                    if ((!listed && candidate.priority != 2) || kind_off(candidate.request.kind))
                    {
                        ++i;
                        continue;
                    }
                    bool loaded = false;
                    if (pause_candidate_done(candidate, loaded))
                    {
                        if (stale.count < stale.entries.size())
                        {
                            // The last entry, not looked at yet, takes this slot.
                            stale.entries[stale.count++] = candidate;
                            candidate = s_candidates[--s_candidate_count];
                            s_candidates[s_candidate_count] = TwinCandidate{};
                            continue;
                        }
                        ++i;
                        continue;
                    }
                    ++left;
                    const int rank = (candidate.priority == 2 ? 2 : 0) + (listed ? 1 : 0);
                    if (loaded && (rank > pool_rank || (rank == pool_rank && candidate.first_ms < pool_first)))
                    {
                        pool_rank = rank;
                        pool_at = i;
                        pool_first = candidate.first_ms;
                    }
                    ++i;
                }
                const auto take_pooled = [&out, &left](std::size_t at)
                {
                    out = s_candidates[at];
                    s_candidates[at] = s_candidates[--s_candidate_count];
                    s_candidates[s_candidate_count] = TwinCandidate{};
                    --left;
                };
                // A variant drawn now goes before the inventory's.
                if (pool_at < s_candidate_count && pool_rank >= 2)
                {
                    take_pooled(pool_at);
                    left += static_cast<int>(s_pause_inventory.size());
                    return true;
                }
                bool picked = false;
                for (std::size_t i = 0; i < s_pause_inventory.size();)
                {
                    const TwinCandidate candidate = s_pause_inventory[i];
                    bool loaded = false;
                    const bool done = pause_candidate_done(candidate, loaded);
                    if (!done && (picked || !loaded))
                    {
                        ++left;
                        ++i;
                        continue;
                    }
                    if (done)
                    {
                        // Its room was reserved for every entry the list ever had, so this does not allocate.
                        s_pause_inventory_done.push_back(candidate);
                    }
                    else
                    {
                        out = candidate;
                        from_inventory = true;
                        picked = true;
                    }
                    s_pause_inventory.erase(s_pause_inventory.begin() + static_cast<std::ptrdiff_t>(i));
                }
                if (picked)
                {
                    return true;
                }
                if (pool_at < s_candidate_count)
                {
                    take_pooled(pool_at);
                    return true;
                }
            }
            catch (...)
            {
                // Whatever was taken out stays owned by out, stale or s_pause_inventory_done.
            }
            return false;
        }

        /**
         * @brief Logs the pause inventory's coverage once its last candidate was handled, and hands back the handled
         *        ones, whose references the caller drops outside every mod mutex.
         * @note Main thread, under s_main_mutex, after the last candidate's barrier, while the handled ones still hold
         *       the shaders the coverage line names.
         */
        void finish_pause_inventory(InventoryBatch &handled) noexcept
        {
            const Coverage coverage = compute_coverage();
            if (coverage.plant + coverage.frame > 0)
            {
                (void)DMK::log().try_log(coverage.missing > 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                                         "ShaderTwins: coverage after the pause inventory: plant {}/{} ready, frame "
                                         "{}/{} ready{}{}",
                                         coverage.plant_ready, coverage.plant, coverage.frame_ready, coverage.frame,
                                         coverage.missing > 0 ? "; missing " : "", coverage.missing_text.data());
            }
            handled.entries.swap(s_pause_inventory_done);
        }

        /**
         * @brief False when a plant or frame variant a material loads during play needs no candidate: the published
         *        swap set already names @p shader as a source of @p kind, or the source table is full.
         * @details The sources only ever grow while the module runs, so a match in any swap set, even one the main
         *          thread is replacing, is a source; a set that does not name it (or none yet) lets the candidate in,
         *          and the pause drops it if it had a source after all.
         * @note Any thread: a loader's material callback. Unlike a render job, a loader is not bound to the frame a set
         *       stays valid for, so it may read a set the main thread is rewriting: the scan is bounded by
         *       SHADER_TWIN_MAX_SOURCES and reads only integers, and since every set lists the append-only source table
         *       by index, a torn read can only miss a source (one more candidate, dropped by the pause), never find
         *       one the table does not hold.
         */
        [[nodiscard]] bool playtime_capture_wanted(std::uintptr_t shader, int kind) noexcept
        {
            const SwapSet *set = s_set_published.load(std::memory_order_acquire);
            if (set == nullptr)
            {
                return true;
            }
            if (set->sources_full)
            {
                return false;
            }
            const int count = std::min(set->source_count, k_max_sources);
            for (int i = 0; i < count; ++i)
            {
                const auto at = static_cast<std::size_t>(i);
                if (set->sources[at] == shader && set->kinds[at] == kind)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief The Debug line of a plant or frame variant a material loaded during play, which waits for the next
         *        pause or loading screen for its twin.
         * @note The material-load callback, while it owns @p material and the item.
         */
        void log_playtime_capture(const ShaderItem &item, int kind, std::uintptr_t material) noexcept
        {
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                return;
            }
            std::array<char, 64> name{};
            copy_text_field_guarded(item.shader, Constants::SHADER_RESOURCE_NAME_OFFSET, name.data(), name.size());
            std::array<char, 160> material_name{};
            copy_text_field_guarded(material, Constants::MATERIAL_NAME_OFFSET, material_name.data(),
                                    material_name.size());
            const auto mask =
                DMK::memory::read<std::uint64_t>(DMK::Address{item.shader + Constants::SHADER_GEN_MASK_OFFSET});
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: play-time {} variant {} (gen mask {:#x}) from material {} waits "
                                     "for a pause or loading screen",
                                     blobs::k_twins[kind].label, name.data(), mask.value_or(0), material_name.data());
        }
    } // namespace

    void *__fastcall shader_gen_load_detour(void *manager, void *out, const char *name) noexcept
    {
        const DetourScope in_flight;
        const ShaderGenLoadFn original = s_shader_gen_original.load(std::memory_order_acquire);
        if (name != nullptr)
        {
            for (const blobs::Twin &twin : blobs::k_twins)
            {
                if (starts_with_nocase(name, twin.twin_name))
                {
                    s_ext_aliases.fetch_add(1, std::memory_order_relaxed);
                    return original(manager, out, twin.source_name);
                }
            }
        }
        return original(manager, out, name);
    }

    void release_candidate_shader(std::uintptr_t shader) noexcept
    {
        if (shader == 0 || !is_shader(shader))
        {
            return;
        }
        const auto fn = image_slot(shader, Constants::SHADER_VTABLE_RELEASE_OFFSET);
        if (fn != 0)
        {
            // IShader::Release returns an int; no caller uses it here.
            __try
            {
                (void)reinterpret_cast<int(__fastcall *)(std::uintptr_t)>(fn)(shader);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                s_guard_faults.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void drop_evicted() noexcept
    {
        TwinCandidate evicted{};
        while (s_evicted.pop(evicted.request))
        {
            drop_candidate(evicted);
        }
    }

    [[nodiscard]] bool loading_materials_admitted() noexcept
    {
        if (s_level_loading.load(std::memory_order_acquire))
        {
            return true;
        }
        const auto manager = s_loading_save_manager.load(std::memory_order_acquire);
        if (manager == 0)
        {
            return false;
        }
        const auto flag =
            DMK::memory::read<std::uint8_t>(DMK::Address{manager + Constants::SAVE_MANAGER_ENGINE_LOADING_OFFSET});
        return flag && *flag == 1;
    }

    bool collect_candidate(const ShaderItem &item, int kind, std::uint8_t priority, bool loading_only) noexcept
    {
        if (kind < 0 || kind >= k_kinds || item.shader == 0 || !s_available.load(std::memory_order_acquire) ||
            s_disabled.load(std::memory_order_acquire) || s_loading_finalizing.load(std::memory_order_acquire) ||
            (loading_only && !loading_materials_admitted()))
        {
            return false;
        }
        TwinCandidate evicted{};
        bool added = false;
        try
        {
            std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
            if (!lock.owns_lock() || !s_available.load(std::memory_order_acquire) ||
                s_loading_finalizing.load(std::memory_order_acquire) || (loading_only && !loading_materials_admitted()))
            {
                s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            std::size_t characters = 0;
            for (std::size_t i = 0; i < s_candidate_count; ++i)
            {
                auto &existing = s_candidates[i];
                if (existing.request.shader == item.shader && existing.request.kind == kind)
                {
                    if (priority == 2 && existing.priority < 2)
                    {
                        s_render_miss_new.store(true, std::memory_order_relaxed);
                    }
                    existing.priority = std::max(existing.priority, priority);
                    return false;
                }
                characters += listed_kind(existing.request.kind) ? 0 : 1;
            }
            const bool listed = listed_kind(kind);
            const bool share_full = !listed && characters >= k_max_candidates - k_reserved_listed_candidates;
            std::size_t slot = s_candidate_count;
            if (slot == s_candidates.size() || share_full)
            {
                // The lowest-ranked entry whose slot this one may take: with the character share full, only
                // another character's.
                slot = s_candidates.size();
                for (std::size_t i = 0; i < s_candidate_count; ++i)
                {
                    if (share_full && listed_kind(s_candidates[i].request.kind))
                    {
                        continue;
                    }
                    if (slot == s_candidates.size() ||
                        candidate_rank(s_candidates[i]) < candidate_rank(s_candidates[slot]))
                    {
                        slot = i;
                    }
                }
                if (slot == s_candidates.size() || candidate_rank(s_candidates[slot]) >= candidate_rank(kind, priority))
                {
                    s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
            }
            const Ownership owned = take_candidate_refs(item);
            if (owned == Ownership::ShaderOnly)
            {
                // The shader is still owned by this callback, so this cannot delete it.
                lock.unlock();
                release_candidate_shader(item.shader);
                return false;
            }
            if (owned != Ownership::Owned)
            {
                return false;
            }
            bool commit = true;
            if (slot < s_candidate_count)
            {
                // The evicted pair's Release can be the last one (s_evicted): on the main thread it runs below,
                // once the mutex is released, and elsewhere the main thread runs it later. With no room in
                // s_evicted the evicted pair stays and the new one goes instead: its callback owns the item, so
                // that Release is not the last one.
                if (on_engine_main_thread())
                {
                    evicted = s_candidates[slot];
                }
                else if (!s_evicted.push(s_candidates[slot].request))
                {
                    evicted.request = {item.shader, item.resources, kind};
                    commit = false;
                }
                s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                ++s_candidate_count;
            }
            if (commit)
            {
                s_candidates[slot] = TwinCandidate{
                    .request = {item.shader, item.resources, kind}, .priority = priority, .first_ms = GetTickCount64()};
                added = true;
                if (priority == 2)
                {
                    s_render_miss_new.store(true, std::memory_order_relaxed);
                }
            }
        }
        catch (...)
        {
            s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
        }
        drop_candidate(evicted);
        return added;
    }

    void clear_candidates() noexcept
    {
        std::array<TwinCandidate, k_max_candidates> dropped{};
        std::size_t count = 0;
        try
        {
            const std::lock_guard lock(s_candidate_mutex);
            count = std::exchange(s_candidate_count, 0);
            std::copy_n(s_candidates.begin(), count, dropped.begin());
            std::fill_n(s_candidates.begin(), count, TwinCandidate{});
        }
        catch (...)
        {
            return; // Keep references if the ownership mutex cannot be acquired.
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            drop_candidate(dropped[i]);
        }
    }

    [[nodiscard]] int find_track(std::uintptr_t node) noexcept
    {
        for (int i = 0; i < s_track_count; ++i)
        {
            if (s_tracks[static_cast<std::size_t>(i)].node == node)
            {
                return i;
            }
        }
        return -1;
    }

    [[nodiscard]] const char *invalidate_text(Invalidate result) noexcept
    {
        switch (result)
        {
        case Invalidate::Done:
            return "refills next frame";
        case Invalidate::NoRender:
            return "no render data yet";
        case Invalidate::NotTheNode:
            return "no longer live";
        case Invalidate::Fault:
        default:
            return "fault";
        }
    }

    void drop_record(const PsoRecord &record) noexcept
    {
        drop_pso_refs(record.stock);
        drop_pso_refs(record.twin);
    }

    Invalidate refill(Track &track, trace::RefillReason reason) noexcept
    {
        trace_event(trace::Event::RefillBegin, track.node, 0, 0, 0, static_cast<std::uint64_t>(reason));
        track.retry = false;
        track.deferred = false;
        drop_records(track.node);
        const Invalidate result = invalidate_node_guarded(track.node);
        trace_event(trace::Event::RefillEnd, track.node, 0, 0, 0, static_cast<std::uint64_t>(reason),
                    static_cast<std::uint64_t>(result));
        if (result == Invalidate::Fault)
        {
            s_guard_faults.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    void release_track(Track &track, std::uint64_t now, const char *why) noexcept
    {
        trace_event(trace::Event::TrackReleased, track.node, 0, 0, 0, track.activated);
        if (!track.activated)
        {
            drop_records(track.node);
            return;
        }
        const Invalidate result = refill(track, trace::RefillReason::Release);
        (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: swapped off {} ({}, {:.1f} s, {})",
                                 track.name.data(), why, static_cast<double>(now - track.swapped_ms) / 1000.0,
                                 invalidate_text(result));
    }

    void release_all(std::uint64_t now, const char *why) noexcept
    {
        if constexpr (trace::enabled)
            trace::cancel_candidates(s_frame_seq.load(std::memory_order_relaxed));
        s_set_published.store(nullptr, std::memory_order_release);
        s_rows_published.store(nullptr, std::memory_order_release);
        for (int i = 0; i < s_released_count; ++i)
        {
            Track &released = s_released[static_cast<std::size_t>(i)];
            release_track(released, now, released.why);
        }
        s_released_count = 0;
        for (int i = 0; i < s_track_count; ++i)
        {
            release_track(s_tracks[static_cast<std::size_t>(i)], now, why);
        }
        s_track_count = 0;
        s_refill_all = false;
    }

    [[nodiscard]] bool scope_usable(blobs::TwinScope scope) noexcept
    {
        for (int kind = 0; kind < k_kinds; ++kind)
        {
            if (blobs::k_twins[kind].scope == scope && !s_kind_failed[static_cast<std::size_t>(kind)])
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool scope_ready(blobs::TwinScope scope) noexcept
    {
        for (int i = 0; i < s_source_count; ++i)
        {
            const Source &source = s_sources[static_cast<std::size_t>(i)];
            if (source.state == TwinState::Ready && blobs::k_twins[source.kind].scope == scope &&
                !s_kind_failed[static_cast<std::size_t>(source.kind)])
            {
                return true;
            }
        }
        return false;
    }

    void stop_plants(std::uint64_t now, const char *why) noexcept
    {
        if (!scope_usable(blobs::TwinScope::ListedPlants) && !scope_usable(blobs::TwinScope::ListedFrames))
        {
            return;
        }
        for (int kind = 0; kind < k_kinds; ++kind)
        {
            if (listed_scope(blobs::k_twins[kind].scope))
            {
                s_kind_failed[static_cast<std::size_t>(kind)] = true;
            }
        }
        if (!scope_usable(blobs::TwinScope::Dissolving))
        {
            turn_off(now, why);
            return;
        }
        release_all(now, "plants off");
        (void)DMK::log().try_log(
            DMK::LogLevel::Warning,
            "ShaderTwins: {}; trees and frames keep the stock shader (no see-through, no near cut) for "
            "the session, the character fade's twins go on",
            why);
    }

    void refresh_twins(std::uint64_t now) noexcept
    {
        bool newly_ready = false;
        for (int i = 0; i < s_source_count; ++i)
        {
            Source &source = s_sources[static_cast<std::size_t>(i)];
            if (source.state != TwinState::Pending)
            {
                continue;
            }
            const auto flags =
                DMK::memory::read<std::uint32_t>(DMK::Address{source.twin + Constants::SHADER_FLAGS_OFFSET});
            const auto techniques =
                DMK::memory::read<std::uint32_t>(DMK::Address{source.twin + Constants::SHADER_TECHNIQUE_COUNT_OFFSET});
            const auto id = DMK::memory::read<std::uint32_t>(DMK::Address{source.twin + Constants::SHADER_ID_OFFSET});
            const auto mask =
                DMK::memory::read<std::uint64_t>(DMK::Address{source.twin + Constants::SHADER_GEN_MASK_OFFSET});
            const auto flags2 =
                DMK::memory::read<std::uint32_t>(DMK::Address{source.twin + Constants::SHADER_FLAGS2_OFFSET});
            const auto source_flags2 =
                DMK::memory::read<std::uint32_t>(DMK::Address{source.shader + Constants::SHADER_FLAGS2_OFFSET});
            const double waited = static_cast<double>(now - source.created_ms) / 1000.0;
            const blobs::Twin &twin = blobs::k_twins[source.kind];
            if (!flags || !techniques || !id || !mask || !flags2 || !source_flags2)
            {
                source.state = TwinState::Failed;
                (void)DMK::log().try_log(DMK::LogLevel::Warning, "ShaderTwins: {} twin {:#x} for {} could not be read",
                                         twin.label, source.twin, source.name.data());
                continue;
            }
            if ((*flags & Constants::SHADER_FLAG_NOT_FOUND) != 0)
            {
                source.state = TwinState::Failed;
                if (!std::exchange(s_kind_failed[static_cast<std::size_t>(source.kind)], true))
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Error,
                                             "ShaderTwins: twin {} did not load from the user shader cache "
                                             "(EF_NOTFOUND, flags {:#x}, after {:.2f} s); the {} twin's items keep "
                                             "the stock shader",
                                             twin.twin_name, *flags, waited, twin.label);
                }
                if (std::ranges::all_of(s_kind_failed, [](bool failed) { return failed; }))
                {
                    turn_off(now, "no twin shader loaded");
                    return;
                }
                continue;
            }
            const bool loaded = (*flags & Constants::SHADER_FLAG_LOADED) != 0;
            std::uint16_t features_before = 0;
            std::uint16_t features = 0;
            if (loaded && *techniques != source.techniques)
            {
                source.state = TwinState::Failed;
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: {} twin {:#x} for {} parsed with {} techniques, its source "
                                         "has {} (flags {:#x}, after {:.2f} s); items with this material keep the "
                                         "stock shader",
                                         twin.label, source.twin, source.name.data(), *techniques, source.techniques,
                                         *flags, waited);
            }
            else if (loaded)
            {
                if (*id >= Constants::SHADER_MAX_ID)
                {
                    source.state = TwinState::Failed;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} twin {:#x} for {} has shader id {}, past the {} a "
                                             "render object can name; items with this material keep the stock "
                                             "shader",
                                             twin.label, source.twin, source.name.data(), *id,
                                             Constants::SHADER_MAX_ID);
                }
                else if (*mask != source.mask)
                {
                    source.state = TwinState::Failed;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} twin {:#x} for {} has gen mask {:#x}, its source "
                                             "{:#x}; items with this material keep the stock shader",
                                             twin.label, source.twin, source.name.data(), *mask, source.mask);
                }
                else if (((*flags2 ^ *source_flags2) & Constants::SHADER_FLAG2_HAIR) != 0)
                {
                    source.state = TwinState::Failed;
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} twin {:#x} for {} has flags2 {:#x}, its source {:#x}: "
                                             "the hair flag (EF2_HAIR) differs, so its items would draw in another "
                                             "render list; items with this material keep the stock shader",
                                             twin.label, source.twin, source.name.data(), *flags2, *source_flags2);
                }
                else if (!match_feature_flags_guarded(source.twin, source.shader, features_before, features))
                {
                    source.state = TwinState::Failed;
                    s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: {} twin {:#x} for {}: its feature word could not be "
                                             "matched to its source's; items with this material keep the stock "
                                             "shader",
                                             twin.label, source.twin, source.name.data());
                }
                else
                {
                    source.state = TwinState::Ready;
                    source.ready_ms = now;
#if defined(TPVCAMERA_DEV_BUILD)
                    ++s_usable_generation;
#endif
                    // The reloads so far, so revalidate_twins() notices only a later one.
                    ShaderState stock_state{};
                    ShaderState twin_state{};
                    source.reload_bits = read_shader_state_guarded(source.shader, stock_state) &&
                                                 read_shader_state_guarded(source.twin, twin_state)
                                             ? reload_bits(stock_state, twin_state)
                                             : 0;
                    source.stale = false;
                    // A plant or frame twin refills the swapped nodes, and the PSO guard learns it before any item
                    // can name it (the swap set that carries it is published after this). A character twin goes in
                    // the warm-up table instead, also before the set that carries it: its items, added again every
                    // frame, take it at rest without a refill, and their builds then prepare its sets for the fade.
                    const bool plant = listed_scope(twin.scope);
                    newly_ready = newly_ready || plant;
                    const int at = plant ? s_guard_count.load(std::memory_order_relaxed)
                                         : s_warm_count.load(std::memory_order_relaxed);
                    if (plant && at < k_max_sources)
                    {
                        s_guard_twins[static_cast<std::size_t>(at)].store(source.twin, std::memory_order_relaxed);
                        s_guard_sources[static_cast<std::size_t>(at)].store(source.shader, std::memory_order_relaxed);
                        s_guard_prepass[static_cast<std::size_t>(at)].store(prepass_kind(source.kind),
                                                                            std::memory_order_relaxed);
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
                        s_trace_guard_kinds[static_cast<std::size_t>(at)].store(static_cast<std::uint8_t>(source.kind),
                                                                                std::memory_order_relaxed);
#endif
                        s_guard_count.store(at + 1, std::memory_order_release);
                    }
                    else if (!plant && at < k_max_sources)
                    {
                        source.warm_slot = at;
                        s_warm_twins[static_cast<std::size_t>(at)].store(source.twin, std::memory_order_relaxed);
                        s_warm_sources[static_cast<std::size_t>(at)].store(source.shader, std::memory_order_relaxed);
                        s_warm_transparent_only[static_cast<std::size_t>(at)].store(twin.transparent_only,
                                                                                    std::memory_order_relaxed);
                        s_warm_done[static_cast<std::size_t>(at)].store(false, std::memory_order_relaxed);
                        s_warm_cold[static_cast<std::size_t>(at)].store(false, std::memory_order_relaxed);
                        s_warm_count.store(at + 1, std::memory_order_release);
                    }
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "ShaderTwins: {} twin {:#x} ready for {} after {:.2f} s (id {}, {} "
                                             "techniques, flags {:#x}, flags2 {:#x} (source {:#x}), gen mask "
                                             "{:#x}, feature bits {:#x}, {:#x} before it took its source's)",
                                             twin.label, source.twin, source.name.data(), waited, *id, *techniques,
                                             *flags, *flags2, *source_flags2, *mask, features, features_before);
                }
            }
            else if (waited > Constants::SHADER_TWIN_TIMEOUT_SECONDS)
            {
                source.state = TwinState::Failed;
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: {} twin {:#x} for {} not parsed after {:.0f} s ({} of {} "
                                         "techniques, flags {:#x}); items with this material keep the stock shader",
                                         twin.label, source.twin, source.name.data(), waited, *techniques,
                                         source.techniques, *flags);
            }
        }
        // The swapped trees refilled with their source while the twin parsed; they refill with it once the set that
        // carries it is published.
        s_refill_all = s_refill_all || newly_ready;
    }

    void revalidate_twins(std::uint64_t now) noexcept
    {
        // The engine's shader reload count, or none while the manager does not check out or the read faults.
        std::optional<std::int32_t> reloads;
        if (const std::uintptr_t address = shader_reload_count_address(); address != 0)
        {
            if (const auto count = DMK::memory::read<std::int32_t>(DMK::Address{address}); count)
            {
                reloads = *count;
            }
        }
        const bool reloaded = !reloads.has_value() || *reloads != s_revalidate_reloads;
        if (!s_revalidate_stale && !reloaded && now - s_revalidate_ms < Constants::SHADER_TWIN_REVALIDATE_SWEEP_MS)
        {
            return;
        }
        if (reloads.has_value() && *reloads != s_revalidate_reloads)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "ShaderTwins: the engine's shader reload count is {} (was {}); the ready twins are "
                "checked against their sources",
                *reloads, s_revalidate_reloads);
        }
        s_revalidate_reloads = reloads.value_or(s_revalidate_reloads);
        s_revalidate_ms = now;
        bool any_stale = false;
        int went_stale = 0;
        int revalidated = 0;
        const Source *first_stale = nullptr;
        ShaderState first_stock{};
        ShaderState first_twin{};
        for (int i = 0; i < s_source_count; ++i)
        {
            Source &source = s_sources[static_cast<std::size_t>(i)];
            if (source.state != TwinState::Ready)
            {
                continue;
            }
            ShaderState stock{};
            ShaderState twin{};
            const bool read =
                read_shader_state_guarded(source.shader, stock) && read_shader_state_guarded(source.twin, twin);
            const bool consistent = read && shader_parsed(stock) && shader_parsed(twin) &&
                                    twin.techniques == stock.techniques && twin.mask == stock.mask &&
                                    ((twin.flags2 ^ stock.flags2) & Constants::SHADER_FLAG2_HAIR) == 0;
            const bool unchanged = consistent && stock.techniques == source.techniques && stock.mask == source.mask &&
                                   reload_bits(stock, twin) == source.reload_bits;
            if (!source.stale)
            {
                // A ready twin also keeps its source's feature bits; a stale one gets them back when it checks out.
                if (unchanged && features_kept(stock, twin))
                {
                    continue;
                }
                source.stale = true;
                source.stale_ms = now;
                ++went_stale;
                if (first_stale == nullptr)
                {
                    first_stale = &source;
                    first_stock = stock;
                    first_twin = twin;
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: {} twin {:#x} for {} went stale (source flags {:#x}, {} techniques, gen mask "
                    "{:#x}, feature bits {:#x}; twin flags {:#x}, {} techniques, gen mask {:#x}, feature bits "
                    "{:#x}; ready with {} techniques, gen mask {:#x}, reloads {:#x}{})",
                    blobs::k_twins[source.kind].label, source.twin, source.name.data(), stock.flags, stock.techniques,
                    stock.mask, stock.features, twin.flags, twin.techniques, twin.mask, twin.features,
                    source.techniques, source.mask, source.reload_bits, read ? "" : "; unreadable");
            }
            else if (!unchanged)
            {
                // Still changing: the wait starts again from what it reads now.
                source.techniques = stock.techniques;
                source.mask = stock.mask;
                source.reload_bits = reload_bits(stock, twin);
                source.stale_ms = now;
            }
            else if (now - source.stale_ms >= Constants::SHADER_TWIN_REVALIDATE_MS)
            {
                std::uint16_t features_before = 0;
                std::uint16_t features = 0;
                if (!match_feature_flags_guarded(source.twin, source.shader, features_before, features))
                {
                    s_guard_faults.fetch_add(1, std::memory_order_relaxed);
                    source.stale_ms = now;
                    continue;
                }
                source.stale = false;
                ++revalidated;
#if defined(TPVCAMERA_DEV_BUILD)
                ++s_usable_generation;
#endif
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: {} twin {:#x} for {} checks out again ({} techniques, gen "
                                         "mask {:#x}, feature bits {:#x})",
                                         blobs::k_twins[source.kind].label, source.twin, source.name.data(),
                                         source.techniques, source.mask, features);
            }
        }
        for (int i = 0; i < s_source_count; ++i)
        {
            const Source &source = s_sources[static_cast<std::size_t>(i)];
            any_stale = any_stale || (source.state == TwinState::Ready && source.stale);
        }
        s_revalidate_stale = any_stale;
        if (went_stale != 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "ShaderTwins: the engine reloaded or changed {} ready twin(s) or their sources (first: {} twin "
                "for {}, source {} techniques, gen mask {:#x}, flags {:#x}; twin {} techniques, gen mask {:#x}, "
                "flags {:#x}); their items keep the stock shader until both check out again",
                went_stale, blobs::k_twins[first_stale->kind].label, first_stale->name.data(), first_stock.techniques,
                first_stock.mask, first_stock.flags, first_twin.techniques, first_twin.mask, first_twin.flags);
        }
        if (revalidated != 0)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Info,
                                     "ShaderTwins: {} reloaded twin(s) check out against their sources again and "
                                     "swap again",
                                     revalidated);
        }
        s_refill_all = s_refill_all || went_stale != 0 || revalidated != 0;
    }

    void recover_cached_twins(std::uint64_t now) noexcept
    {
        if (s_recovery_done || s_resource_lookup == nullptr || s_resource_lock == nullptr ||
            s_shader_container_global == 0 || loading_renderer() == 0)
        {
            return;
        }
        if (s_source_count != 0)
        {
            s_recovery_done = true; // This generation already prepared its own loading transaction.
            return;
        }
        if (!TryAcquireSRWLockShared(s_resource_lock))
        {
            return; // Never stall a camera frame waiting for resource registration/removal.
        }
        DMK_PROFILE_SCOPE("twins.recover_cache");
        std::array<CachedTwin, k_max_sources> entries{};
        int count = 0;
        const bool complete = snapshot_twins_guarded(entries.data(), count);
        ReleaseSRWLockShared(s_resource_lock);
        s_recovery_done = true;
        int recovered = 0;
        for (int i = 0; i < count; ++i)
        {
            CachedTwin &entry = entries[static_cast<std::size_t>(i)];
            const auto &twin = blobs::k_twins[entry.kind];
            std::array<char, 128> stock_name{};
            const std::size_t prefix = std::strlen(twin.source_name);
            const char *suffix = entry.name.data() + std::strlen(twin.twin_name);
            const std::size_t suffix_length = std::strlen(suffix);
            std::uintptr_t stock = 0;
            if (complete && prefix + suffix_length < stock_name.size())
            {
                for (std::size_t n = 0; n < prefix; ++n)
                {
                    stock_name[n] = ascii_lower(twin.source_name[n]);
                }
                std::memcpy(stock_name.data() + prefix, suffix, suffix_length + 1);
                const ResourceName key{prefix + suffix_length, stock_name.size(), stock_name.data()};
                // The lookup acquires the same SRW lock: call it only after releasing the inventory lock.
                stock = lookup_cached_shader_guarded(key);
            }
            const auto flags = DMK::memory::read<std::uint32_t>(DMK::Address{stock + Constants::SHADER_FLAGS_OFFSET});
            const auto mask = DMK::memory::read<std::uint64_t>(DMK::Address{stock + Constants::SHADER_GEN_MASK_OFFSET});
            const auto twin_mask =
                DMK::memory::read<std::uint64_t>(DMK::Address{entry.shader + Constants::SHADER_GEN_MASK_OFFSET});
            const auto techniques =
                DMK::memory::read<std::uint32_t>(DMK::Address{stock + Constants::SHADER_TECHNIQUE_COUNT_OFFSET});
            if (stock != 0 && is_shader(stock) && is_shader(entry.shader) &&
                source_kind_guarded(stock, twin.scope) == entry.kind && find_source(stock, entry.kind) < 0 && flags &&
                (*flags & Constants::SHADER_FLAG_LOADED) != 0 && (*flags & Constants::SHADER_FLAG_NOT_FOUND) == 0 &&
                mask && twin_mask && *mask == *twin_mask && techniques && *techniques != 0)
            {
                Source &source = s_sources[static_cast<std::size_t>(s_source_count++)];
                source = Source{.shader = stock,
                                .kind = entry.kind,
                                .twin = entry.shader,
                                .mask = *mask,
                                .techniques = *techniques,
                                .created_ms = now,
                                .recovered = true};
                copy_text_field_guarded(stock, Constants::SHADER_RESOURCE_NAME_OFFSET, source.name.data(),
                                        source.name.size());
                ++recovered;
            }
            else
            {
                release_candidate_shader(stock);
                release_candidate_shader(entry.shader);
            }
        }
        if (recovered != 0)
        {
            read_dissolve_rt_bit();
        }
        (void)DMK::log().try_log(
            complete ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
            "ShaderTwins: cache recovery retained {} of {} loaded twin(s), inventory {}; no shader creation", recovered,
            count, complete ? "complete" : "faulted");
    }

    void log_render_misses() noexcept
    {
        if (s_inventory_pairs.empty() || !s_render_miss_new.load(std::memory_order_relaxed))
        {
            return;
        }
        std::array<MissReport, 8> reports{};
        std::size_t count = 0;
        try
        {
            const std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                return;
            }
            s_render_miss_new.store(false, std::memory_order_relaxed);
            for (std::size_t i = 0; i < s_candidate_count; ++i)
            {
                TwinCandidate &candidate = s_candidates[i];
                if (candidate.priority != 2 || candidate.reported)
                {
                    continue;
                }
                const MissScope scope = classify_render_miss(candidate);
                if (scope == MissScope::OtherScope || scope == MissScope::NotInLevel)
                {
                    if (count == reports.size())
                    {
                        s_render_miss_new.store(true, std::memory_order_relaxed);
                        break;
                    }
                    reports[count++] = miss_report(candidate, scope);
                }
                candidate.reported = true;
            }
        }
        catch (...)
        {
            return;
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            log_render_miss(reports[i], false);
        }
    }

    [[nodiscard]] PauseReason pause_reason(const TwinPauseSignals &signals) noexcept
    {
        if (!signals.clock_paused || signals.dialogue)
        {
            return PauseReason::None;
        }
        return signals.menu_open ? PauseReason::Menu : signals.overlay_up ? PauseReason::Overlay : PauseReason::Clock;
    }

    void end_pause(std::uint64_t now) noexcept
    {
        PauseState &pause = s_pause;
        if (pause.since_ms == 0)
        {
            return;
        }
        const double lasted = static_cast<double>(now - pause.since_ms) / 1000.0;
        if (pause.last_created_ms != 0 && now - pause.last_created_ms < Constants::SHADER_TWIN_PAUSE_SPACING_MS)
        {
            ++s_pause_overlaps;
            (void)DMK::log().try_log(DMK::LogLevel::Info,
                                     "ShaderTwins: {} ended {} ms after the barrier of a twin created in it; that "
                                     "twin's parse had finished inside the pause",
                                     pause_text(pause.reason), now - pause.last_created_ms);
        }
        if (pause.created != 0)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Info,
                                     "ShaderTwins: {} ended after {:.1f} s with {} twin(s) created in it, {} of "
                                     "them ready once their barrier returned",
                                     pause_text(pause.reason), lasted, pause.created, pause.ready);
        }
        pause = PauseState{.off = pause.off};
    }

    void clear_pause_inventory() noexcept
    {
        InventoryBatch left;
        InventoryBatch done;
        try
        {
            const std::lock_guard lock(s_main_mutex);
            left.entries.swap(s_pause_inventory);
            done.entries.swap(s_pause_inventory_done);
        }
        catch (...)
        {
            // A lock that cannot be taken leaves those references held: the shaders stay alive, nothing breaks.
        }
    }

    void create_during_pause(std::uint64_t now, PauseReason reason) noexcept
    {
        PauseState &pause = s_pause;
        pause.servicing = true;
        struct Servicing
        {
            ~Servicing() noexcept { s_pause.servicing = false; }
        } servicing;
#if defined(TPVCAMERA_DEV_BUILD)
        s_pause_drained = false;
#endif
        // Owned while handled, dropped below outside every mod mutex (an inventory pick moves to its done list).
        TwinCandidate picked{};
        PauseStale stale;
        InventoryBatch handled;
        bool from_inventory = false;
        bool inventory_drained = false;
        const char *origin = "";
        int left = 0;
        int created_at = -1;
        std::uintptr_t renderer = 0;
        try
        {
            const std::unique_lock lock(s_main_mutex, std::try_to_lock);
            if (!lock.owns_lock() || !s_available.load(std::memory_order_acquire) ||
                s_disabled.load(std::memory_order_acquire))
            {
                return;
            }
            renderer = loading_renderer();
            if (renderer == 0)
            {
                return;
            }
            recover_cached_twins(now);
            if (s_inventory_objman_global != 0 &&
                (s_inventory_epoch.load(std::memory_order_relaxed) != s_load_epoch.load(std::memory_order_relaxed) ||
                 s_inventory_rules.load(std::memory_order_relaxed) != Rules::current_generation()))
            {
                pause_inventory(reason);
                // The walk was this frame's work; the first creation comes after the spacing.
                pause.next_ms = GetTickCount64() + Constants::SHADER_TWIN_PAUSE_SPACING_MS;
                return;
            }
            if (!prepare(now))
            {
                return;
            }
            refresh_twins(now);
            if (s_source_count >= k_max_sources)
            {
                if (!std::exchange(pause.full_logged, true))
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "ShaderTwins: all {} source slots are in use; this pause creates no "
                                             "twin",
                                             k_max_sources);
                }
#if defined(TPVCAMERA_DEV_BUILD)
                s_pause_drained = true;
#endif
                return;
            }
            const bool had_inventory = !s_pause_inventory.empty();
            const bool taken = take_pause_pick(picked, from_inventory, left, stale);
            inventory_drained = had_inventory && s_pause_inventory.empty();
            if (!taken)
            {
                // Nothing to create now (or the pool was busy): look again after the spacing.
                pause.next_ms = now + Constants::SHADER_TWIN_PAUSE_SPACING_MS;
#if defined(TPVCAMERA_DEV_BUILD)
                s_pause_drained = true;
#endif
            }
            else
            {
                origin = from_inventory ? "the pause inventory" : candidate_origin(picked, false);
                s_pause_permit = true;
                const int before = s_source_count;
                create_twin(picked.request, now);
                s_pause_permit = false;
                created_at = s_source_count > before ? before : -1;
                if (from_inventory)
                {
                    // Its room was reserved with the inventory, so this does not allocate.
                    s_pause_inventory_done.push_back(std::exchange(picked, TwinCandidate{}));
                }
            }
            if (inventory_drained && created_at < 0)
            {
                finish_pause_inventory(handled);
            }
        }
        catch (...)
        {
            s_pause_permit = false;
        }
        if (created_at < 0)
        {
            // Nothing was created, so nothing was parsed: the candidate goes (handled, or not a source any more).
            drop_candidate(picked);
            return;
        }
        // The twin's parse, queued by mfForName, runs on the render thread before this paused frame goes on.
        const std::uint64_t barrier_started = GetTickCount64();
        const bool completed = flush_loading_commands(renderer);
        const std::uint64_t finished = GetTickCount64();
        drop_candidate(picked);
        pause.next_ms = finished + Constants::SHADER_TWIN_PAUSE_SPACING_MS;
        pause.last_created_ms = finished;
        ++pause.created;
        ++s_pause_created_total;
        if (!completed)
        {
            pause.off = true;
            (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                     "ShaderTwins: the render-command barrier after a twin created during {} did "
                                     "not complete, so its parse may still run after the pause; no more twins are "
                                     "created during pauses this session (loading screens still create them)",
                                     pause_text(reason));
        }
        try
        {
            const std::lock_guard lock(s_main_mutex);
            if (!s_available.load(std::memory_order_acquire))
            {
                return;
            }
            refresh_twins(finished);
            const Source &source = s_sources[static_cast<std::size_t>(created_at)];
            const bool ready = source.state == TwinState::Ready;
            pause.ready += ready ? 1 : 0;
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "ShaderTwins: {} twin requested for {} (gen mask {:#x}) from {} during {} ({:.1f} s into it); its "
                "barrier took {} ms and it is {}; {} candidate(s) left",
                blobs::k_twins[source.kind].label, source.name.data(), source.mask, origin, pause_text(reason),
                static_cast<double>(barrier_started - pause.since_ms) / 1000.0, finished - barrier_started,
                ready                                ? "ready"
                : source.state == TwinState::Pending ? "still parsing"
                                                     : "given up",
                left);
            if (inventory_drained)
            {
                finish_pause_inventory(handled);
            }
        }
        catch (...)
        {
            // The twin's state is polled again next frame; only its log line is lost.
        }
    }

#if defined(TPVCAMERA_DEV_BUILD)

    void service_paused_dry_run(PauseReason reason) noexcept
    {
        if constexpr (TwinClone::k_stage == TwinClone::Stage::DryRun)
        {
            if (!std::exchange(s_pause_drained, false) || !TwinClone::dry_run_available())
            {
                return;
            }
            try
            {
                std::vector<TwinClone::DryRunPair> pairs;
                {
                    const std::unique_lock lock(s_main_mutex, std::try_to_lock);
                    if (!lock.owns_lock() || !s_available.load(std::memory_order_acquire) ||
                        s_disabled.load(std::memory_order_acquire))
                    {
                        return;
                    }
                    pairs = dry_run_pairs(true);
                }
                (void)TwinClone::queue_dry_run(pairs, pause_text(reason));
            }
            catch (...)
            {
                // A list that could not allocate is taken again in the next paused frame with nothing to create.
            }
            if (!TwinClone::dry_run_pending())
            {
                return;
            }
            TwinClone::run_dry_run(Constants::TWIN_CLONE_DRY_RUN_PAUSE_US, Constants::TWIN_CLONE_DRY_RUN_PAUSE_PAIRS,
                                   pause_text(reason));
            if (TwinClone::dry_run_pending())
            {
                s_pause.next_ms = GetTickCount64();
            }
        }
    }
#endif

    void log_pause_waits(std::uint64_t now) noexcept
    {
        int waiting = 0;
        int drawn = 0;
        std::uint64_t oldest = now;
        try
        {
            const std::unique_lock lock(s_candidate_mutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                return;
            }
            for (std::size_t i = 0; i < s_candidate_count; ++i)
            {
                const TwinCandidate &candidate = s_candidates[i];
                // The pause creates a plant or frame variant, or one a render job drew; another character's,
                // and one of a kind that is off, waits for the next boundary.
                if ((!listed_kind(candidate.request.kind) && candidate.priority != 2) ||
                    kind_off(candidate.request.kind) ||
                    find_source(candidate.request.shader, candidate.request.kind) >= 0)
                {
                    continue;
                }
                ++waiting;
                drawn += candidate.priority == 2 ? 1 : 0;
                oldest = candidate.first_ms != 0 ? std::min(oldest, candidate.first_ms) : oldest;
            }
        }
        catch (...)
        {
            return;
        }
        const int inventory_left = static_cast<int>(s_pause_inventory.size());
        for (const TwinCandidate &candidate : s_pause_inventory)
        {
            oldest = candidate.first_ms != 0 ? std::min(oldest, candidate.first_ms) : oldest;
        }
        const std::uint32_t captures = s_playtime_captures.exchange(0, std::memory_order_relaxed);
        if (waiting == s_logged_waiting && inventory_left == s_logged_inventory_left && captures == 0 &&
            s_pause_created_total == s_logged_pause_created && s_pause_overlaps == s_logged_overlaps)
        {
            return;
        }
        s_logged_waiting = waiting;
        s_logged_inventory_left = inventory_left;
        s_logged_pause_created = s_pause_created_total;
        s_logged_overlaps = s_pause_overlaps;
        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                 "ShaderTwins: {} variant(s) wait for a pause or loading screen ({} drawn without "
                                 "their twin, {} from the pause inventory; oldest {:.1f} s); {} play-time "
                                 "capture(s) in the last second; {} twin(s) created during pauses, {} pause(s) "
                                 "ended within {} ms of a creation",
                                 waiting + inventory_left, drawn, inventory_left,
                                 static_cast<double>(now - oldest) / 1000.0, captures, s_pause_created_total,
                                 s_pause_overlaps, Constants::SHADER_TWIN_PAUSE_SPACING_MS);
    }

    std::uintptr_t __fastcall load_cgf_material_detour(std::uintptr_t manager, const char *material, const char *asset,
                                                       std::uint32_t flags) noexcept
    {
        const DetourScope detour_scope;
        const auto original = s_load_cgf_material_original.load(std::memory_order_acquire);
        const auto slot = s_asset_scope_tls;
        struct AssetScope
        {
            DWORD slot;
            void *previous;
            ~AssetScope() noexcept
            {
                if (slot != TLS_OUT_OF_INDEXES)
                {
                    (void)TlsSetValue(slot, previous);
                }
            }
        } scope{slot, slot != TLS_OUT_OF_INDEXES ? TlsGetValue(slot) : nullptr};
        if (slot != TLS_OUT_OF_INDEXES && s_available.load(std::memory_order_acquire))
        {
            const auto bits = reinterpret_cast<std::uintptr_t>(scope.previous) | twin_asset_scopes(asset);
            (void)TlsSetValue(slot, reinterpret_cast<void *>(bits));
        }
        return original(manager, material, asset, flags);
    }

    bool __fastcall load_material_shader_detour(std::uintptr_t material, std::uintptr_t parent, const char *shader,
                                                std::uint64_t mask, std::uintptr_t resources,
                                                std::uintptr_t public_params) noexcept
    {
        const DetourScope detour_scope;
        const auto original = s_load_material_shader_original.load(std::memory_order_acquire);
        const bool result = original(material, parent, shader, mask, resources, public_params);
        if (!result || !s_available.load(std::memory_order_acquire))
        {
            return result;
        }
        try
        {
            // Inside a loading screen every kind is captured; during play only a plant or frame variant, which
            // waits for the next pause (service_shader_twins_paused()): the character's twins come from loading
            // screens and from his own drawn items, so another character's eyes and hair never spend a pause's
            // parse.
            const bool loading = loading_materials_admitted();
            // ShaderItem contains only integers and bytes. DMK's typed reader deliberately excludes structs;
            // copy its representation into a local item while this material-load callback owns the material.
            ShaderItem item{};
            if (!DMK::memory::read_into(DMK::Address{material + Constants::MATERIAL_SHADER_ITEM_OFFSET},
                                        std::as_writable_bytes(std::span{&item, 1})) ||
                item.shader == 0)
            {
                return result;
            }
            std::uintptr_t scopes = twin_material_scopes(parent) | twin_material_scopes(material);
            if (s_asset_scope_tls != TLS_OUT_OF_INDEXES)
            {
                scopes |= reinterpret_cast<std::uintptr_t>(TlsGetValue(s_asset_scope_tls));
            }
            if (loading)
            {
                for_each_twin_kind(item, scopes, true,
                                   [&item](int kind)
                                   {
                                       (void)collect_candidate(
                                           item, kind,
                                           blobs::k_twins[kind].scope == blobs::TwinScope::Dissolving ? 1 : 0, true);
                                   });
                return result;
            }
            for_each_twin_kind(item, scopes & (k_scope_plants | k_scope_frames), false,
                               [&item, material](int kind)
                               {
                                   if (!playtime_capture_wanted(item.shader, kind) ||
                                       !collect_candidate(item, kind, 0, false))
                                   {
                                       return;
                                   }
                                   s_playtime_captures.fetch_add(1, std::memory_order_relaxed);
                                   log_playtime_capture(item, kind, material);
                               });
        }
        catch (...)
        {
            s_dropped_requests.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    void __fastcall set_system_state_detour(std::uintptr_t system, std::int32_t state) noexcept
    {
        const DetourScope detour_scope;
        const auto original = s_set_system_state_original.load(std::memory_order_acquire);
        // A bad/off-thread close can never authorize preparation, but it must still close admission. Otherwise
        // an unexpected engine transition could leave loader callbacks admitted during gameplay.
        if (state < 1 || state > 9)
        {
            s_level_loading.store(false, std::memory_order_release);
        }
        bool valid = false;
        bool opening = false;
        try
        {
            const auto expected =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_SYSTEM_OFFSET});
            const auto previous =
                DMK::memory::read<std::int32_t>(DMK::Address{system + Constants::SYSTEM_GLOBAL_STATE_OFFSET});
            valid = s_available.load(std::memory_order_acquire) && expected && *expected == system && previous &&
                    *previous >= 0 && *previous <= 13 && state >= 0 && state <= 13 && loading_renderer() != 0;
            if (valid && !s_loading_finalizing.load(std::memory_order_acquire))
            {
                opening = state >= 1 && state <= 9;
                if (!opening)
                {
                    s_level_loading.store(false, std::memory_order_release);
                    if (*previous >= 1 && *previous <= 9 && state >= 10)
                    {
                        finalize_loading("level load");
                    }
                }
            }
        }
        catch (...)
        {
            s_level_loading.store(false, std::memory_order_release);
        }
        original(system, state);
        if (valid && opening && !s_loading_finalizing.load(std::memory_order_acquire))
        {
            if (!s_level_loading.exchange(true, std::memory_order_acq_rel))
            {
                s_load_epoch.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    bool __fastcall save_game_load_detour(std::uintptr_t manager, std::uintptr_t chunk,
                                          std::uintptr_t save_path) noexcept
    {
        const DetourScope detour_scope;
        const auto original = s_save_game_load_original.load(std::memory_order_acquire);
        const bool admitted = s_available.load(std::memory_order_acquire) && loading_renderer() != 0 &&
                              !s_loading_finalizing.load(std::memory_order_acquire);
        struct SaveScope
        {
            bool admitted;
            std::uintptr_t previous;
            ~SaveScope() noexcept
            {
                if (admitted)
                {
                    s_loading_save_manager.store(previous, std::memory_order_release);
                }
            }
        } scope{admitted, admitted ? s_loading_save_manager.exchange(manager, std::memory_order_acq_rel) : 0};
        if (admitted)
        {
            s_load_epoch.fetch_add(1, std::memory_order_relaxed);
        }
        const bool result = original(manager, chunk, save_path);
        // This wrapper's caller still owns the loading screen, including the failed-load return path. The
        // manager's engine-loading byte is already clear here; it only admits material callbacks inside original.
        if (admitted)
        {
            finalize_loading("engine save load");
        }
        return result;
    }

    void queue_release(const Track &track, const char *why) noexcept
    {
        if (s_released_count < static_cast<int>(s_released.size()))
        {
            Track &released = s_released[static_cast<std::size_t>(s_released_count++)];
            released = track;
            released.why = why;
        }
    }
} // namespace TPVCamera::Twins
