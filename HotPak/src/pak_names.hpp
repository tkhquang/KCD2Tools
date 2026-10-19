/**
 * @file pak_names.hpp
 * @brief What a pak entry name, a shadow-copy name or the INI pak list says, with no game access.
 *
 * Every entry name here is in normalize_path() form: lowercase, forward slashes, relative to the pak root.
 */
#ifndef HOTPAK_PAK_NAMES_HPP
#define HOTPAK_PAK_NAMES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HotPak
{
    /** @brief "x.dds" for "x.dds" and its split mips "x.dds.N", "x.dds.Na", "x.dds.a", or empty. */
    [[nodiscard]] std::string texture_base(std::string_view name);

    /** @brief "x.skin" for "x.skin" or "x_lodN.skin" (CSkin loads its LOD files itself), or empty. */
    [[nodiscard]] std::string skin_base(std::string_view name);

    /** @brief The CMatMan key of a ".mtl" entry, its path without the extension, or empty. */
    [[nodiscard]] std::string material_key(std::string_view name);

    /** @brief "<id>" for a mod init script "scripts/mods/<id>.lua", or empty. */
    [[nodiscard]] std::string_view mod_script_id(std::string_view name) noexcept;

    /** @brief True when @p id can name a Lua global and sit inside a Lua string literal unescaped. */
    [[nodiscard]] bool is_plain_script_id(std::string_view id) noexcept;

    /** @brief A table database, by its Libs/Tables subfolder and name, as an entry names it. */
    struct TableRef
    {
        /// Folder between "libs/tables/" and the file, empty for a top-level table.
        std::string_view subfolder;
        /// The table name, without a mod patch's "__<suffix>".
        std::string_view name;
    };

    /**
     * @brief The database a "libs/tables/<subfolder>/<name>.xml" or "<name>__<suffix>.xml" patch entry belongs to.
     * @return std::nullopt for any other entry.
     */
    [[nodiscard]] std::optional<TableRef> table_of(std::string_view name) noexcept;

    /** @brief The file name of a shadow copy: "<source stem>.g<NNNN>.pak". */
    [[nodiscard]] std::wstring shadow_file_name(std::wstring_view source_path, std::uint32_t generation);

    /** @brief The generation number of a shadow-copy file name, or 0 when @p file_name is not one. */
    [[nodiscard]] std::uint32_t shadow_generation_of(std::wstring_view file_name) noexcept;

    /** @brief The INI pak list, split. */
    struct PakList
    {
        /// The entries that name a .pak file, in INI order.
        std::vector<std::wstring> paths;
        /// The entries that do not end in ".pak" (ignored).
        std::vector<std::string> rejected;
    };

    /** @brief Splits a ';'-separated list, trims spaces, tabs and quotes, and keeps the entries that end in ".pak". */
    [[nodiscard]] PakList parse_pak_list(std::string_view text);

} // namespace HotPak

#endif // HOTPAK_PAK_NAMES_HPP
