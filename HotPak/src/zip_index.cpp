/**
 * @file zip_index.cpp
 * @brief Central-directory reader for KCD paks.
 */

#include "zip_index.hpp"
#include "text_util.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HotPak
{
    namespace
    {
        constexpr std::uint32_t EOCD_SIGNATURE = 0x06054B50;
        constexpr std::uint32_t CENTRAL_SIGNATURE = 0x02014B50;
        constexpr std::size_t EOCD_SIZE = 22;
        constexpr std::size_t CENTRAL_HEADER_SIZE = 46;
        // The EOCD sits within its own size plus the longest possible archive comment of the end of the file.
        constexpr std::size_t EOCD_SEARCH_SPAN = EOCD_SIZE + 0xFFFF;
        constexpr std::size_t MAX_CENTRAL_DIRECTORY = 64ull * 1024 * 1024;

        [[nodiscard]] DMK::Error fail(const char *reason)
        {
            return DMK::Error{DMK::ErrorCode::Unknown, reason};
        }

        [[nodiscard]] std::uint16_t u16(const std::byte *p) noexcept
        {
            std::uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }

        [[nodiscard]] std::uint32_t u32(const std::byte *p) noexcept
        {
            std::uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }

        /** @brief Owns a Win32 file handle. */
        class FileHandle
        {
        public:
            explicit FileHandle(const std::wstring &path) noexcept
                : m_handle(CreateFileW(
                      path.c_str(),
                      GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr,
                      OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,
                      nullptr
                  ))
            {
            }

            ~FileHandle() noexcept
            {
                if (valid())
                {
                    CloseHandle(m_handle);
                }
            }

            FileHandle(const FileHandle &) = delete;
            FileHandle &operator=(const FileHandle &) = delete;
            FileHandle(FileHandle &&) = delete;
            FileHandle &operator=(FileHandle &&) = delete;

            [[nodiscard]] bool valid() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }

            /** @brief Reads exactly @p out.size() bytes at @p offset. */
            [[nodiscard]] bool read_at(std::uint64_t offset, std::span<std::byte> out) const noexcept
            {
                LARGE_INTEGER position{};
                position.QuadPart = static_cast<LONGLONG>(offset);
                if (!SetFilePointerEx(m_handle, position, nullptr, FILE_BEGIN))
                {
                    return false;
                }
                std::size_t done = 0;
                while (done < out.size())
                {
                    DWORD got = 0;
                    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 20));
                    if (!ReadFile(m_handle, out.data() + done, want, &got, nullptr) || got == 0)
                    {
                        return false;
                    }
                    done += got;
                }
                return true;
            }

            [[nodiscard]] std::uint64_t size() const noexcept
            {
                LARGE_INTEGER value{};
                return GetFileSizeEx(m_handle, &value) ? static_cast<std::uint64_t>(value.QuadPart) : 0;
            }

        private:
            HANDLE m_handle = INVALID_HANDLE_VALUE;
        };
    } // namespace

    DMK::Result<std::vector<ZipEntry>> read_zip_index(const std::wstring &path)
    {
        const FileHandle file(path);
        if (!file.valid())
        {
            return std::unexpected(fail("zip/open"));
        }
        const std::uint64_t file_size = file.size();
        if (file_size < EOCD_SIZE)
        {
            return std::unexpected(fail("zip/too_small"));
        }

        const std::size_t tail_size = static_cast<std::size_t>(std::min<std::uint64_t>(file_size, EOCD_SEARCH_SPAN));
        std::vector<std::byte> tail(tail_size);
        if (!file.read_at(file_size - tail_size, tail))
        {
            return std::unexpected(fail("zip/read_tail"));
        }

        std::size_t eocd = std::string::npos;
        for (std::size_t i = tail_size - EOCD_SIZE + 1; i-- > 0;)
        {
            if (u32(tail.data() + i) == EOCD_SIGNATURE)
            {
                eocd = i;
                break;
            }
        }
        if (eocd == std::string::npos)
        {
            return std::unexpected(fail("zip/no_eocd"));
        }

        const std::byte *record = tail.data() + eocd;
        const std::uint16_t entry_count = u16(record + 10);
        const std::uint32_t directory_size = u32(record + 12);
        const std::uint32_t directory_offset = u32(record + 16);
        if (entry_count == 0xFFFF || directory_size == 0xFFFFFFFF || directory_offset == 0xFFFFFFFF)
        {
            return std::unexpected(fail("zip/zip64"));
        }
        if (directory_size > MAX_CENTRAL_DIRECTORY ||
            static_cast<std::uint64_t>(directory_offset) + directory_size > file_size)
        {
            return std::unexpected(fail("zip/bad_directory"));
        }

        std::vector<std::byte> directory(directory_size);
        if (!file.read_at(directory_offset, directory))
        {
            return std::unexpected(fail("zip/read_directory"));
        }

        std::vector<ZipEntry> entries;
        entries.reserve(entry_count);
        std::size_t at = 0;
        for (std::uint16_t i = 0; i < entry_count; ++i)
        {
            if (at + CENTRAL_HEADER_SIZE > directory.size() || u32(directory.data() + at) != CENTRAL_SIGNATURE)
            {
                return std::unexpected(fail("zip/bad_header"));
            }
            const std::byte *header = directory.data() + at;
            const std::uint16_t name_length = u16(header + 28);
            const std::uint16_t extra_length = u16(header + 30);
            const std::uint16_t comment_length = u16(header + 32);
            if (at + CENTRAL_HEADER_SIZE + name_length > directory.size())
            {
                return std::unexpected(fail("zip/bad_name"));
            }
            const std::string_view name(reinterpret_cast<const char *>(header + CENTRAL_HEADER_SIZE), name_length);
            entries.push_back(
                ZipEntry{
                    .name = normalize_path(name),
                    .crc32 = u32(header + 16),
                    .size = u32(header + 24),
                }
            );
            at += CENTRAL_HEADER_SIZE + name_length + extra_length + comment_length;
        }
        return entries;
    }

    std::vector<ZipEntry> changed_entries(const std::vector<ZipEntry> &previous, const std::vector<ZipEntry> &next)
    {
        std::unordered_map<std::string_view, const ZipEntry *> known;
        known.reserve(previous.size());
        for (const ZipEntry &entry : previous)
        {
            known.emplace(entry.name, &entry);
        }
        std::vector<ZipEntry> changed;
        for (const ZipEntry &entry : next)
        {
            const auto it = known.find(entry.name);
            if (it == known.end() || it->second->crc32 != entry.crc32 || it->second->size != entry.size)
            {
                changed.push_back(entry);
            }
        }
        return changed;
    }

} // namespace HotPak
