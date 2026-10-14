/**
 * @file dev/twin_clone/twin_clone.cpp
 * @brief Implementation of the DEV twin clone's dry-run (see twin_clone.hpp). A release build compiles none of it.
 *
 * @details Every engine read copies plain bytes into the mod's own memory under SEH, so a layout drift or an object the
 *          engine is freeing reads as a fault that skips the pair, never a crash, and the copies are then read without
 *          SEH. A HW shader's token map is copied under the HW shader factory's SRWLOCK, taken shared and never waited
 *          for: the factory writes a map only while it holds the lock exclusively, and a busy lock leaves the pair for
 *          a later step. The only engine calls are that lock and the resource lookup (GetResource without a reference,
 *          which takes the registry's own lock shared); the dry-run never arms, creates or writes anything. Class
 *          checks go through RTTI once per class: the first vtable proven a CShader or a CHWShader_D3D is compared from
 *          then on. Everything runs on the main thread, so the queue, the tallies and the buffers need no lock.
 */

#include "twin_clone.hpp"

#if defined(TPVCAMERA_DEV_BUILD)

#include "aob_resolver.hpp"
#include "dev/hook_profile.hpp"
#include "global_state.hpp"
#include "shader_token_diff.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace TPVCamera::TwinClone
{

    namespace
    {
        namespace blobs = shader_twin_blobs;

        static_assert(sizeof(ShaderSplice::StringHeader) == Constants::CRY_STRING_HEADER_SIZE,
                      "The splice's strings must carry the CryStringT header the factory reads.");
        static_assert(Constants::TECHNIQUE_SHADERS_OFFSET + Constants::TECHNIQUE_SHADER_SLOTS * 8 <=
                              Constants::TECHNIQUE_FLAGS_OFFSET &&
                          Constants::TECHNIQUE_ELEMENTS_OFFSET + Constants::TECHNIQUE_ELEMENTS_SIZE <=
                              Constants::TECHNIQUE_CRC_OFFSET &&
                          Constants::TECHNIQUE_INDICES_OFFSET + Constants::TECHNIQUE_INDICES_SIZE <=
                              Constants::TECHNIQUE_SIZE,
                      "The technique fields must lie inside SShaderTechnique.");
        static_assert(Constants::SHADER_RUNTIME_FLOAT_OFFSET + 4 <= Constants::SHADER_SIZE &&
                          Constants::HW_SHADER_CACHE_OFFSET + 8 <= Constants::HW_SHADER_SIZE,
                      "The fields read must lie inside CShader and CHWShader_D3D.");

        // CBaseResource::GetResource(&class CRC, &name, add a reference) -> the resource or 0 (ResourceLookup); the
        // name is (length, capacity, char *), of which the lookup hashes the text.
        struct ResourceName
        {
            std::size_t length;
            std::size_t capacity;
            const char *data;
        };
        using ResourceLookupFn = std::uintptr_t(__fastcall *)(const std::uint32_t *, const ResourceName *, bool);

        // Resolved at init, read-only afterward.
        ResourceLookupFn s_lookup = nullptr;
        PSRWLOCK s_factory_lock = nullptr;
        bool s_available = false;

        using Clock = std::chrono::steady_clock;

        [[nodiscard]] std::uint64_t micros_since(Clock::time_point started) noexcept
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
        }

        [[nodiscard]] constexpr double millis(std::uint64_t micros) noexcept
        {
            return static_cast<double>(micros) / 1000.0;
        }

        [[nodiscard]] constexpr char lower(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        [[nodiscard]] constexpr bool equals_nocase(std::string_view a, std::string_view b) noexcept
        {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
        }

        /** @brief The family CRC a twin's name ends with ('_' and eight hex digits): its binary's header CRC, or 0. */
        [[nodiscard]] constexpr std::uint32_t family_crc(std::string_view name) noexcept
        {
            const auto at = name.rfind('_');
            if (at == std::string_view::npos || name.size() - at - 1 != 8)
            {
                return 0;
            }
            std::uint32_t crc = 0;
            for (const char c : name.substr(at + 1))
            {
                const char l = lower(c);
                const int digit = l >= '0' && l <= '9' ? l - '0' : l >= 'a' && l <= 'f' ? l - 'a' + 10 : -1;
                if (digit < 0)
                {
                    return 0;
                }
                crc = (crc << 4) | static_cast<std::uint32_t>(digit);
            }
            return crc;
        }
        static_assert(family_crc("KCD2_TPVCamera_Vegetation_5dba6d8a") == 0x5DBA6D8Au);

        /** @brief The clone splice of @p kind (an index in k_twins), or nullptr for a kind the clone cannot derive. */
        [[nodiscard]] const blobs::CloneSplice *splice_of(int kind) noexcept
        {
            if (kind < 0 || kind >= static_cast<int>(std::size(blobs::k_twins)))
            {
                return nullptr;
            }
            return ShaderSplice::find_kind(blobs::k_twins[kind].label);
        }

        /** @brief A field of a copied object. */
        template <typename T, std::size_t N>
        [[nodiscard]] T field(const std::array<std::uint8_t, N> &bytes, std::ptrdiff_t offset) noexcept
        {
            T value;
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            return value;
        }

        /** @brief True when @p size bytes of a copied object at @p offset are all zero. */
        template <std::size_t N>
        [[nodiscard]] bool zeroed(const std::array<std::uint8_t, N> &bytes, std::ptrdiff_t offset,
                                  std::size_t size) noexcept
        {
            return std::all_of(bytes.begin() + offset, bytes.begin() + offset + static_cast<std::ptrdiff_t>(size),
                               [](std::uint8_t b) { return b == 0; });
        }

        /** @brief True when two copied objects hold the same @p size bytes at @p offset. */
        template <std::size_t N>
        [[nodiscard]] bool same_bytes(const std::array<std::uint8_t, N> &a, const std::array<std::uint8_t, N> &b,
                                      std::ptrdiff_t offset, std::size_t size) noexcept
        {
            return std::memcmp(a.data() + offset, b.data() + offset, size) == 0;
        }

        /** @brief Copies @p size bytes at @p address into @p out. False on a fault. SEH-guarded, POD body. */
        [[nodiscard]] bool copy_guarded(void *out, std::uintptr_t address, std::size_t size) noexcept
        {
            __try
            {
                std::memcpy(out, reinterpret_cast<const void *>(address), size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /**
         * @brief Copies the NUL-terminated text at @p text into @p out. False, with @p out empty, for a null text, one
         *        that does not fit, or a fault. SEH-guarded, POD body.
         */
        [[nodiscard]] bool copy_text_guarded(const char *text, char *out, std::size_t capacity) noexcept
        {
            __try
            {
                if (text != nullptr)
                {
                    for (std::size_t i = 0; i < capacity; ++i)
                    {
                        out[i] = text[i];
                        if (text[i] == '\0')
                        {
                            return true;
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            out[0] = '\0';
            return false;
        }

        template <std::size_t N> bool copy_text(std::uintptr_t text, std::array<char, N> &out) noexcept
        {
            return copy_text_guarded(reinterpret_cast<const char *>(text), out.data(), out.size());
        }

        /** @brief A class proven by RTTI once (a loader query), then by comparing vtables. Main thread only. */
        struct ProvenType
        {
            std::string_view name;
            std::uintptr_t vtable = 0;
            std::uintptr_t rejected = 0;

            [[nodiscard]] bool is(std::uintptr_t candidate) noexcept
            {
                if (candidate == 0 || candidate == rejected)
                {
                    return false;
                }
                if (candidate == vtable)
                {
                    return true;
                }
                if (!DMK::rtti::vtable_is_type(DMK::Address{candidate}, name))
                {
                    rejected = candidate;
                    return false;
                }
                if (vtable == 0)
                {
                    vtable = candidate;
                }
                return true;
            }
        };
        ProvenType s_shader_type{Constants::CSHADER_RTTI_NAME};
        ProvenType s_hardware_type{Constants::CHWSHADER_RTTI_NAME};

        /** @brief A CShader as copied: its bytes, its techniques (up to k_max_techniques), its file name and name. */
        struct ShaderSnapshot
        {
            std::array<std::uint8_t, Constants::SHADER_SIZE> bytes{};
            std::uint32_t techniques = 0; // as the shader counts them
            std::array<std::uintptr_t, k_max_techniques> technique_ptrs{};
            std::array<std::array<std::uint8_t, Constants::TECHNIQUE_SIZE>, k_max_techniques> technique_bytes{};
            std::array<char, 160> file{};
            std::array<char, 96> name{};
        };

        /** @brief Copies @p shader and its techniques. False on a fault. SEH-guarded, POD body. */
        [[nodiscard]] bool snapshot_shader_guarded(std::uintptr_t shader, ShaderSnapshot &out) noexcept
        {
            __try
            {
                std::memcpy(out.bytes.data(), reinterpret_cast<const void *>(shader), Constants::SHADER_SIZE);
                std::uint32_t count = 0;
                std::uintptr_t array = 0;
                std::memcpy(&count, out.bytes.data() + Constants::SHADER_TECHNIQUE_COUNT_OFFSET, sizeof(count));
                std::memcpy(&array, out.bytes.data() + Constants::SHADER_TECHNIQUES_OFFSET, sizeof(array));
                out.techniques = count;
                const auto limit = static_cast<std::uint32_t>(k_max_techniques);
                const std::uint32_t copied = count < limit ? count : limit;
                for (std::uint32_t i = 0; i < copied; ++i)
                {
                    const std::uintptr_t technique = reinterpret_cast<const std::uintptr_t *>(array)[i];
                    out.technique_ptrs[i] = technique;
                    if (technique != 0)
                    {
                        std::memcpy(out.technique_bytes[i].data(), reinterpret_cast<const void *>(technique),
                                    Constants::TECHNIQUE_SIZE);
                    }
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief Copies @p shader and its techniques, then its file name and name. False on a fault. */
        [[nodiscard]] bool snapshot_shader(std::uintptr_t shader, ShaderSnapshot &out) noexcept
        {
            out.technique_ptrs.fill(0);
            if (!snapshot_shader_guarded(shader, out))
            {
                return false;
            }
            (void)copy_text(field<std::uintptr_t>(out.bytes, Constants::SHADER_FILE_NAME_OFFSET), out.file);
            (void)copy_text(field<std::uintptr_t>(out.bytes, Constants::SHADER_NAME_OFFSET), out.name);
            return true;
        }

        /** @brief A CHWShader_D3D as copied, with its resource name, name source and entry. */
        struct HwSnapshot
        {
            std::array<std::uint8_t, Constants::HW_SHADER_SIZE> bytes{};
            std::array<char, 256> key{};
            std::array<char, 160> source{};
            std::array<char, 64> entry{};
        };

        /** @brief Copies @p shader and its three names. False on a fault. */
        [[nodiscard]] bool snapshot_hw(std::uintptr_t shader, HwSnapshot &out) noexcept
        {
            if (!copy_guarded(out.bytes.data(), shader, out.bytes.size()))
            {
                return false;
            }
            (void)copy_text(field<std::uintptr_t>(out.bytes, Constants::HW_SHADER_KEY_OFFSET), out.key);
            (void)copy_text(field<std::uintptr_t>(out.bytes, Constants::HW_SHADER_NAME_SOURCE_OFFSET), out.source);
            (void)copy_text(field<std::uintptr_t>(out.bytes, Constants::HW_SHADER_ENTRY_OFFSET), out.entry);
            return true;
        }

        enum class MapRead : std::uint8_t
        {
            Read,
            Busy,
            Bad, // empty, past ShaderTokens::k_max_bytes, or past its capacity
            Fault,
        };

        /**
         * @brief Copies @p shader's token map (a std::string at Constants::HW_SHADER_TOKENS_OFFSET) into @p out.
         * @note Only while the factory's lock is held shared (copy_map()). SEH-guarded, POD body.
         */
        [[nodiscard]] MapRead copy_map_guarded(std::uintptr_t shader, std::uint8_t *out, std::size_t capacity,
                                               std::size_t &size) noexcept
        {
            __try
            {
                const auto bytes =
                    *reinterpret_cast<const std::size_t *>(shader + Constants::HW_SHADER_TOKENS_SIZE_OFFSET);
                const auto allocated =
                    *reinterpret_cast<const std::size_t *>(shader + Constants::HW_SHADER_TOKENS_CAPACITY_OFFSET);
                if (bytes < 4 || bytes > capacity || bytes > allocated)
                {
                    return MapRead::Bad;
                }
                const std::uintptr_t data =
                    allocated > Constants::HW_SHADER_TOKENS_INLINE_CAPACITY
                        ? *reinterpret_cast<const std::uintptr_t *>(shader + Constants::HW_SHADER_TOKENS_OFFSET)
                        : shader + Constants::HW_SHADER_TOKENS_OFFSET;
                std::memcpy(out, reinterpret_cast<const void *>(data), bytes);
                size = bytes;
                return MapRead::Read;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return MapRead::Fault;
            }
        }

        /** @brief A HW shader's token map, copied out; its buffer holds ShaderTokens::k_max_bytes once initialized. */
        struct MapCopy
        {
            std::vector<std::uint8_t> bytes;
            std::size_t size = 0;

            [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return {bytes.data(), size}; }
        };

        /**
         * @brief Copies @p shader's token map under the factory's lock, taken shared and never waited for: the factory
         *        replaces a map only while it holds the lock exclusively.
         */
        [[nodiscard]] MapRead copy_map(std::uintptr_t shader, MapCopy &out) noexcept
        {
            out.size = 0;
            if (out.bytes.size() < ShaderTokens::k_max_bytes)
            {
                return MapRead::Bad;
            }
            if (!TryAcquireSRWLockShared(s_factory_lock))
            {
                return MapRead::Busy;
            }
            const MapRead read = copy_map_guarded(shader, out.bytes.data(), out.bytes.size(), out.size);
            ReleaseSRWLockShared(s_factory_lock);
            return read;
        }

        /**
         * @brief The resource of class @p class_crc named @p name, without a reference: an identity to compare, never
         *        read through. 0 for none or a fault. SEH-guarded, POD body.
         */
        [[nodiscard]] std::uintptr_t lookup_guarded(std::uint32_t class_crc, const char *name,
                                                    std::size_t length) noexcept
        {
            __try
            {
                const ResourceName key{length, length, name};
                return s_lookup(&class_crc, &key, false);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        template <std::size_t N>
        [[nodiscard]] std::uintptr_t lookup(std::uint32_t class_crc, const std::array<char, N> &name) noexcept
        {
            return lookup_guarded(class_crc, name.data(), std::string_view{name.data()}.size());
        }

        /**
         * @brief Writes lower("<name>@<entry>(<platform>)"), and "(<mask in hex>)" after it when @p with_mask and the
         *        mask is not zero: the name the HW shader factory gives a pass (sub_1807E8740). False when it does not
         *        fit.
         */
        [[nodiscard]] bool factory_key(std::array<char, 256> &out, std::string_view name, std::string_view entry,
                                       std::string_view platform, std::uint64_t mask, bool with_mask) noexcept
        {
            const auto base = std::format_to_n(out.data(), out.size() - 1, "{}@{}({})", name, entry, platform);
            auto used = static_cast<std::size_t>(base.size);
            if (used >= out.size() - 1)
            {
                out[0] = '\0';
                return false;
            }
            std::transform(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(used), out.begin(), lower);
            if (with_mask && mask != 0)
            {
                const auto tail = std::format_to_n(out.data() + used, out.size() - 1 - used, "({:x})", mask);
                if (used + static_cast<std::size_t>(tail.size) >= out.size() - 1)
                {
                    out[0] = '\0';
                    return false;
                }
                used += static_cast<std::size_t>(tail.size);
            }
            out[used] = '\0';
            return true;
        }

        /**
         * @brief The platform part of a pass's resource name, read from the stock pass's own name, which must be
         *        exactly lower("<name>@<entry>(<platform>)") with "(<HW mask in hex>)" after it when the mask is not
         *        zero. Empty when it is not.
         */
        [[nodiscard]] std::string_view key_platform(std::string_view key, std::string_view name, std::string_view entry,
                                                    std::uint64_t mask) noexcept
        {
            std::size_t at = 0;
            const auto expect = [&key, &at](std::string_view text) noexcept
            {
                for (const char c : text)
                {
                    if (at >= key.size() || key[at] != lower(c))
                    {
                        return false;
                    }
                    ++at;
                }
                return true;
            };
            if (!expect(name) || !expect("@") || !expect(entry) || !expect("("))
            {
                return {};
            }
            const auto close = key.find(')', at);
            if (close == std::string_view::npos || close == at)
            {
                return {};
            }
            std::array<char, 24> suffix{};
            std::size_t length = 0;
            if (mask != 0)
            {
                length =
                    static_cast<std::size_t>(std::format_to_n(suffix.data(), suffix.size() - 1, "({:x})", mask).size);
            }
            if (key.substr(close + 1) != std::string_view{suffix.data(), std::min(length, suffix.size() - 1)})
            {
                return {};
            }
            return key.substr(at, close - at);
        }

        /** @brief Empties @p plan for @p stock and @p kind, keeping its buffers' capacity. */
        void reset(ClonePlan &plan, std::uintptr_t stock, int kind) noexcept
        {
            plan.status = PlanStatus::Fault;
            plan.stock = stock;
            plan.kind = kind;
            plan.family_crc = 0;
            plan.stock_flags = 0;
            plan.scalars = ShaderScalars{};
            plan.techniques = 0;
            plan.zpass_of.fill(-1);
            plan.zpass_count = 0;
            for (ZPassPlan &zpass : plan.zpasses)
            {
                zpass.stock_shader = 0;
                zpass.key[0] = '\0';
                zpass.cache_key[0] = '\0';
                zpass.hw_mask = 0;
                zpass.splice = ShaderSplice::Plan{};
                zpass.splice_status = ShaderSplice::Status::Ok;
                zpass.derived.clear();
            }
            plan.micros = 0;
        }

        // The objects and token maps a pair is read into. Main thread only; the maps' buffers are sized at init.
        ShaderSnapshot s_stock_snapshot;
        ShaderSnapshot s_twin_snapshot;
        HwSnapshot s_hw_stock;
        HwSnapshot s_hw_twin;
        MapCopy s_map_stock;
        MapCopy s_map_twin;

        /**
         * @brief Records what a compare found: the first mismatch, or the reason it was not compared, wins the detail;
         *        else the first allowed difference.
         */
        class Findings
        {
        public:
            explicit Findings(CompareResult &result) noexcept : m_result(result) {}

            /// True once the compare can stop: a mismatch or a skip was found.
            [[nodiscard]] bool done() const noexcept
            {
                return m_result.verdict == Verdict::Mismatch || m_result.verdict == Verdict::Skipped;
            }

            template <typename... Args> void mismatch(std::format_string<Args...> format, Args &&...args) noexcept
            {
                if (!done())
                {
                    write(format, std::forward<Args>(args)...);
                    m_result.verdict = Verdict::Mismatch;
                }
            }

            template <typename... Args> void skip(std::format_string<Args...> format, Args &&...args) noexcept
            {
                if (!done())
                {
                    write(format, std::forward<Args>(args)...);
                    m_result.verdict = Verdict::Skipped;
                }
            }

            template <typename... Args>
            void allow(std::uint32_t bit, std::format_string<Args...> format, Args &&...args) noexcept
            {
                if (m_result.allowed == 0 && !done())
                {
                    write(format, std::forward<Args>(args)...);
                }
                m_result.allowed |= bit;
            }

        private:
            template <typename... Args> void write(std::format_string<Args...> format, Args &&...args) noexcept
            {
                auto &detail = m_result.detail;
                const auto end =
                    std::format_to_n(detail.data(), detail.size() - 1, format, std::forward<Args>(args)...);
                detail[std::min(static_cast<std::size_t>(end.size), detail.size() - 1)] = '\0';
            }

            CompareResult &m_result;
        };

        /** @brief The entry a HW snapshot names, for messages. */
        [[nodiscard]] std::string_view entry_of(const HwSnapshot &shader) noexcept
        {
            return shader.entry[0] != '\0' ? std::string_view{shader.entry.data()} : std::string_view{"?"};
        }

        /**
         * @brief Compares the permutation fields of a parsed HW pass with what a plan or its stock pass says it holds:
         *        class, entry, RT masks, HW mask, flags without WASGENERATED, and type.
         */
        void compare_permutation(Findings &findings, const HwSnapshot &want, const HwSnapshot &got, int technique,
                                 int slot, std::string_view whose) noexcept
        {
            namespace C = Constants;
            const auto u8 = [](const HwSnapshot &s, std::ptrdiff_t o) { return field<std::uint8_t>(s.bytes, o); };
            const auto u32 = [](const HwSnapshot &s, std::ptrdiff_t o) { return field<std::uint32_t>(s.bytes, o); };
            const auto u64 = [](const HwSnapshot &s, std::ptrdiff_t o) { return field<std::uint64_t>(s.bytes, o); };
            if (std::string_view{want.entry.data()} != std::string_view{got.entry.data()})
            {
                findings.mismatch("technique {} slot {}: entry {} (the {} {})", technique, slot, entry_of(got), whose,
                                  entry_of(want));
            }
            else if (u8(want, C::HW_SHADER_CLASS_OFFSET) != u8(got, C::HW_SHADER_CLASS_OFFSET))
            {
                findings.mismatch("technique {} slot {} ({}): class {} (the {} {})", technique, slot, entry_of(got),
                                  u8(got, C::HW_SHADER_CLASS_OFFSET), whose, u8(want, C::HW_SHADER_CLASS_OFFSET));
            }
            else if (u64(want, C::HW_SHADER_RT_AND_OFFSET) != u64(got, C::HW_SHADER_RT_AND_OFFSET) ||
                     u64(want, C::HW_SHADER_RT_OR_OFFSET) != u64(got, C::HW_SHADER_RT_OR_OFFSET))
            {
                findings.mismatch("technique {} slot {} ({}): RT masks {:#x}/{:#x} (the {} {:#x}/{:#x})", technique,
                                  slot, entry_of(got), u64(got, C::HW_SHADER_RT_AND_OFFSET),
                                  u64(got, C::HW_SHADER_RT_OR_OFFSET), whose, u64(want, C::HW_SHADER_RT_AND_OFFSET),
                                  u64(want, C::HW_SHADER_RT_OR_OFFSET));
            }
            else if (u64(want, C::HW_SHADER_MASK_OFFSET) != u64(got, C::HW_SHADER_MASK_OFFSET))
            {
                findings.mismatch("technique {} slot {} ({}): HW mask {:#x} (the {} {:#x})", technique, slot,
                                  entry_of(got), u64(got, C::HW_SHADER_MASK_OFFSET), whose,
                                  u64(want, C::HW_SHADER_MASK_OFFSET));
            }
            else if (((u32(want, C::HW_SHADER_FLAGS_OFFSET) ^ u32(got, C::HW_SHADER_FLAGS_OFFSET)) &
                      ~C::HW_SHADER_FLAG_WAS_GENERATED) != 0)
            {
                findings.mismatch("technique {} slot {} ({}): flags {:#x} (the {} {:#x}, WASGENERATED aside)",
                                  technique, slot, entry_of(got), u32(got, C::HW_SHADER_FLAGS_OFFSET), whose,
                                  u32(want, C::HW_SHADER_FLAGS_OFFSET));
            }
            else if (u32(want, C::HW_SHADER_TYPE_OFFSET) != u32(got, C::HW_SHADER_TYPE_OFFSET))
            {
                findings.mismatch("technique {} slot {} ({}): type {} (the {} {})", technique, slot, entry_of(got),
                                  u32(got, C::HW_SHADER_TYPE_OFFSET), whose, u32(want, C::HW_SHADER_TYPE_OFFSET));
            }
        }

        /** @brief Reads a token map for a compare; a busy lock marks the result busy and skips it. */
        [[nodiscard]] bool read_map(Findings &findings, CompareResult &result, std::uintptr_t shader, MapCopy &out,
                                    std::string_view what) noexcept
        {
            switch (copy_map(shader, out))
            {
            case MapRead::Read:
                return true;
            case MapRead::Busy:
                result.busy = true;
                findings.skip("the HW shader factory held its lock");
                return false;
            case MapRead::Bad:
                findings.skip("the token map of {} {:#x} is empty or too large", what, shader);
                return false;
            case MapRead::Fault:
            default:
                findings.skip("the token map of {} {:#x} faulted on read", what, shader);
                return false;
            }
        }

        /**
         * @brief Compares the parsed twin's Z-pass pixel shader @p twin_pass with the plan for it: the permutation, the
         *        key, family CRC, name source, the cache object and resource the plan's keys find, and the token map.
         */
        void compare_zpass(Findings &findings, CompareResult &result, const ClonePlan &plan, const ZPassPlan &zpass,
                           std::uintptr_t twin_pass, const ShaderSnapshot &twin, int technique) noexcept
        {
            namespace C = Constants;
            HwSnapshot &got = s_hw_twin;
            HwSnapshot &want = s_hw_stock;
            if (!snapshot_hw(twin_pass, got) || !snapshot_hw(zpass.stock_shader, want))
            {
                findings.skip("technique {}'s Z-pass pixel shader faulted on read", technique);
                return;
            }
            if (!s_hardware_type.is(field<std::uintptr_t>(got.bytes, 0)))
            {
                findings.mismatch("technique {}'s Z-pass pixel shader {:#x} is not a CHWShader_D3D", technique,
                                  twin_pass);
                return;
            }
            compare_permutation(findings, want, got, technique, C::TECHNIQUE_PIXEL_SHADER_SLOT, "stock pass's");
            if (findings.done())
            {
                return;
            }
            const std::string_view entry = entry_of(got);
            if (std::string_view{got.key.data()} != std::string_view{zpass.key.data()})
            {
                findings.mismatch("technique {} ({}): resource name '{}' (planned '{}')", technique, entry,
                                  got.key.data(), zpass.key.data());
                return;
            }
            if (field<std::uint32_t>(got.bytes, C::HW_SHADER_CRC_OFFSET) != plan.family_crc)
            {
                findings.mismatch("technique {} ({}): CRC {:#x} (planned the family CRC {:#x})", technique, entry,
                                  field<std::uint32_t>(got.bytes, C::HW_SHADER_CRC_OFFSET), plan.family_crc);
                return;
            }
            if (std::string_view{got.source.data()} != std::string_view{twin.file.data()})
            {
                findings.mismatch("technique {} ({}): name source '{}' (planned the twin's file name '{}')", technique,
                                  entry, got.source.data(), twin.file.data());
                return;
            }
            const std::uintptr_t cache = lookup(C::HW_SHADER_CACHE_PS_CLASS_CRC, zpass.cache_key);
            const auto parsed_cache = field<std::uintptr_t>(got.bytes, C::HW_SHADER_CACHE_OFFSET);
            if (cache == 0 || cache != parsed_cache)
            {
                findings.mismatch("technique {} ({}): the cache object named '{}' is {:#x}, the parsed pass's {:#x}",
                                  technique, entry, zpass.cache_key.data(), cache, parsed_cache);
                return;
            }
            const std::uintptr_t named = lookup(C::HW_SHADER_PS_CLASS_CRC, zpass.key);
            if (named != twin_pass)
            {
                findings.mismatch("technique {} ({}): the planned name finds {:#x}, the parsed pass is {:#x}",
                                  technique, entry, named, twin_pass);
                return;
            }
            if (!read_map(findings, result, twin_pass, s_map_twin, "the parsed Z-pass pixel shader"))
            {
                return;
            }
            const ShaderTokenDiff::Difference difference =
                ShaderTokenDiff::classify(std::span<const std::uint8_t>{zpass.derived}, s_map_twin.view());
            ++result.zpasses;
            switch (difference.kind)
            {
            case ShaderTokenDiff::Kind::Equal:
                result.zpasses_identical += difference.identical ? 1 : 0;
                break;
            case ShaderTokenDiff::Kind::StringTable:
                // A string the derived tokens name that the parsed table lacks or spells otherwise: a splice fault.
                findings.mismatch("technique {} ({}): string {:#x}, which the tokens name, is missing from one table "
                                  "or has other text ({} planned tokens, {} parsed)",
                                  technique, entry, difference.at, difference.expected_count, difference.actual_count);
                break;
            case ShaderTokenDiff::Kind::HeaderDefines:
                findings.allow(Allowed::HEADER_DEFINES,
                               "technique {} ({}): only the leading #define lines differ, from token {} (the last "
                               "parse to write a shared key)",
                               technique, entry, difference.at);
                break;
            case ShaderTokenDiff::Kind::Tokens:
                findings.mismatch("technique {} ({}): the tokens differ at token {} (planned {:#x}, parsed {:#x}; "
                                  "{} planned tokens, {} parsed)",
                                  technique, entry, difference.at, difference.expected, difference.actual,
                                  difference.expected_count, difference.actual_count);
                break;
            case ShaderTokenDiff::Kind::Malformed:
            default:
                findings.mismatch("technique {} ({}): a token map is malformed", technique, entry);
                break;
            }
        }

        /**
         * @brief Compares a pass a clone keeps from its stock, @p stock_pass, with the parsed twin's own object
         *        @p twin_pass: the same permutation is required; a token difference is allowed (the clone draws the
         *        stock's).
         */
        void compare_stock_pass(Findings &findings, CompareResult &result, std::uintptr_t stock_pass,
                                std::uintptr_t twin_pass, int technique, int slot) noexcept
        {
            HwSnapshot &want = s_hw_stock;
            HwSnapshot &got = s_hw_twin;
            if (!snapshot_hw(stock_pass, want) || !snapshot_hw(twin_pass, got))
            {
                findings.skip("technique {} slot {} faulted on read", technique, slot);
                return;
            }
            if (!s_hardware_type.is(field<std::uintptr_t>(got.bytes, 0)))
            {
                findings.mismatch("technique {} slot {}: {:#x} is not a CHWShader_D3D", technique, slot, twin_pass);
                return;
            }
            compare_permutation(findings, want, got, technique, slot, "stock's");
            if (findings.done() || !read_map(findings, result, stock_pass, s_map_stock, "a stock pass") ||
                !read_map(findings, result, twin_pass, s_map_twin, "a parsed pass"))
            {
                return;
            }
            ++result.stock_passes;
            const ShaderTokenDiff::Difference difference =
                ShaderTokenDiff::classify(s_map_stock.view(), s_map_twin.view());
            if (difference.kind != ShaderTokenDiff::Kind::Equal)
            {
                findings.allow(Allowed::STOCK_PASS,
                               "technique {} slot {} ({}), a pass the clone keeps from the stock: {} at {} (the last "
                               "parse to write a shared stock key)",
                               technique, slot, entry_of(got), ShaderTokenDiff::describe(difference.kind),
                               difference.at);
            }
        }

        /** @brief The fields a pair's check that nothing changed while it was read compares. */
        struct ShaderState
        {
            std::uint32_t flags = 0;
            std::uint32_t techniques = 0;
            std::uint64_t mask = 0;
            bool operator==(const ShaderState &) const = default;
        };

        [[nodiscard]] bool read_state(std::uintptr_t shader, ShaderState &out) noexcept
        {
            return copy_guarded(&out.flags, shader + Constants::SHADER_FLAGS_OFFSET, sizeof(out.flags)) &&
                   copy_guarded(&out.techniques, shader + Constants::SHADER_TECHNIQUE_COUNT_OFFSET,
                                sizeof(out.techniques)) &&
                   copy_guarded(&out.mask, shader + Constants::SHADER_GEN_MASK_OFFSET, sizeof(out.mask));
        }

        /** @brief A queued pair, and how often the factory's lock turned it away. */
        struct QueuedPair
        {
            DryRunPair pair{};
            int busy = 0;
        };

        /** @brief What a kind's compares found this session. */
        struct KindTally
        {
            int compares = 0;
            int exact = 0;
            int allowed = 0;
            int mismatches = 0;
            int refused = 0;
        };

        /** @brief One run: from the first pair queued while none waited to the step that empties the queue. */
        struct Run
        {
            bool active = false;
            std::array<char, 96> occasion{};
            int compares = 0;
            int exact = 0;
            int allowed = 0;
            int mismatches = 0;
            int refused = 0;
            int skipped = 0;
            int busy_retries = 0;
            int zpasses = 0;
            int zpasses_identical = 0;
            std::array<int, Allowed::COUNT> allowed_by{};
            int planned = 0;
            std::uint64_t plan_us = 0;
            std::uint64_t plan_max_us = 0;
            int compared = 0;
            std::uint64_t compare_us = 0;
            std::uint64_t compare_max_us = 0;
            std::uint64_t pair_max_us = 0;
            int steps = 0;
            std::uint64_t step_us = 0;
            std::uint64_t step_max_us = 0;
        };

        /** @brief The dry-run's state. Main thread only. */
        struct State
        {
            std::vector<QueuedPair> queue;
            std::size_t next = 0;
            // The (stock, twin) pairs compared or refused this session: never queued again. A pair a busy factory lock
            // turned away is not among them.
            std::vector<std::pair<std::uintptr_t, std::uintptr_t>> handled;
            Run run{};
            std::array<KindTally, std::size(blobs::k_twins)> kinds{};
            ClonePlan plan{};
        };
        State s_state;

        void mark_handled(const DryRunPair &pair) noexcept
        {
            try
            {
                s_state.handled.emplace_back(pair.stock, pair.twin);
            }
            catch (...)
            {
                // It may be queued again by a later occasion; a second compare is harmless.
            }
        }

        [[nodiscard]] std::string_view verdict_text(Verdict verdict) noexcept
        {
            switch (verdict)
            {
            case Verdict::Exact:
                return "exact";
            case Verdict::Allowed:
                return "allowed differences only";
            case Verdict::Mismatch:
                return "MISMATCH";
            case Verdict::Skipped:
            default:
                return "not compared";
            }
        }

        enum class PairOutcome : std::uint8_t
        {
            Done,
            Busy,
        };

        /** @brief Plans and compares one pair, tallies it and logs it. */
        [[nodiscard]] PairOutcome dry_run_pair(const DryRunPair &pair) noexcept
        {
            HookProfile::Scope scope(HookProfile::Path::CloneDryRun);
            DMK_PROFILE_SCOPE("twins.clone.dryrun");
            Run &run = s_state.run;
            ClonePlan &plan = s_state.plan;
            KindTally &tally = s_state.kinds[static_cast<std::size_t>(pair.kind)];
            const blobs::Twin &kind = blobs::k_twins[pair.kind];
            const char *recovered = pair.recovered ? ", recovered after a hot reload" : "";
            const PlanStatus status = prepare_clone_plan(pair.stock, pair.kind, plan);
            if (status == PlanStatus::Busy)
            {
                scope.outcome(HookProfile::Outcome::NotCompared);
                return PairOutcome::Busy;
            }
            // Only a pair that is done counts toward the plan times: a busy one is planned again in a later step.
            const auto count_plan = [&run, &plan]() noexcept
            {
                ++run.planned;
                run.plan_us += plan.micros;
                run.plan_max_us = std::max(run.plan_max_us, plan.micros);
            };
            if (status != PlanStatus::Ready)
            {
                count_plan();
                ++run.refused;
                ++tally.refused;
                mark_handled(pair);
                scope.outcome(HookProfile::Outcome::NotCompared);
                const bool splice = status == PlanStatus::Splice;
                std::string_view splice_text;
                for (int i = 0; splice && i < plan.zpass_count; ++i)
                {
                    if (plan.zpasses[static_cast<std::size_t>(i)].splice_status != ShaderSplice::Status::Ok)
                    {
                        splice_text = ShaderSplice::describe(plan.zpasses[static_cast<std::size_t>(i)].splice_status);
                    }
                }
                (void)DMK::log().try_log(DMK::LogLevel::Info,
                                         "ShaderTwins: DEV clone dry-run: no plan for the {} twin of {} (gen mask "
                                         "{:#x}{}): {}{}{} ({:.2f} ms)",
                                         kind.label, pair.name.data(), pair.mask, recovered, describe(status),
                                         splice_text.empty() ? "" : ": ", splice_text, millis(plan.micros));
                return PairOutcome::Done;
            }
            const CompareResult result = dry_run_compare(plan, pair.twin);
            if (result.busy)
            {
                scope.outcome(HookProfile::Outcome::NotCompared);
                return PairOutcome::Busy;
            }
            count_plan();
            ++run.compared;
            run.compare_us += result.micros;
            run.compare_max_us = std::max(run.compare_max_us, result.micros);
            run.pair_max_us = std::max(run.pair_max_us, plan.micros + result.micros);
            mark_handled(pair);
            run.zpasses += result.zpasses;
            run.zpasses_identical += result.zpasses_identical;
            switch (result.verdict)
            {
            case Verdict::Skipped:
                ++run.skipped;
                scope.outcome(HookProfile::Outcome::NotCompared);
                break;
            case Verdict::Exact:
                ++run.compares;
                ++run.exact;
                ++tally.compares;
                ++tally.exact;
                scope.outcome(HookProfile::Outcome::Default);
                break;
            case Verdict::Allowed:
                ++run.compares;
                ++run.allowed;
                ++tally.compares;
                ++tally.allowed;
                for (int bit = 0; bit < Allowed::COUNT; ++bit)
                {
                    run.allowed_by[static_cast<std::size_t>(bit)] += (result.allowed >> bit) & 1u;
                }
                scope.outcome(HookProfile::Outcome::Allowed);
                break;
            case Verdict::Mismatch:
            default:
                ++run.compares;
                ++run.mismatches;
                ++tally.compares;
                ++tally.mismatches;
                scope.outcome(HookProfile::Outcome::Mismatch);
                break;
            }
            if (result.verdict == Verdict::Mismatch)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: DEV clone dry-run: MISMATCH for the {} twin of {} (gen mask "
                                         "{:#x}{}, stock {:#x}, twin {:#x}): {}; plan {:.2f} ms, compare {:.2f} ms",
                                         kind.label, pair.name.data(), pair.mask, recovered, pair.stock, pair.twin,
                                         result.detail.data(), millis(plan.micros), millis(result.micros));
            }
            else
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: DEV clone dry-run: the {} twin of {} (gen mask {:#x}{}): {}{}{}; "
                    "{} Z-pass map(s), {} byte-identical to the parsed twin's, {} stock pass(es) "
                    "compared; plan {:.2f} ms, compare {:.2f} ms",
                    kind.label, pair.name.data(), pair.mask, recovered, verdict_text(result.verdict),
                    result.detail[0] != '\0' ? ": " : "", result.detail.data(), result.zpasses,
                    result.zpasses_identical, result.stock_passes, millis(plan.micros), millis(result.micros));
            }
            return PairOutcome::Done;
        }

        /** @brief Logs the run's summary and each clone-capable kind's verdict for the session, and ends the run. */
        void finish_run(std::string_view where) noexcept
        {
            State &state = s_state;
            Run &run = state.run;
            const auto average = [](std::uint64_t total, int count)
            { return count > 0 ? millis(total) / static_cast<double>(count) : 0.0; };
            (void)DMK::log().try_log(
                run.mismatches > 0 ? DMK::LogLevel::Warning : DMK::LogLevel::Info,
                "ShaderTwins: DEV clone dry-run of {} finished in {}: {} compare(s): {} exact, {} with allowed "
                "differences ({} in leading #define lines, {} in passes the clone keeps from the stock, {} in the "
                "runtime float), {} MISMATCH; {} without a plan, {} not compared ({} lock retries); {} Z-pass map(s) "
                "derived, {} byte-identical to the parsed twin's; plan {:.2f} ms average, {:.2f} ms max, {:.1f} ms in "
                "all; compare {:.2f} ms average, {:.2f} ms max, {:.1f} ms in all; slowest pair {:.2f} ms; {} step(s), "
                "{:.1f} ms in all, longest step {:.2f} ms",
                run.occasion.data(), where, run.compares, run.exact, run.allowed, run.allowed_by[0], run.allowed_by[1],
                run.allowed_by[2], run.mismatches, run.refused, run.skipped, run.busy_retries, run.zpasses,
                run.zpasses_identical, average(run.plan_us, run.planned), millis(run.plan_max_us), millis(run.plan_us),
                average(run.compare_us, run.compared), millis(run.compare_max_us), millis(run.compare_us),
                millis(run.pair_max_us), run.steps, millis(run.step_us), millis(run.step_max_us));
            for (int kind = 0; kind < static_cast<int>(std::size(blobs::k_twins)); ++kind)
            {
                if (splice_of(kind) == nullptr)
                {
                    continue;
                }
                const KindTally &tally = state.kinds[static_cast<std::size_t>(kind)];
                if (tally.compares > 0 && tally.mismatches == 0)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "ShaderTwins: DEV clone dry-run verdict: {} twins are clone-eligible this "
                                             "session ({} compare(s): {} exact, {} with allowed differences, no "
                                             "mismatch; {} without a plan)",
                                             blobs::k_twins[kind].label, tally.compares, tally.exact, tally.allowed,
                                             tally.refused);
                }
                else if (tally.mismatches > 0)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                             "ShaderTwins: DEV clone dry-run verdict: {} twins are not clone-eligible "
                                             "this session ({} MISMATCH in {} compare(s); {} without a plan)",
                                             blobs::k_twins[kind].label, tally.mismatches, tally.compares,
                                             tally.refused);
                }
                else
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "ShaderTwins: DEV clone dry-run verdict: {} twins are not clone-eligible "
                                             "this session (no compare yet; {} without a plan)",
                                             blobs::k_twins[kind].label, tally.refused);
                }
            }
            state.queue.clear();
            state.next = 0;
            run.active = false;
        }

    } // namespace

    std::string_view describe(PlanStatus status) noexcept
    {
        switch (status)
        {
        case PlanStatus::Ready:
            return "planned";
        case PlanStatus::NotClonable:
            return "the kind has no clone splice";
        case PlanStatus::NotShader:
            return "the stock is not a CShader";
        case PlanStatus::NotLoaded:
            return "the stock is not parsed, or its binary did not load";
        case PlanStatus::Reloaded:
            return "the engine reloaded the stock (EF_RELOADED)";
        case PlanStatus::Techniques:
            return "the stock has no technique or too many";
        case PlanStatus::TechniqueLayout:
            return "a technique has another owner, render elements or render targets";
        case PlanStatus::HardwareSlot:
            return "a technique slot is not a CHWShader_D3D";
        case PlanStatus::NoZPass:
            return "no Z pre-pass or Z pass pixel shader";
        case PlanStatus::TooManyZPasses:
            return "more than two distinct Z-pass pixel shaders";
        case PlanStatus::Busy:
            return "the HW shader factory held its lock";
        case PlanStatus::TokenMap:
            return "a Z-pass token map is malformed or too large";
        case PlanStatus::StockKey:
            return "a Z-pass pixel shader's resource name is not \"<name>@<entry>(<platform>)(<mask>)\"";
        case PlanStatus::Splice:
            return "the splice refused a Z-pass map";
        case PlanStatus::Fault:
            return "a read faulted";
        case PlanStatus::OutOfMemory:
            return "out of memory";
        }
        return "unknown";
    }

    PlanStatus prepare_clone_plan(std::uintptr_t stock, int kind, ClonePlan &plan) noexcept
    {
        namespace C = Constants;
        const auto started = Clock::now();
        reset(plan, stock, kind);
        const auto done = [&plan, started](PlanStatus status) noexcept
        {
            plan.status = status;
            plan.micros = micros_since(started);
            return status;
        };
        const blobs::CloneSplice *splice = splice_of(kind);
        if (splice == nullptr || !s_available)
        {
            return done(PlanStatus::NotClonable);
        }
        const std::string_view twin_name = blobs::k_twins[kind].twin_name;
        plan.family_crc = family_crc(twin_name);
        ShaderSnapshot &shader = s_stock_snapshot;
        if (!snapshot_shader(stock, shader))
        {
            return done(PlanStatus::Fault);
        }
        if (!s_shader_type.is(field<std::uintptr_t>(shader.bytes, 0)))
        {
            return done(PlanStatus::NotShader);
        }
        const auto flags = field<std::uint32_t>(shader.bytes, C::SHADER_FLAGS_OFFSET);
        plan.stock_flags = flags;
        if ((flags & C::SHADER_FLAG_LOADED) == 0 || (flags & C::SHADER_FLAG_NOT_FOUND) != 0)
        {
            return done(PlanStatus::NotLoaded);
        }
        if ((flags & C::SHADER_FLAG_RELOADED) != 0)
        {
            return done(PlanStatus::Reloaded);
        }
        if (shader.techniques == 0 || shader.techniques > static_cast<std::uint32_t>(k_max_techniques))
        {
            return done(PlanStatus::Techniques);
        }
        plan.techniques = shader.techniques;
        const std::string_view stock_name{shader.name.data()};

        // The techniques a clone copies whole, and the Z-pass pixel shaders it derives in place of the stock's.
        HwSnapshot &pass = s_hw_stock;
        for (std::uint32_t t = 0; t < shader.techniques; ++t)
        {
            const auto &bytes = shader.technique_bytes[t];
            if (shader.technique_ptrs[t] == 0 || field<std::uintptr_t>(bytes, C::TECHNIQUE_OWNER_OFFSET) != stock ||
                !zeroed(bytes, C::TECHNIQUE_ELEMENTS_OFFSET, C::TECHNIQUE_ELEMENTS_SIZE))
            {
                return done(PlanStatus::TechniqueLayout);
            }
            plan.technique_bytes[t] = bytes;
            for (int slot = 0; slot < k_slots; ++slot)
            {
                const auto hw = field<std::uintptr_t>(bytes, C::TECHNIQUE_SHADERS_OFFSET + slot * 8);
                std::uintptr_t vtable = 0;
                if (hw == 0)
                {
                    continue;
                }
                if (!copy_guarded(&vtable, hw, sizeof(vtable)))
                {
                    return done(PlanStatus::Fault);
                }
                if (!s_hardware_type.is(vtable))
                {
                    return done(PlanStatus::HardwareSlot);
                }
            }
            const auto pixel =
                field<std::uintptr_t>(bytes, C::TECHNIQUE_SHADERS_OFFSET + C::TECHNIQUE_PIXEL_SHADER_SLOT * 8);
            if (pixel == 0)
            {
                continue;
            }
            int at = -1;
            for (int z = 0; z < plan.zpass_count; ++z)
            {
                at = plan.zpasses[static_cast<std::size_t>(z)].stock_shader == pixel ? z : at;
            }
            if (at >= 0)
            {
                plan.zpass_of[t] = static_cast<std::int8_t>(at);
                continue;
            }
            if (!snapshot_hw(pixel, pass))
            {
                return done(PlanStatus::Fault);
            }
            const std::string_view entry{pass.entry.data()};
            if (entry != C::ZPREPASS_PIXEL_ENTRY && entry != C::ZPASS_PIXEL_ENTRY)
            {
                continue;
            }
            if (plan.zpass_count == static_cast<int>(plan.zpasses.size()))
            {
                return done(PlanStatus::TooManyZPasses);
            }
            at = plan.zpass_count++;
            plan.zpass_of[t] = static_cast<std::int8_t>(at);
            ZPassPlan &zpass = plan.zpasses[static_cast<std::size_t>(at)];
            zpass.stock_shader = pixel;
            zpass.entry = entry == C::ZPASS_PIXEL_ENTRY ? ShaderSplice::Entry::ZPass : ShaderSplice::Entry::ZPrePass;
            zpass.hw_mask = field<std::uint64_t>(pass.bytes, C::HW_SHADER_MASK_OFFSET);
            // The stock pass's own name proves the factory's naming and gives its platform.
            const std::string_view platform = key_platform(pass.key.data(), stock_name, entry, zpass.hw_mask);
            if (platform.empty() || !factory_key(zpass.key, twin_name, entry, platform, zpass.hw_mask, true) ||
                !factory_key(zpass.cache_key, twin_name, entry, platform, zpass.hw_mask, false))
            {
                return done(PlanStatus::StockKey);
            }
        }
        if (plan.zpass_count == 0)
        {
            return done(PlanStatus::NoZPass);
        }

        // The derived token maps, laid out for the factory (the arena) and as the engine stores one (derived).
        for (int z = 0; z < plan.zpass_count; ++z)
        {
            ZPassPlan &zpass = plan.zpasses[static_cast<std::size_t>(z)];
            switch (copy_map(zpass.stock_shader, s_map_stock))
            {
            case MapRead::Read:
                break;
            case MapRead::Busy:
                return done(PlanStatus::Busy);
            case MapRead::Bad:
                return done(PlanStatus::TokenMap);
            case MapRead::Fault:
            default:
                return done(PlanStatus::Fault);
            }
            ShaderSplice::TokenMap map;
            if (!ShaderSplice::read_token_map(s_map_stock.view(), map))
            {
                return done(PlanStatus::TokenMap);
            }
            try
            {
                zpass.arena.resize(ShaderSplice::arena_size(map, *splice));
            }
            catch (...)
            {
                return done(PlanStatus::OutOfMemory);
            }
            ShaderSplice::Arena arena{std::span<std::byte>{zpass.arena}};
            zpass.splice = ShaderSplice::splice(s_map_stock.view(), zpass.entry, *splice, arena);
            zpass.splice_status = zpass.splice.status;
            if (zpass.splice.status != ShaderSplice::Status::Ok)
            {
                return done(PlanStatus::Splice);
            }
            try
            {
                zpass.derived.resize(ShaderTokenDiff::serialized_size(zpass.splice));
            }
            catch (...)
            {
                return done(PlanStatus::OutOfMemory);
            }
            if (ShaderTokenDiff::serialize(zpass.splice, zpass.derived) != zpass.derived.size())
            {
                return done(PlanStatus::Splice);
            }
        }

        // The CShader scalars: the stock's, but for the binary CRCs a parsed twin carries.
        ShaderScalars &scalars = plan.scalars;
        scalars.draw_type = field<std::uint32_t>(shader.bytes, C::SHADER_DRAW_TYPE_OFFSET);
        scalars.flags = flags & ~(C::SHADER_FLAG_RELOADED | C::SHADER_FLAG_NOT_FOUND);
        scalars.flags2 = field<std::uint32_t>(shader.bytes, C::SHADER_FLAGS2_OFFSET);
        scalars.features = field<std::uint16_t>(shader.bytes, C::SHADER_FEATURE_FLAGS_OFFSET);
        scalars.features_high = field<std::uint16_t>(shader.bytes, C::SHADER_FEATURE_HIGH_OFFSET);
        scalars.techniques = shader.techniques;
        scalars.fx_params_key = field<std::uint32_t>(shader.bytes, C::SHADER_FX_PARAMS_KEY_OFFSET);
        scalars.shader_type = field<std::uint32_t>(shader.bytes, C::SHADER_TYPE_OFFSET);
        scalars.gen_mask = field<std::uint64_t>(shader.bytes, C::SHADER_GEN_MASK_OFFSET);
        scalars.gen_params = field<std::uintptr_t>(shader.bytes, C::SHADER_GEN_PARAMS_OFFSET);
        scalars.second_crc = 0;
        scalars.binary_crc = plan.family_crc;
        scalars.feature_count = field<std::uint32_t>(shader.bytes, C::SHADER_FEATURE_COUNT_OFFSET);
        scalars.decal_atlas_size = field<std::uint32_t>(shader.bytes, C::SHADER_DECAL_ATLAS_SIZE_OFFSET);
        scalars.runtime_float = field<std::uint32_t>(shader.bytes, C::SHADER_RUNTIME_FLOAT_OFFSET);
        return done(PlanStatus::Ready);
    }

    CompareResult dry_run_compare(const ClonePlan &plan, std::uintptr_t twin) noexcept
    {
        namespace C = Constants;
        const auto started = Clock::now();
        CompareResult result;
        Findings findings(result);
        const auto finish = [&result, &findings, started]() noexcept -> CompareResult
        {
            if (!findings.done())
            {
                result.verdict = result.allowed != 0 ? Verdict::Allowed : Verdict::Exact;
            }
            result.micros = micros_since(started);
            return result;
        };
        result.verdict = Verdict::Exact;
        if (plan.status != PlanStatus::Ready || plan.kind < 0 || !s_available)
        {
            findings.skip("no plan");
            return finish();
        }
        ShaderSnapshot &parsed = s_twin_snapshot;
        if (!snapshot_shader(twin, parsed))
        {
            findings.skip("the twin faulted on read");
            return finish();
        }
        if (!s_shader_type.is(field<std::uintptr_t>(parsed.bytes, 0)))
        {
            findings.skip("the twin is not a CShader");
            return finish();
        }
        const auto flags = field<std::uint32_t>(parsed.bytes, C::SHADER_FLAGS_OFFSET);
        if ((flags & C::SHADER_FLAG_LOADED) == 0 || (flags & C::SHADER_FLAG_NOT_FOUND) != 0)
        {
            findings.skip("the twin is not parsed (flags {:#x})", flags);
            return finish();
        }
        if ((flags & C::SHADER_FLAG_RELOADED) != 0)
        {
            findings.skip("the engine reloaded the twin (flags {:#x})", flags);
            return finish();
        }
        // A clone shares its stock's FX parameters key; only a parse is ground truth.
        const auto key = field<std::uint32_t>(parsed.bytes, C::SHADER_FX_PARAMS_KEY_OFFSET);
        if (key == plan.scalars.fx_params_key && key != 0xFFFFFFFFu)
        {
            findings.skip("the twin shares its stock's FX parameters key {:#x}: a clone, never ground truth", key);
            return finish();
        }

        // The CShader: its name, then the scalars a clone takes from its stock or its family.
        const std::string_view twin_name = blobs::k_twins[plan.kind].twin_name;
        if (!equals_nocase(parsed.name.data(), twin_name))
        {
            findings.mismatch("CShader +{:#x} name '{}' (planned '{}')", C::SHADER_NAME_OFFSET, parsed.name.data(),
                              twin_name);
            return finish();
        }
        const ShaderScalars &want = plan.scalars;
        const auto u16 = [&parsed](std::ptrdiff_t o) { return field<std::uint16_t>(parsed.bytes, o); };
        const auto u32 = [&parsed](std::ptrdiff_t o) { return field<std::uint32_t>(parsed.bytes, o); };
        const auto u64 = [&parsed](std::ptrdiff_t o) { return field<std::uint64_t>(parsed.bytes, o); };
        const auto scalar =
            [&findings](std::ptrdiff_t offset, std::string_view what, std::uint64_t got, std::uint64_t planned) noexcept
        {
            if (got != planned)
            {
                findings.mismatch("CShader +{:#x} ({}) {:#x} (planned {:#x})", offset, what, got, planned);
            }
        };
        scalar(C::SHADER_DRAW_TYPE_OFFSET, "draw type", u32(C::SHADER_DRAW_TYPE_OFFSET), want.draw_type);
        scalar(C::SHADER_FLAGS_OFFSET, "flags", flags & ~(C::SHADER_FLAG_RELOADED | C::SHADER_FLAG_NOT_FOUND),
               want.flags);
        scalar(C::SHADER_FLAGS2_OFFSET, "flags2", u32(C::SHADER_FLAGS2_OFFSET), want.flags2);
        scalar(C::SHADER_FEATURE_FLAGS_OFFSET, "feature word", u16(C::SHADER_FEATURE_FLAGS_OFFSET), want.features);
        scalar(C::SHADER_FEATURE_HIGH_OFFSET, "word after it", u16(C::SHADER_FEATURE_HIGH_OFFSET), want.features_high);
        scalar(C::SHADER_TECHNIQUE_COUNT_OFFSET, "technique count", parsed.techniques, want.techniques);
        scalar(C::SHADER_TYPE_OFFSET, "shader type", u32(C::SHADER_TYPE_OFFSET), want.shader_type);
        scalar(C::SHADER_GEN_MASK_OFFSET, "gen mask", u64(C::SHADER_GEN_MASK_OFFSET), want.gen_mask);
        scalar(C::SHADER_GEN_PARAMS_OFFSET, "generator parameters", u64(C::SHADER_GEN_PARAMS_OFFSET), want.gen_params);
        scalar(C::SHADER_SECOND_CRC_OFFSET, "second binary CRC", u32(C::SHADER_SECOND_CRC_OFFSET), want.second_crc);
        scalar(C::SHADER_BINARY_CRC_OFFSET, "binary CRC", u32(C::SHADER_BINARY_CRC_OFFSET), want.binary_crc);
        scalar(C::SHADER_FEATURE_COUNT_OFFSET, "FEATURE_COUNT", u32(C::SHADER_FEATURE_COUNT_OFFSET),
               want.feature_count);
        scalar(C::SHADER_DECAL_ATLAS_SIZE_OFFSET, "DECAL_ATLAS_SIZE", u32(C::SHADER_DECAL_ATLAS_SIZE_OFFSET),
               want.decal_atlas_size);
        if (findings.done())
        {
            return finish();
        }
        if (u32(C::SHADER_RUNTIME_FLOAT_OFFSET) != want.runtime_float)
        {
            findings.allow(Allowed::RUNTIME_FLOAT, "CShader +{:#x} (runtime float) {:#x}, the stock's {:#x}",
                           C::SHADER_RUNTIME_FLOAT_OFFSET, u32(C::SHADER_RUNTIME_FLOAT_OFFSET), want.runtime_float);
        }

        // The techniques, field by field, and their slots.
        std::array<std::uintptr_t, 2> parsed_zpass{};
        std::array<std::pair<std::uintptr_t, std::uintptr_t>, k_max_techniques * k_slots> stock_passes{};
        std::size_t stock_pass_count = 0;
        for (std::uint32_t t = 0; t < plan.techniques && !findings.done(); ++t)
        {
            const int i = static_cast<int>(t);
            const auto &got = parsed.technique_bytes[t];
            const auto &planned = plan.technique_bytes[t];
            if (parsed.technique_ptrs[t] == 0 || field<std::uintptr_t>(got, C::TECHNIQUE_OWNER_OFFSET) != twin)
            {
                findings.mismatch("technique {} is null or has another owner", i);
                break;
            }
            const auto got_name = field<std::uintptr_t>(got, C::TECHNIQUE_NAME_OFFSET);
            const auto planned_name = field<std::uintptr_t>(planned, C::TECHNIQUE_NAME_OFFSET);
            if (got_name != planned_name)
            {
                std::array<char, 64> a{};
                std::array<char, 64> b{};
                if (!copy_text(got_name, a) || !copy_text(planned_name, b) ||
                    std::string_view{a.data()} != std::string_view{b.data()})
                {
                    findings.mismatch("technique {} +{:#x}: name '{}' (planned '{}')", i, C::TECHNIQUE_NAME_OFFSET,
                                      a.data(), b.data());
                    break;
                }
            }
            if (!same_bytes(got, planned, C::TECHNIQUE_FLAGS_OFFSET, C::TECHNIQUE_FLAGS_SIZE))
            {
                findings.mismatch("technique {} +{:#x} (flags) {:#x} (planned {:#x})", i, C::TECHNIQUE_FLAGS_OFFSET,
                                  field<std::uint64_t>(got, C::TECHNIQUE_FLAGS_OFFSET),
                                  field<std::uint64_t>(planned, C::TECHNIQUE_FLAGS_OFFSET));
                break;
            }
            if (!zeroed(got, C::TECHNIQUE_ELEMENTS_OFFSET, C::TECHNIQUE_ELEMENTS_SIZE))
            {
                findings.mismatch("technique {} +{:#x}: render elements or render targets", i,
                                  C::TECHNIQUE_ELEMENTS_OFFSET);
                break;
            }
            if (!same_bytes(got, planned, C::TECHNIQUE_CRC_OFFSET, C::TECHNIQUE_CRC_SIZE))
            {
                findings.mismatch("technique {} +{:#x}..+{:#x} (CRC {:#x}, planned {:#x})", i, C::TECHNIQUE_CRC_OFFSET,
                                  C::TECHNIQUE_CRC_OFFSET + static_cast<std::ptrdiff_t>(C::TECHNIQUE_CRC_SIZE) - 1,
                                  field<std::uint32_t>(got, C::TECHNIQUE_CRC_OFFSET),
                                  field<std::uint32_t>(planned, C::TECHNIQUE_CRC_OFFSET));
                break;
            }
            if (!same_bytes(got, planned, C::TECHNIQUE_INDICES_OFFSET, C::TECHNIQUE_INDICES_SIZE))
            {
                findings.mismatch("technique {} +{:#x}..+{:#x} (pass indices)", i, C::TECHNIQUE_INDICES_OFFSET,
                                  C::TECHNIQUE_INDICES_OFFSET + static_cast<std::ptrdiff_t>(C::TECHNIQUE_INDICES_SIZE) -
                                      1);
                break;
            }
            for (int slot = 0; slot < k_slots && !findings.done(); ++slot)
            {
                const auto stock_pass = field<std::uintptr_t>(planned, C::TECHNIQUE_SHADERS_OFFSET + slot * 8);
                const auto twin_pass = field<std::uintptr_t>(got, C::TECHNIQUE_SHADERS_OFFSET + slot * 8);
                if ((stock_pass == 0) != (twin_pass == 0))
                {
                    findings.mismatch("technique {} slot {}: {:#x} (the stock's {:#x})", i, slot, twin_pass,
                                      stock_pass);
                    break;
                }
                if (stock_pass == 0)
                {
                    continue;
                }
                const int z = slot == C::TECHNIQUE_PIXEL_SHADER_SLOT ? plan.zpass_of[t] : -1;
                if (z >= 0)
                {
                    // One parsed object per derived map, compared once.
                    std::uintptr_t &seen = parsed_zpass[static_cast<std::size_t>(z)];
                    if (seen == 0)
                    {
                        seen = twin_pass;
                        compare_zpass(findings, result, plan, plan.zpasses[static_cast<std::size_t>(z)], twin_pass,
                                      parsed, i);
                    }
                    else if (seen != twin_pass)
                    {
                        findings.mismatch("technique {}'s Z-pass pixel shader {:#x} is another object than an earlier "
                                          "technique's {:#x} for the same stock pass",
                                          i, twin_pass, seen);
                    }
                    continue;
                }
                if (stock_pass == twin_pass)
                {
                    continue; // the HW shader factory handed the twin the stock's own pass object
                }
                const std::pair<std::uintptr_t, std::uintptr_t> key_pair{stock_pass, twin_pass};
                const auto end = stock_passes.begin() + static_cast<std::ptrdiff_t>(stock_pass_count);
                if (std::find(stock_passes.begin(), end, key_pair) != end)
                {
                    continue;
                }
                if (stock_pass_count < stock_passes.size())
                {
                    stock_passes[stock_pass_count++] = key_pair;
                }
                compare_stock_pass(findings, result, stock_pass, twin_pass, i, slot);
            }
        }
        if (findings.done())
        {
            return finish();
        }

        // Neither shader may have changed while it was read: a reload in the pause menu frees and parses again.
        ShaderState stock_now{};
        ShaderState twin_now{};
        if (!read_state(plan.stock, stock_now) || !read_state(twin, twin_now) ||
            stock_now != ShaderState{plan.stock_flags, plan.techniques, plan.scalars.gen_mask} ||
            twin_now != ShaderState{flags, parsed.techniques, u64(C::SHADER_GEN_MASK_OFFSET)})
        {
            findings.skip("the stock or the twin changed while it was read");
        }
        return finish();
    }

    void initialize_twin_clone() noexcept
    {
        // Every offset the dry-run reads lies on a store one of these pins (aob_resolver.hpp).
        constexpr AnchorId k_layout_proofs[] = {AnchorId::TechniqueLayout,     AnchorId::ShaderLayout,
                                                AnchorId::ShaderMembers,       AnchorId::ShaderBinaryCrcs,
                                                AnchorId::ResourceName,        AnchorId::HardwareShaderMembers,
                                                AnchorId::HardwareShaderSetup, AnchorId::HardwareShaderCache};
        const bool proven = std::ranges::all_of(k_layout_proofs, [](AnchorId id)
                                                { return gated_anchor_address(Feature::TwinCloneDryRun, id) != 0; });
        const std::uintptr_t factory = gated_anchor_address(Feature::TwinCloneDryRun, AnchorId::HardwareShaderForName);
        const std::uintptr_t lock_site =
            gated_anchor_address(Feature::TwinCloneDryRun, AnchorId::HardwareShaderLockSite);
        const std::uintptr_t lookup_fn = gated_anchor_address(Feature::TwinCloneDryRun, AnchorId::ResourceLookup);
        const char *miss = nullptr;
        std::uintptr_t lock = 0;
        if (!proven || factory == 0 || lock_site == 0 || lookup_fn == 0)
        {
            miss = "the TwinCloneDryRun gate failed: a layout proof of SShaderTechnique, CShader, CBaseResource or "
                   "CHWShader_D3D, the HW shader factory, its lock or the resource lookup did not resolve";
        }
        else if (!site_in_function(lock_site, factory))
        {
            miss = "the factory lock site is not inside the HW shader factory";
        }
        else if (const auto decoded = DMK::scan::resolve_rip_relative(DMK::Address{lock_site}, 3, 7); !decoded)
        {
            miss = "the factory lock did not decode";
        }
        else
        {
            const ModuleInfo &mod = module_info();
            lock = decoded->raw();
            if (mod.base == 0 || lock < mod.base || lock >= mod.base + mod.size || lock % alignof(SRWLOCK) != 0 ||
                !DMK::memory::is_writable(DMK::Region{*decoded, sizeof(SRWLOCK)}))
            {
                miss = "the factory lock is not a writable SRWLOCK in the game image";
            }
        }
        if (miss == nullptr)
        {
            try
            {
                s_map_stock.bytes.resize(ShaderTokens::k_max_bytes);
                s_map_twin.bytes.resize(ShaderTokens::k_max_bytes);
                s_state.queue.reserve(Constants::SHADER_TWIN_MAX_SOURCES);
                s_state.handled.reserve(Constants::SHADER_TWIN_MAX_SOURCES);
            }
            catch (...)
            {
                miss = "its buffers could not be allocated";
            }
        }
        if (miss != nullptr)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Info, "ShaderTwins: DEV clone dry-run off ({})", miss);
            return;
        }
        s_lookup = reinterpret_cast<ResourceLookupFn>(lookup_fn);
        s_factory_lock = reinterpret_cast<PSRWLOCK>(lock);
        s_available = true;
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "ShaderTwins: DEV clone dry-run on: at each loading boundary, and in paused frames with nothing left to "
            "create, it plans every ready plant and frame twin from its stock shader and compares the plan with the "
            "twin the engine parsed; read only, it never arms, creates or writes anything (the SShaderTechnique, "
            "CShader, CBaseResource and CHWShader_D3D layouts it reads are proven by {} anchors; they prove layouts, "
            "not a build, so the dry-run runs on any build that matches them; factory lock 0x{:016X})",
            std::size(k_layout_proofs), lock);
    }

    bool dry_run_available() noexcept
    {
        return s_available;
    }

    bool clone_capable(int kind) noexcept
    {
        return splice_of(kind) != nullptr;
    }

    int queue_dry_run(std::span<const DryRunPair> pairs, std::string_view occasion) noexcept
    {
        State &state = s_state;
        if (!s_available || pairs.empty())
        {
            return 0;
        }
        int queued = 0;
        try
        {
            for (const DryRunPair &pair : pairs)
            {
                const auto same = [&pair](const std::pair<std::uintptr_t, std::uintptr_t> &known)
                { return known.first == pair.stock && known.second == pair.twin; };
                const bool waiting = std::any_of(state.queue.begin() + static_cast<std::ptrdiff_t>(state.next),
                                                 state.queue.end(), [&pair](const QueuedPair &q)
                                                 { return q.pair.stock == pair.stock && q.pair.twin == pair.twin; });
                if (pair.stock == 0 || pair.twin == 0 || !clone_capable(pair.kind) || waiting ||
                    std::any_of(state.handled.begin(), state.handled.end(), same))
                {
                    continue;
                }
                state.queue.push_back(QueuedPair{.pair = pair});
                ++queued;
            }
        }
        catch (...)
        {
            // What did not fit is queued again by a later occasion.
        }
        if (queued == 0)
        {
            return 0;
        }
        Run &run = state.run;
        if (!run.active)
        {
            run = Run{};
            run.active = true;
            const auto written = std::format_to_n(run.occasion.data(), run.occasion.size() - 1, "{}", occasion);
            run.occasion[std::min(static_cast<std::size_t>(written.size), run.occasion.size() - 1)] = '\0';
            (void)DMK::log().try_log(DMK::LogLevel::Info,
                                     "ShaderTwins: DEV clone dry-run of {}: {} ready plant and frame twin(s) to plan "
                                     "and compare ({} handled earlier this session); read only",
                                     run.occasion.data(), queued, state.handled.size());
        }
        else
        {
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: DEV clone dry-run: {} more twin(s) from {} join the run of {} ({} "
                                     "left)",
                                     queued, occasion, run.occasion.data(), state.queue.size() - state.next);
        }
        return queued;
    }

    bool dry_run_pending() noexcept
    {
        return s_available && s_state.next < s_state.queue.size();
    }

    void run_dry_run(std::uint64_t budget_us, int max_pairs, std::string_view where) noexcept
    {
        State &state = s_state;
        if (!s_available || state.next >= state.queue.size())
        {
            return;
        }
        DMK_PROFILE_SCOPE("twins.clone.dryrun_step");
        Run &run = state.run;
        const auto started = Clock::now();
        int handled = 0;
        while (state.next < state.queue.size() && (max_pairs <= 0 || handled < max_pairs) &&
               micros_since(started) < budget_us)
        {
            QueuedPair &queued = state.queue[state.next];
            ++handled;
            if (dry_run_pair(queued.pair) != PairOutcome::Busy)
            {
                ++state.next;
                continue;
            }
            ++run.busy_retries;
            if (++queued.busy <= Constants::TWIN_CLONE_DRY_RUN_BUSY_RETRIES)
            {
                // The factory holds its lock only while it creates a pass: the next step tries this pair again.
                break;
            }
            // Given up for this run but not marked handled, so a later occasion queues it again.
            ++state.next;
            ++run.skipped;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: DEV clone dry-run: the {} twin of {} not compared in this run; the "
                                     "HW shader factory held its lock in {} step(s)",
                                     blobs::k_twins[queued.pair.kind].label, queued.pair.name.data(), queued.busy);
        }
        const std::uint64_t step_us = micros_since(started);
        ++run.steps;
        run.step_us += step_us;
        run.step_max_us = std::max(run.step_max_us, step_us);
        const std::size_t left = state.queue.size() - state.next;
        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                 "ShaderTwins: DEV clone dry-run step in {}: {} pair(s) in {:.2f} ms, {} left", where,
                                 handled, millis(step_us), left);
        if (left == 0)
        {
            finish_run(where);
        }
    }

} // namespace TPVCamera::TwinClone

#endif // TPVCAMERA_DEV_BUILD
