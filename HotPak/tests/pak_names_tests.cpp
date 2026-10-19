/**
 * @file tests/pak_names_tests.cpp
 * @brief Unit tests of the pak entry, shadow-copy and INI pak-list name rules.
 */

#include "check.hpp"
#include "pak_names.hpp"

#include <string>

namespace
{
    using namespace HotPak;

    void texture_names()
    {
        CHECK(texture_base("textures/a.dds") == "textures/a.dds");
        CHECK(texture_base("textures/a.dds.3") == "textures/a.dds");
        CHECK(texture_base("textures/a.dds.3a") == "textures/a.dds");
        CHECK(texture_base("textures/a.dds.a") == "textures/a.dds");
        CHECK(texture_base("textures/a.ddsx").empty());
        CHECK(texture_base("textures/a.tif").empty());
    }

    void skin_names()
    {
        CHECK(skin_base("objects/fur.skin") == "objects/fur.skin");
        CHECK(skin_base("objects/fur_lod1.skin") == "objects/fur.skin");
        CHECK(skin_base("objects/fur_lodx.skin") == "objects/fur_lodx.skin");
        CHECK(skin_base("objects/_lod1.skin") == "objects/_lod1.skin");
        CHECK(skin_base("objects/fur.cgf").empty());
    }

    void material_names()
    {
        CHECK(material_key("objects/fur.mtl") == "objects/fur");
        CHECK(material_key(".mtl").empty());
        CHECK(material_key("objects/fur.mtl.bak").empty());
    }

    void script_names()
    {
        CHECK(mod_script_id("scripts/mods/mutt.lua") == "mutt");
        CHECK(mod_script_id("scripts/mods/sub/mutt.lua").empty());
        CHECK(mod_script_id("scripts/other/mutt.lua").empty());
        CHECK(mod_script_id("scripts/mods/mutt.txt").empty());
        CHECK(is_plain_script_id("mutt_the-shiba.v2"));
        CHECK(!is_plain_script_id(""));
        CHECK(!is_plain_script_id("mutt\"); os.exit() end"));
    }

    void table_names()
    {
        const auto plain = table_of("libs/tables/character/clothingmaterial.xml");
        CHECK(plain.has_value() && plain->subfolder == "character" && plain->name == "clothingmaterial");
        const auto patch = table_of("libs/tables/character/clothingmaterial__mutt.xml");
        CHECK(patch.has_value() && patch->subfolder == "character" && patch->name == "clothingmaterial");
        const auto nested = table_of("libs/tables/rpg/item/weapon.xml");
        CHECK(nested.has_value() && nested->subfolder == "rpg/item" && nested->name == "weapon");
        const auto top = table_of("libs/tables/soul.xml");
        CHECK(top.has_value() && top->subfolder.empty() && top->name == "soul");
        CHECK(!table_of("libs/tables/character/__mutt.xml").has_value());
        CHECK(!table_of("libs/other/soul.xml").has_value());
        CHECK(!table_of("libs/tables/soul.tbl").has_value());
    }

    void shadow_names()
    {
        CHECK(shadow_file_name(L"D:\\build\\Data\\mutt_the_shiba.pak", 7) == L"mutt_the_shiba.g0007.pak");
        CHECK(shadow_file_name(L"mutt.pak", 12345) == L"mutt.g12345.pak");
        CHECK(shadow_generation_of(L"mutt_the_shiba.g0007.pak") == 7);
        CHECK(shadow_generation_of(L"mutt.g12345.pak") == 12345);
        CHECK(shadow_generation_of(L"mutt.pak") == 0);
        CHECK(shadow_generation_of(L"my.game.pak") == 0);
        CHECK(shadow_generation_of(L"mutt.g.pak") == 0);
        CHECK(shadow_generation_of(L"mutt.g0007.zip") == 0);
        CHECK(shadow_generation_of(L"mutt.g1234567890.pak") == 0);
    }

    void pak_lists()
    {
        const PakList list = parse_pak_list(" \"D:\\a\\Mutt.PAK\" ; ;D:\\b\\notes.txt;D:\\c\\b.pak ");
        CHECK(list.paths.size() == 2);
        CHECK(list.paths.size() == 2 && list.paths[0] == L"D:\\a\\Mutt.PAK" && list.paths[1] == L"D:\\c\\b.pak");
        CHECK(list.rejected.size() == 1 && list.rejected[0] == "D:\\b\\notes.txt");
        CHECK(parse_pak_list("").paths.empty());
    }
} // namespace

int main()
{
    texture_names();
    skin_names();
    material_names();
    script_names();
    table_names();
    shadow_names();
    pak_lists();
    return HotPak::tests::finish("pak_names");
}
