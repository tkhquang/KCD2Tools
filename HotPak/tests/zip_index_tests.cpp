/**
 * @file tests/zip_index_tests.cpp
 * @brief Unit tests of the central-directory reader and the change diff.
 */

#include "check.hpp"
#include "zip_index.hpp"

#include <process.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    using namespace HotPak;

    /** @brief One central-directory record to write. */
    struct Record
    {
        std::string name;
        std::uint32_t crc32 = 0;
        std::uint32_t size = 0;
        std::string extra;
        std::string comment;
    };

    void put16(std::string &out, std::uint16_t value)
    {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>(value >> 8));
    }

    void put32(std::string &out, std::uint32_t value)
    {
        put16(out, static_cast<std::uint16_t>(value & 0xFFFF));
        put16(out, static_cast<std::uint16_t>(value >> 16));
    }

    /** @brief A zip whose body is filler: the reader only reads the central directory and the end record. */
    std::string make_zip(const std::vector<Record> &records, std::string_view archive_comment = {})
    {
        std::string zip(64, 'x');
        const auto directory_offset = static_cast<std::uint32_t>(zip.size());
        for (const Record &record : records)
        {
            put32(zip, 0x02014B50);
            put16(zip, 20);
            put16(zip, 20);
            put16(zip, 0);
            put16(zip, 8);
            put16(zip, 0);
            put16(zip, 0);
            put32(zip, record.crc32);
            put32(zip, record.size / 2);
            put32(zip, record.size);
            put16(zip, static_cast<std::uint16_t>(record.name.size()));
            put16(zip, static_cast<std::uint16_t>(record.extra.size()));
            put16(zip, static_cast<std::uint16_t>(record.comment.size()));
            put16(zip, 0);
            put16(zip, 0);
            put32(zip, 0);
            put32(zip, 0);
            zip += record.name + record.extra + record.comment;
        }
        const auto directory_size = static_cast<std::uint32_t>(zip.size() - directory_offset);
        put32(zip, 0x06054B50);
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, static_cast<std::uint16_t>(records.size()));
        put16(zip, static_cast<std::uint16_t>(records.size()));
        put32(zip, directory_size);
        put32(zip, directory_offset);
        put16(zip, static_cast<std::uint16_t>(archive_comment.size()));
        zip += archive_comment;
        return zip;
    }

    /** @brief Writes @p bytes to a temporary file unique to this process and call. */
    std::filesystem::path write_temp(const std::string &bytes)
    {
        static int s_counter = 0;
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("hotpak_zip_" + std::to_string(_getpid()) + "_" + std::to_string(++s_counter) + ".pak");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    void remove_temp(const std::filesystem::path &path)
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    void reads_entries()
    {
        const std::filesystem::path path = write_temp(make_zip(
            {
                Record{.name = "Textures\\A.dds", .crc32 = 0x11111111, .size = 100},
                Record{.name = "libs/tables/x.xml", .crc32 = 0x22222222, .size = 7, .extra = "EXTRA", .comment = "c"},
            },
            "an archive comment"
        ));
        const auto index = read_zip_index(path.wstring());
        CHECK(index.has_value());
        CHECK(index.has_value() && index->size() == 2);
        if (index.has_value() && index->size() == 2)
        {
            CHECK((*index)[0].name == "textures/a.dds");
            CHECK((*index)[0].crc32 == 0x11111111 && (*index)[0].size == 100);
            CHECK((*index)[1].name == "libs/tables/x.xml");
            CHECK((*index)[1].crc32 == 0x22222222 && (*index)[1].size == 7);
        }
        remove_temp(path);
    }

    void rejects_broken_files()
    {
        const std::string whole = make_zip({Record{.name = "a.dds", .crc32 = 1, .size = 1}});
        const std::filesystem::path truncated = write_temp(whole.substr(0, whole.size() - 10));
        CHECK(!read_zip_index(truncated.wstring()).has_value());
        remove_temp(truncated);

        const std::filesystem::path tiny = write_temp("PK");
        CHECK(!read_zip_index(tiny.wstring()).has_value());
        remove_temp(tiny);

        CHECK(!read_zip_index(L"Z:\\hotpak\\no\\such\\file.pak").has_value());
    }

    void diffs_entries()
    {
        const std::vector<ZipEntry> previous{
            ZipEntry{.name = "a.dds", .crc32 = 1, .size = 10},
            ZipEntry{.name = "b.skin", .crc32 = 2, .size = 20},
            ZipEntry{.name = "c.mtl", .crc32 = 3, .size = 30},
        };
        const std::vector<ZipEntry> next{
            ZipEntry{.name = "a.dds", .crc32 = 1, .size = 10},
            ZipEntry{.name = "b.skin", .crc32 = 9, .size = 20},
            ZipEntry{.name = "c.mtl", .crc32 = 3, .size = 31},
            ZipEntry{.name = "d.lua", .crc32 = 4, .size = 40},
        };
        const std::vector<ZipEntry> changed = changed_entries(previous, next);
        CHECK(changed.size() == 3);
        CHECK(
            changed.size() == 3 && changed[0].name == "b.skin" && changed[1].name == "c.mtl" &&
            changed[2].name == "d.lua"
        );
        CHECK(changed_entries(next, next).empty());
        CHECK(changed_entries({}, next).size() == next.size());
    }
} // namespace

int main()
{
    reads_entries();
    rejects_broken_files();
    diffs_entries();
    return HotPak::tests::finish("zip_index");
}
