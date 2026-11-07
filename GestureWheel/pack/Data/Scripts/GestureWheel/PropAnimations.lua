-- Generated from the game's data: for each animated prop model, the fragments the game plays on it
-- too, each option's clip, fragment tags, tags and whether it loops (Mannequin ADBs of the model's own clips).
local tables = {
	{
		["ADLG_ShowBook"] = {
			{ clip = "dlg_pull_out_book", fragTags = "", tags = "", loop = false },
		},
		["ReadingBook"] = {
			{ clip = "reading_table_inv_loop", fragTags = "book", tags = "sitting", loop = true },
			{ clip = "male_reading_stand_loop", fragTags = "bookStand+book", tags = "", loop = true },
			{ clip = "reading_bench_inv_loop", fragTags = "book", tags = "sittingNoTable", loop = true },
			{ clip = "reading_stand_inv_loop", fragTags = "book", tags = "", loop = true },
		},
		["ReadingBookIn"] = {
			{ clip = "reading_table_inv_in", fragTags = "book", tags = "sitting", loop = false },
			{ clip = "reading_stand_bookshelf_down_in", fragTags = "bookShelfDown+bookShelf+book", tags = "", loop = false },
			{ clip = "reading_bench_inv_in", fragTags = "book", tags = "sittingNoTable", loop = false },
			{ clip = "reading_stand_inv_in", fragTags = "book", tags = "", loop = false },
			{ clip = "male_reading_stand_in", fragTags = "bookStand+book", tags = "", loop = false },
			{ clip = "reading_stand_bookshelf_middle_in", fragTags = "bookShelfMid+bookShelf+book", tags = "", loop = false },
		},
		["ReadingBookOut"] = {
			{ clip = "male_reading_stand_out", fragTags = "bookStand+book", tags = "", loop = false },
			{ clip = "reading_stand_bookshelf_middle_out", fragTags = "bookShelfMid+bookShelf+book", tags = "", loop = false },
			{ clip = "reading_bench_inv_out", fragTags = "book", tags = "sittingNoTable", loop = false },
			{ clip = "reading_table_inv_out", fragTags = "book", tags = "sitting", loop = false },
			{ clip = "reading_stand_bookshelf_down_out", fragTags = "bookShelfDown+bookShelf+book", tags = "", loop = false },
			{ clip = "reading_stand_inv_out", fragTags = "book", tags = "", loop = false },
		},
		["ReadingBookVAR"] = {
			{ clip = "reading_table_inv_var1", fragTags = "book", tags = "sitting", loop = false },
			{ clip = "reading_stand_inv_var3", fragTags = "book", tags = "", loop = false },
			{ clip = "male_reading_stand_loop_var01", fragTags = "bookStand+book", tags = "", loop = false },
			{ clip = "reading_bench_inv_var3", fragTags = "book", tags = "sittingNoTable", loop = false },
			{ clip = "reading_stand_inv_var2", fragTags = "book", tags = "", loop = false },
			{ clip = "reading_table_inv_var2", fragTags = "book", tags = "sitting", loop = false },
			{ clip = "reading_bench_inv_var1", fragTags = "book", tags = "sittingNoTable", loop = false },
			{ clip = "reading_stand_inv_var1", fragTags = "book", tags = "", loop = false },
			{ clip = "male_reading_stand_loop_var04", fragTags = "bookStand+book", tags = "", loop = false },
			{ clip = "male_reading_stand_loop_var02", fragTags = "bookStand+book", tags = "", loop = false },
			{ clip = "reading_bench_inv_var2", fragTags = "book", tags = "sittingNoTable", loop = false },
			{ clip = "male_reading_stand_loop_var03", fragTags = "bookStand+book", tags = "", loop = false },
			{ clip = "reading_table_inv_var3", fragTags = "book", tags = "sitting", loop = false },
		},
		["ReadingBookstand"] = {
			{ clip = "male_reading_stand_loop", fragTags = "bookStand+book", tags = "", loop = true },
		},
		["ReadingBookstandIn"] = {
			{ clip = "male_reading_stand_in", fragTags = "bookStand+book", tags = "", loop = false },
		},
		["ReadingBookstandOut"] = {
			{ clip = "male_reading_stand_out", fragTags = "bookStand+book", tags = "", loop = false },
		},
		["ReadingFromShelfIn"] = {
			{ clip = "reading_stand_bookshelf_down_in", fragTags = "bookShelfDown+book", tags = "", loop = false },
			{ clip = "reading_stand_bookshelf_up_in", fragTags = "bookShelfUp+book", tags = "", loop = false },
			{ clip = "reading_stand_bookshelf_middle_in", fragTags = "bookShelfMid+book", tags = "", loop = false },
		},
		["ReadingFromShelfOut"] = {
			{ clip = "reading_stand_bookshelf_middle_out", fragTags = "bookShelfMid+book", tags = "", loop = false },
			{ clip = "reading_stand_bookshelf_down_out", fragTags = "bookShelfDown+book", tags = "", loop = false },
			{ clip = "reading_stand_bookshelf_up_out", fragTags = "bookShelfUp+book", tags = "", loop = false },
		},
		["Scribe_BookFromRead"] = {
			{ clip = "scribe_book_from_read", fragTags = "book", tags = "", loop = false },
		},
		["Scribe_BookToRead"] = {
			{ clip = "scribe_book_to_read", fragTags = "book", tags = "", loop = false },
		},
	},
	{
		["Charge"] = {
			{ clip = "bow_pull_back_over", fragTags = "", tags = "l_bow", loop = false },
			{ clip = "bow_crouched_down_pull_back_over", fragTags = "", tags = "stealth+l_bow", loop = false },
			{ clip = "bow_pull_back_rider_over", fragTags = "", tags = "horse+l_bow", loop = false },
			{ clip = "bow_palisade_lean_in", fragTags = "", tags = "l_bow+prime+battleArchery", loop = false },
			{ clip = "bow_pull_back_over_female", fragTags = "", tags = "l_bow", loop = false },
			{ clip = "bow_crouched_pull_back_over", fragTags = "", tags = "stealth+l_bow", loop = false },
		},
		["Charged"] = {
			{ clip = "bow_idle_pulled", fragTags = "", tags = "l_bow+charge+prime", loop = false },
			{ clip = "bow_idle_pulled_female", fragTags = "", tags = "l_bow+charge+prime", loop = false },
		},
		["Fire"] = {
			{ clip = "bow_palisade_lean_shoot", fragTags = "", tags = "l_bow+battleArchery", loop = false },
			{ clip = "bow_shot_over", fragTags = "", tags = "l_bow", loop = false },
			{ clip = "bow_shot_over_female", fragTags = "", tags = "l_bow+charge+prime", loop = false },
			{ clip = "bow_shot_over_female", fragTags = "", tags = "l_bow", loop = false },
		},
		["HitDeath"] = {
			{ clip = "bow_pulled_hit_left", fragTags = "so_back+major_hit", tags = "l_bow+charge+prime", loop = false },
			{ clip = "bow_pulled_hit_right", fragTags = "so_right+major_hit", tags = "l_bow+charge+prime", loop = false },
			{ clip = "bow_pulled_hit_left", fragTags = "so_left+major_hit", tags = "l_bow+charge+prime", loop = false },
			{ clip = "bow_pulled_hit_right", fragTags = "so_forward+major_hit", tags = "l_bow+charge+prime", loop = false },
		},
		["HolsterWeapon"] = {
			{ clip = "bow_idle_2nd_loaded", fragTags = "", tags = "l_bow+player", loop = false },
		},
		["Load"] = {
			{ clip = "bow_load_rider_over", fragTags = "", tags = "horse+l_bow", loop = false },
			{ clip = "bow_crouched_load_over", fragTags = "", tags = "stealth+l_bow", loop = false },
			{ clip = "bow_load_loop_over_player", fragTags = "", tags = "l_bow+afterShot+player", loop = false },
			{ clip = "bow_load_1st_over", fragTags = "", tags = "l_bow", loop = false },
			{ clip = "bow_load_2nd_over_player", fragTags = "", tags = "l_bow+player", loop = false },
			{ clip = "bow_load_loop_over", fragTags = "", tags = "l_bow+afterShot", loop = false },
		},
		["MotionIdle"] = {
			{ clip = "bow_idle_2nd_loaded", fragTags = "", tags = "l_bow+prime", loop = true },
			{ clip = "bow_idle", fragTags = "", tags = "l_bow+afterShot", loop = true },
		},
		["MotionMovement"] = {
			{ clip = "bow_idle_2nd_loaded", fragTags = "", tags = "walk+l_bow+prime", loop = true },
			{ clip = "bow_idle_pulled", fragTags = "", tags = "walk+backward+l_bow+charge+prime", loop = true },
			{ clip = "bow_idle_2nd_loaded", fragTags = "", tags = "run+l_bow+prime", loop = true },
		},
		["Uncharge"] = {
			{ clip = "bow_release_over", fragTags = "", tags = "l_bow", loop = false },
			{ clip = "bow_release_rider_over", fragTags = "", tags = "horse+l_bow", loop = false },
			{ clip = "bow_crouched_release_over", fragTags = "", tags = "stealth+l_bow", loop = false },
			{ clip = "bow_release_over_female", fragTags = "", tags = "l_bow", loop = false },
		},
		["WeaponIdle"] = {
			{ clip = "bow_idle_loaded", fragTags = "", tags = "l_bow", loop = true },
			{ clip = "bow_idle_loaded_female", fragTags = "", tags = "l_bow", loop = true },
		},
		["WeaponShake"] = {
			{ clip = "bow_idle_pulled", fragTags = "", tags = "l_bow+charge+prime", loop = true },
			{ clip = "bow_idle_pulled_female", fragTags = "", tags = "l_bow+charge+prime", loop = true },
		},
	},
}
GestureWheel.PropAnimations = {
	["objects/characters/assets/book/book_01.cdf"] = tables[1],
	["objects/characters/assets/parchment_folded/parchment_folded.cdf"] = tables[1],
	["objects/manmade/weapons/bows/bow_a.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_b.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_b_a.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_b_b.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_c.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_c_a.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_c_b.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_c_c.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_d.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_d_a.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_e.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_e_a.cdf"] = tables[2],
	["objects/manmade/weapons/bows/bow_e_b.cdf"] = tables[2],
	["objects/manmade/weapons/bows/cuman_bow_a.cdf"] = tables[2],
	["objects/manmade/weapons/bows/cuman_bow_a_a.cdf"] = tables[2],
	["objects/manmade/weapons/bows/cuman_bow_a_b.cdf"] = tables[2],
	["objects/manmade/weapons/bows/cuman_bow_b.cdf"] = tables[2],
}
-- The clip the game opens each model with when Henry reads it (entity scripts' AnimOpen).
GestureWheel.PropOpen = {
	["objects/characters/assets/book/book_01.cdf"] = "open",
}
