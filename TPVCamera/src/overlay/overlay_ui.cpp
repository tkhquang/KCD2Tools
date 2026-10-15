/**
 * @file overlay/overlay_ui.cpp
 * @brief Preset-manager panel: the body of TPVCamera::Overlay::draw_ui().
 *
 * @details Renders the "TPV Camera - Presets" window using Dear ImGui called
 *          directly. It is invoked by the overlay render loop inside an already-active
 *          ImGui frame, so it only issues widget calls - it never begins/ends the ImGui
 *          frame, creates the context, or touches the device.
 *
 *          The panel drives PresetStore CRUD (list selection, rename, new/duplicate/
 *          remove/reset-to-factory, save) and the per-preset field editors generated
 *          from Presets::fields(). Every field mutation this frame calls
 *          PresetStore::mark_dirty(), which republishes the live binding table so the
 *          edit previews on the third-person camera in real time. The overlay never
 *          sets any menu/overlay GameState bit, so the camera keeps rendering live
 *          while the panel is open.
 *
 *          Presets bound to exactly the same states form a cycle group. The panel
 *          tags each member in the list, lists every group with the member it uses,
 *          and sets that member on a click through the same published pick the
 *          cycle keys change.
 */

#include "overlay.hpp"

#include "config.hpp"
#include "game_state.hpp"
#include "global_state.hpp"
#include "presets/camera_preset.hpp"
#include "presets/camera_preset_fields.hpp"
#include "presets/preset_runtime.hpp"
#include "presets/preset_store.hpp"

#include <DetourModKit.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TPVCamera::Overlay
{

    namespace
    {

        using Presets::CameraPreset;
        using Presets::FieldType;
        using Presets::PresetField;
        using Presets::PresetStore;

        /// Inline-rename scratch: the row index currently being renamed (-1 when none).
        int s_rename_index = -1;
        /// Inline-rename scratch: the edit buffer backing the rename InputText.
        char s_rename_buffer[128] = {};

        /// Tint applied to the Save button while there are unsaved changes.
        constexpr ImVec4 k_save_dirty_normal{0.80f, 0.40f, 0.15f, 1.0f};
        constexpr ImVec4 k_save_dirty_hovered{0.95f, 0.52f, 0.20f, 1.0f};
        constexpr ImVec4 k_save_dirty_active{1.00f, 0.60f, 0.25f, 1.0f};

        /// Green tint marking the preset currently driving the camera (name text + "[active]" tag).
        constexpr ImVec4 k_active_preset{0.40f, 0.90f, 0.45f, 1.0f};

        /// Amber tint marking cycle groups: presets bound to exactly the same states, which the cycle keys switch.
        constexpr ImVec4 k_cycle_group{0.95f, 0.75f, 0.30f, 1.0f};

        /// Label of the umbrella minigame binding, which matches whichever minigame is on screen.
        constexpr const char *k_any_minigame_label = "Any minigame";

        /// Tooltip of the cycle tag a preset list row carries when other presets share its exact states.
        constexpr const char *k_cycle_tag_tooltip =
            "Presets bound to exactly the same states form a cycle group, and * marks the one used for them.\n"
            "NextPresetKey / PreviousPresetKey (INI) switch it while those states are active,\n"
            "or pick one under Cycle groups.";

        /// The framing states a user preset can auto-bind to, with their display label and GameState bit.
        struct BindOption
        {
            const char *label;
            GameState bit;
        };
        constexpr BindOption k_bind_options[] = {
            {"Aiming (bow / crossbow)", GameState::Aiming},
            {"Crouch / Stealth", GameState::Crouch},
            {"Combat (weapon drawn)", GameState::Combat},
            {"Mount (horseback)", GameState::Mount},
            {"Lying down (sleeping in bed)", GameState::Lying},
            {"Sitting (bench / chair)", GameState::Sitting},
            {"Kneeling", GameState::Kneel},
            {"Cart (riding / driving)", GameState::Cart},
            {"Indoors (inside a house, shed or barn)", GameState::Indoors},
        };

        /**
         * @struct CycleGroup
         * @brief Two or more presets bound to exactly the same states, which the cycle keys switch between.
         */
        struct CycleGroup
        {
            /// The bind mask every member shares.
            std::uint32_t mask = 0;
            /// Member names in cycle order, which is the publish order (built-ins first).
            std::vector<std::string> members;
            /// Index into @ref members of the member the group uses.
            int in_use = 0;
        };

        /**
         * @struct PanelFrame
         * @brief This frame's reading of the published snapshot, shared by the preset list, the cycle panel and the
         *        bind editor so they agree with each other and with the render thread.
         */
        struct PanelFrame
        {
            /// Store row of the preset driving the camera (the editing row while pinned), or -1.
            int active_row = -1;
            /// Mask of the group the current state resolves to, or empty before the store's first publish.
            std::optional<std::uint32_t> state_mask;
            /// Every cycle group, in the order of its first member.
            std::vector<CycleGroup> groups;
        };

        /**
         * @struct CycleSlot
         * @brief Where a preset sits in its cycle group.
         */
        struct CycleSlot
        {
            /// The preset's group, or nullptr when no other preset shares its states.
            const CycleGroup *group = nullptr;
            /// Index of the preset in group->members.
            int position = 0;
        };

        /**
         * @brief Display label for the states a bind mask applies on: the bind editor's labels joined by " + ".
         * @param mask A bind mask. The empty mask is the DEFAULT floor.
         */
        [[nodiscard]] std::string describe_states(std::uint32_t mask)
        {
            if (mask == 0)
            {
                return "Default (no other state)";
            }
            std::string label;
            const auto append = [&label](std::string_view part)
            {
                if (!label.empty())
                {
                    label += " + ";
                }
                label += part;
            };
            for (const BindOption &option : k_bind_options)
            {
                if ((mask & state_bit(option.bit)) != 0)
                {
                    append(option.label);
                }
            }
            // A per-minigame bind also carries the umbrella bit, so it is named by its child alone.
            const auto child = std::find_if(k_minigames.begin(), k_minigames.end(), [mask](const MinigameInfo &minigame)
                                            { return (mask & state_bit(minigame.bit)) != 0; });
            if (child != k_minigames.end())
            {
                append(child->label);
            }
            else if ((mask & state_bit(GameState::Minigame)) != 0)
            {
                append(k_any_minigame_label);
            }
            return label;
        }

        /**
         * @brief Shows @p text as a hovered tooltip if the previous item is hovered and text is non-empty.
         * @param text Tooltip body; ignored when null or empty.
         */
        void hover_tooltip(const char *text)
        {
            if (text == nullptr || text[0] == '\0')
            {
                return;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(text);
                ImGui::EndTooltip();
            }
        }

        /**
         * @brief Draws a dimmed "(?)" marker that shows @p text as a wrapped tooltip on hover.
         * @details Placed before each field label so the per-field explanation (mirroring the INI comments)
         *          is discoverable, like the ImGui demo's HelpMarker.
         */
        void help_marker(const char *text)
        {
            ImGui::TextDisabled("(?)");
            if (text != nullptr && text[0] != '\0' && ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                ImGui::TextUnformatted(text);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }

        /**
         * @brief Draws a small clickable reload/reset icon and returns true on click.
         * @details The overlay loads only the default ImGui font (ASCII range), so a Unicode reload glyph would
         *          render as a missing-glyph box. The icon is drawn with ImDrawList primitives instead: a 3/4
         *          circular arc with an arrowhead at its terminus, sized to the frame height so it sits inline
         *          with a field row. It tints brighter on hover so it reads as interactive without a frame.
         * @param id Unique ImGui id for the invisible hit-test button (e.g. "##reset" inside a field PushID).
         */
        [[nodiscard]] bool reload_icon_button(const char *id)
        {
            const float diameter = ImGui::GetFrameHeight();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(diameter, diameter));
            const bool hovered = ImGui::IsItemHovered();

            ImDrawList *draw = ImGui::GetWindowDrawList();
            const ImVec2 center(origin.x + diameter * 0.5f, origin.y + diameter * 0.5f);
            const float radius = diameter * 0.30f;
            const float thickness = (std::max)(1.0f, diameter * 0.09f);
            const ImU32 color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);

            // 3/4 arc from -45 degrees sweeping clockwise to +270 degrees; the open slice carries the arrowhead.
            constexpr float k_pi = 3.14159265358979f;
            const float arc_begin = -k_pi * 0.25f;
            const float arc_end = k_pi * 1.5f;
            draw->PathArcTo(center, radius, arc_begin, arc_end, 24);
            draw->PathStroke(color, ImDrawFlags_None, thickness);

            // Arrowhead at the arc terminus (arc_begin), pointing along the clockwise sweep tangent.
            const float tip_x = center.x + std::cos(arc_begin) * radius;
            const float tip_y = center.y + std::sin(arc_begin) * radius;
            const float tangent_x = std::sin(arc_begin);
            const float tangent_y = -std::cos(arc_begin);
            const float radial_x = std::cos(arc_begin);
            const float radial_y = std::sin(arc_begin);
            const float head = (std::max)(2.0f, diameter * 0.20f);
            const ImVec2 p_tip(tip_x + tangent_x * head, tip_y + tangent_y * head);
            const ImVec2 p_in(tip_x - radial_x * head * 0.7f, tip_y - radial_y * head * 0.7f);
            const ImVec2 p_out(tip_x + radial_x * head * 0.7f, tip_y + radial_y * head * 0.7f);
            draw->AddTriangleFilled(p_tip, p_in, p_out, color);
            return clicked;
        }

        /**
         * @brief Begins the confirmation modal @p id centered over the preset window.
         * @details Call it inside the preset window, in the ID scope that opened the popup. The centering is queued
         *          only while the popup is open, so the next-window position cannot leak onto another window, and it
         *          targets the preset window rather than the display, so the modal appears where the user is working.
         * @return True while the modal is open: draw its contents, then call ImGui::EndPopup.
         */
        [[nodiscard]] bool begin_confirm_modal(const char *id)
        {
            if (ImGui::IsPopupOpen(id))
            {
                const ImVec2 win_pos = ImGui::GetWindowPos();
                const ImVec2 win_size = ImGui::GetWindowSize();
                ImGui::SetNextWindowPos(ImVec2(win_pos.x + win_size.x * 0.5f, win_pos.y + win_size.y * 0.5f),
                                        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            }
            return ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        }

        /**
         * @brief Begins inline rename of preset row @p index, seeding the scratch buffer with its name.
         * @param name  Current preset name to pre-fill into the edit buffer.
         * @param index Row index entering rename mode.
         */
        void begin_rename(const std::string &name, int index)
        {
            s_rename_index = index;
            std::snprintf(s_rename_buffer, sizeof(s_rename_buffer), "%s", name.c_str());
        }

        /** @brief Clears any active inline-rename session. */
        void cancel_rename() noexcept
        {
            s_rename_index = -1;
            s_rename_buffer[0] = '\0';
        }

        /**
         * @brief Reads the published snapshot for this frame.
         * @details Resolves with the render thread's own snapshot and functions (resolve_active_entry,
         *          cycle_group_entry), so the highlight and the groups cannot drift from the live camera. The pin
         *          overrides the state for the camera only, so it moves the active row but not the state's group.
         *          The snapshot names its presets, so a row is found by name, which the store keeps unique.
         * @param store The process-wide preset store.
         */
        [[nodiscard]] PanelFrame read_panel_frame(PresetStore &store)
        {
            PanelFrame frame;
            const std::shared_ptr<const Presets::StateBindingTable> table = Presets::published_table();
            if (!table)
            {
                return frame;
            }
            const std::vector<std::uint32_t> &masks = table->masks;
            const std::vector<CameraPreset> &entries = table->presets;
            const std::size_t entry_count = std::min(masks.size(), entries.size());

            const std::uint32_t state = game_state_mask().load(std::memory_order_relaxed);
            const int active_entry = Presets::resolve_active_entry(*table, state);
            if (active_entry >= 0)
            {
                const std::string &active_name = entries[static_cast<std::size_t>(active_entry)].name;
                const auto &presets = store.presets();
                const auto row = std::find_if(presets.begin(), presets.end(), [&active_name](const CameraPreset &preset)
                                              { return preset.name == active_name; });
                frame.active_row = (row != presets.end()) ? static_cast<int>(row - presets.begin()) : -1;
                frame.state_mask = masks[static_cast<std::size_t>(active_entry)];
            }
            if (store.editing_pinned())
            {
                frame.active_row = store.editing_index();
            }

            for (std::size_t i = 0; i < entry_count; ++i)
            {
                // Gather each group at its first entry. A later entry of the same mask already belongs to it.
                const auto first = masks.begin() + static_cast<std::ptrdiff_t>(i);
                if (std::find(masks.begin(), first, masks[i]) != first)
                {
                    continue;
                }
                const std::vector<std::size_t> members = Presets::cycle_group_members(*table, masks[i]);
                if (members.size() < 2)
                {
                    continue;
                }
                CycleGroup group;
                group.mask = masks[i];
                const int in_use = Presets::cycle_group_entry(*table, masks[i]);
                for (std::size_t j = 0; j < members.size(); ++j)
                {
                    if (static_cast<int>(members[j]) == in_use)
                    {
                        group.in_use = static_cast<int>(j);
                    }
                    group.members.push_back(entries[members[j]].name);
                }
                frame.groups.push_back(std::move(group));
            }
            return frame;
        }

        /** @brief Where the preset named @p name sits in its cycle group, or an empty slot when it is in none. */
        [[nodiscard]] CycleSlot find_cycle_slot(const PanelFrame &frame, std::string_view name) noexcept
        {
            for (const CycleGroup &group : frame.groups)
            {
                for (std::size_t i = 0; i < group.members.size(); ++i)
                {
                    if (group.members[i] == name)
                    {
                        return CycleSlot{
                            .group = &group,
                            .position = static_cast<int>(i),
                        };
                    }
                }
            }
            return {};
        }

        /**
         * @brief Renders the CollapsingHeader preset list with selection, inline rename, an active-preset
         *        highlight (green name + "[active]" tag) marking the preset currently driving the camera, and a
         *        cycle tag on every preset that shares its exact states with others.
         * @param store The process-wide preset store (already validated to be non-empty).
         * @param frame This frame's reading of the published snapshot.
         */
        void draw_preset_list(PresetStore &store, const PanelFrame &frame)
        {
            if (!ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            auto &presets = store.presets();
            const int count = static_cast<int>(presets.size());
            const int selected = store.editing_index();
            const int active = frame.active_row;

            for (int i = 0; i < count; ++i)
            {
                ImGui::PushID(i);

                CameraPreset &preset = presets[static_cast<std::size_t>(i)];
                const bool can_rename = !preset.builtin;

                if (s_rename_index == i && can_rename)
                {
                    // Inline rename: commit on Enter, cancel on Escape or focus loss.
                    ImGui::SetNextItemWidth(200.0f);
                    ImGui::SetKeyboardFocusHere();
                    const bool committed = ImGui::InputText("##rename", s_rename_buffer, sizeof(s_rename_buffer),
                                                            ImGuiInputTextFlags_EnterReturnsTrue);
                    if (committed)
                    {
                        if (!store.rename(i, std::string(s_rename_buffer)))
                        {
                            DMK::log().warning(
                                "Overlay: rename of preset index {} rejected (empty/duplicate/built-in).", i);
                        }
                        cancel_rename();
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                             (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()))
                    {
                        cancel_rename();
                    }
                }
                else
                {
                    const bool is_selected = (i == selected);
                    const bool is_active = (i == active);

                    // Tint the row of the preset currently driving the camera green so it is obvious which
                    // one is live; the tint wraps only the Selectable draw so the click handling is unchanged.
                    if (is_active)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, k_active_preset);
                    }
                    const bool clicked = ImGui::Selectable(preset.name.c_str(), is_selected);
                    if (is_active)
                    {
                        ImGui::PopStyleColor();
                    }
                    if (clicked)
                    {
                        store.set_editing_index(i);
                    }

                    // Double-click a user preset to begin inline rename; built-ins are fixed.
                    if (can_rename && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    {
                        store.set_editing_index(i);
                        begin_rename(preset.name, i);
                    }

                    if (is_active)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(k_active_preset, "[active]");
                        hover_tooltip("Currently driving the camera (chosen by game state, or pinned for preview).");
                    }

                    if (preset.builtin)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(built-in)");
                    }

                    if (const CycleSlot slot = find_cycle_slot(frame, preset.name); slot.group != nullptr)
                    {
                        const int position = slot.position + 1;
                        const int members = static_cast<int>(slot.group->members.size());
                        ImGui::SameLine();
                        if (slot.position == slot.group->in_use)
                        {
                            ImGui::TextColored(k_cycle_group, "[cycle %d/%d *]", position, members);
                        }
                        else
                        {
                            ImGui::TextColored(k_cycle_group, "[cycle %d/%d]", position, members);
                        }
                        hover_tooltip(k_cycle_tag_tooltip);
                    }
                }

                ImGui::PopID();
            }
        }

        /**
         * @brief Renders every cycle group: the states it shares, its members, and the member it uses.
         * @details A click on a member makes the group use it, as the cycle keys do while its states are active, so
         *          the player can choose without stepping through the group in game. The group the current state
         *          resolves to is tagged, since only its states respond to the keys right now.
         * @param frame This frame's reading of the published snapshot.
         */
        void draw_cycle_groups(const PanelFrame &frame)
        {
            if (!ImGui::CollapsingHeader("Cycle groups", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }
            if (frame.groups.empty())
            {
                ImGui::TextDisabled("None. Bind two or more presets to exactly the same states to cycle them.");
                return;
            }
            ImGui::TextDisabled("NextPresetKey / PreviousPresetKey (INI) switch the one the current state uses.");

            for (std::size_t i = 0; i < frame.groups.size(); ++i)
            {
                const CycleGroup &group = frame.groups[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextColored(k_cycle_group, "%s", describe_states(group.mask).c_str());
                if (frame.state_mask == group.mask)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(k_active_preset, "[current state]");
                    hover_tooltip("The game is in these states now, so the cycle keys switch this group.");
                }
                ImGui::Indent();
                for (std::size_t j = 0; j < group.members.size(); ++j)
                {
                    const bool in_use = static_cast<int>(j) == group.in_use;
                    if (ImGui::RadioButton(group.members[j].c_str(), in_use) && !in_use)
                    {
                        Presets::set_cycle_pick(group.mask, group.members[j]);
                        // A pick for the current state switches the camera now, so it eases the zoom out as a cycle
                        // key does.
                        if (frame.state_mask == group.mask)
                        {
                            camera_state().zoom_reset_requested.store(true, std::memory_order_relaxed);
                        }
                    }
                }
                ImGui::Unindent();
                ImGui::PopID();
            }
        }

        /**
         * @brief Notes under the bind editor that the edited preset shares its exact states with other presets.
         * @param frame This frame's reading of the published snapshot.
         * @param name The edited preset's name.
         */
        void draw_cycle_note(const PanelFrame &frame, std::string_view name)
        {
            const CycleSlot slot = find_cycle_slot(frame, name);
            if (slot.group == nullptr)
            {
                return;
            }
            const int others = static_cast<int>(slot.group->members.size()) - 1;
            ImGui::TextColored(k_cycle_group, "Shares these states with %d other preset%s (see Cycle groups).", others,
                               (others == 1) ? "" : "s");
        }

        /**
         * @brief Renders the pin-for-live-preview checkbox bridging editing_pinned/set_editing_pinned.
         * @param store The process-wide preset store.
         */
        void draw_pin_toggle(PresetStore &store)
        {
            bool pinned = store.editing_pinned();
            if (ImGui::Checkbox("Pin selected for live preview", &pinned))
            {
                store.set_editing_pinned(pinned);
            }
            hover_tooltip("Preview the edited preset live regardless of the in-game state\n"
                          "(combat / mount / aiming). Unpin to let state selection drive the camera again.");
        }

        /**
         * @brief Renders the New / Duplicate / Remove / Reset to factory / Reset all / Save button row.
         * @param store    The process-wide preset store.
         * @param selected The currently selected (and edited) preset.
         */
        void draw_button_row(PresetStore &store, const CameraPreset &selected)
        {
            const int editing = store.editing_index();
            // Read what is needed from `selected` up front: New/Duplicate/Remove mutate the presets vector
            // (push_back/erase can reallocate), which would dangle `selected`. After the captured copies,
            // `selected` is never dereferenced again in this function.
            const bool is_builtin = selected.builtin;
            const bool has_factory = Presets::factory_preset(selected.name).has_value();

            if (ImGui::Button("New"))
            {
                store.add_new();
            }
            hover_tooltip("Create a new user preset seeded from the current values and select it.");

            ImGui::SameLine();
            ImGui::BeginDisabled(is_builtin);
            if (ImGui::Button("Duplicate"))
            {
                store.duplicate(editing);
            }
            ImGui::EndDisabled();
            hover_tooltip(is_builtin ? "Built-in presets cannot be duplicated."
                                     : "Clone the selected preset into a new user preset and select it.");

            ImGui::SameLine();
            ImGui::BeginDisabled(is_builtin);
            if (ImGui::Button("Remove"))
            {
                store.remove(editing);
            }
            ImGui::EndDisabled();
            hover_tooltip(is_builtin ? "Built-in presets cannot be removed." : "Delete the selected user preset.");

            // Reset to factory exists for the built-ins and the factory starting points beside them.
            if (has_factory)
            {
                ImGui::SameLine();
                if (ImGui::Button("Reset to factory"))
                {
                    store.reset_to_factory(editing);
                }
                hover_tooltip(is_builtin
                                  ? "Restore this built-in to its factory values, and add back any factory preset\n"
                                    "for the same states that is missing. Shared settings keep their value."
                                  : "Restore this preset to its factory values. Shared settings keep their value.");
            }

            // Reset all overwrites the tuning of every factory preset at once, so it asks before it applies.
            ImGui::SameLine();
            if (ImGui::Button("Reset all"))
            {
                ImGui::OpenPopup("##resetallconfirm");
            }
            hover_tooltip("Restore every built-in and shipped preset to its factory values (asks first).\n"
                          "Your own presets are kept.");
            if (begin_confirm_modal("##resetallconfirm"))
            {
                ImGui::Text("Reset all factory presets?");
                ImGui::Spacing();
                // A fixed wrap width sizes the auto-resizing modal. TextWrapped alone wraps at the width of the short
                // title line.
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                ImGui::TextUnformatted("Every built-in preset, Close Shoulder and Aiming Wide go back to their factory "
                                       "values and states, and any of them you deleted comes back. DEFAULT and AIMING "
                                       "become the presets in use again. Your own presets and Shared settings are "
                                       "kept.");
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
                if (ImGui::Button("Reset all"))
                {
                    store.reset_all_to_factory();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            const bool dirty = store.dirty();
            if (dirty)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, k_save_dirty_normal);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k_save_dirty_hovered);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, k_save_dirty_active);
            }
            if (ImGui::Button(dirty ? "Save *" : "Save"))
            {
                store.save();
            }
            if (dirty)
            {
                ImGui::PopStyleColor(3);
            }
            hover_tooltip(dirty ? "Write all presets to disk (unsaved changes pending)."
                                : "Write all presets to disk.");
        }

        /**
         * @brief Edits one float preset field: a range slider, a direct numeric input, fine + coarse steppers,
         *        and a (?) help marker.
         * @param field    Field metadata (label, range, coarse step, tooltip).
         * @param value    The float to edit in place.
         * @param decimals Display + round precision (2 or 3).
         * @param default_value Value restored by the per-field reset button (R).
         * @return True if the value changed this frame.
         * @details Row: [ slider ] [ input ] << < > >> R (?) Label. The slider drags across the bound range; the
         *          input box takes an EXACT typed value (commit on Enter or focus loss) for when you know the
         *          number you want; the single arrows step by the field's FINE step (0.01) and the << / >> double
         *          arrows step 10x that (0.1). All four steppers press-and-hold repeat. Slider / arrow / committed
         *          edits are clamped to the range and quantised to @p decimals (no 0.30000001 noise); a value is
         *          left UNTOUCHED while the input box is focused so the clamp does not fight a partial entry (e.g.
         *          typing "0." for a field whose minimum is 0.5). Explicit item widths keep the row content-sized
         *          so the auto-resizing window hugs it.
         */
        [[nodiscard]] bool edit_float(const PresetField &field, float &value, int decimals, float default_value)
        {
            const float fine_step = field.fine_step;           // single arrows: per-field precise nudge
            const float coarse_step = field.fine_step * 10.0f; // << / >> : 10x the fine step
            const float slider_width = ImGui::GetFontSize() * 6.5f;
            const float input_width = ImGui::GetFontSize() * 4.5f;
            const char *const fmt = (decimals <= 2) ? "%.2f" : "%.3f";
            const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
            bool changed = false;

            ImGui::PushID(field.key);

            // Range slider: drag to sweep the bound range.
            ImGui::SetNextItemWidth(slider_width);
            if (ImGui::SliderFloat("##slider", &value, field.min_value, field.max_value, fmt))
            {
                changed = true;
            }

            // Direct numeric entry: type an exact value when the range is not the point. step 0 suppresses
            // InputFloat's own +/- buttons (the << < > >> cluster below is the stepper). The typed value is
            // applied live for preview but only clamped/rounded once the box is committed (see input_committed),
            // so a partial entry is not fought mid-keystroke.
            ImGui::SameLine(0.0f, inner);
            ImGui::SetNextItemWidth(input_width);
            if (ImGui::InputFloat("##input", &value, 0.0f, 0.0f, fmt))
            {
                changed = true;
            }
            const bool input_active = ImGui::IsItemActive();                  // still typing: defer the clamp
            const bool input_committed = ImGui::IsItemDeactivatedAfterEdit(); // Enter / focus loss: clamp now
            hover_tooltip("Type an exact value (Enter or click away to commit).");

            // Steppers (press-and-hold repeats): << -coarse | < -fine | > +fine | >> +coarse.
            ImGui::PushButtonRepeat(true);
            ImGui::SameLine(0.0f, inner);
            if (ImGui::Button("<<"))
            {
                value -= coarse_step;
                changed = true;
            }
            ImGui::SameLine(0.0f, inner);
            if (ImGui::ArrowButton("##finedn", ImGuiDir_Left))
            {
                value -= fine_step;
                changed = true;
            }
            ImGui::SameLine(0.0f, inner);
            if (ImGui::ArrowButton("##fineup", ImGuiDir_Right))
            {
                value += fine_step;
                changed = true;
            }
            ImGui::SameLine(0.0f, inner);
            if (ImGui::Button(">>"))
            {
                value += coarse_step;
                changed = true;
            }
            ImGui::PopButtonRepeat();

            // Per-field reset icon (before the help marker): restore this field to its default value.
            ImGui::SameLine(0.0f, inner);
            if (reload_icon_button("##reset"))
            {
                value = default_value;
                changed = true;
            }
            hover_tooltip("Reset this field to its default value.");

            // (?) help marker (tooltip mirrors the INI explanation) then the field label.
            ImGui::SameLine(0.0f, inner);
            help_marker(field.tooltip);
            ImGui::SameLine(0.0f, inner);
            ImGui::TextUnformatted(field.label);
            ImGui::PopID();

            // Snap into range + quantise on every edit EXCEPT while the input box is actively being typed. A
            // commit (input_committed) clamps the just-typed value even on a frame where it did not otherwise
            // change, and counts as a change so the clamped result republishes to the live camera.
            if ((changed && !input_active) || input_committed)
            {
                value = std::clamp(value, field.min_value, field.max_value);
                const float scale = (decimals <= 2) ? 100.0f : 1000.0f;
                value = std::round(value * scale) / scale;
                changed = true;
            }
            return changed;
        }

        /**
         * @brief Renders the "Applies to states" editor: which game states the selected preset auto-applies on.
         * @details Built-ins show their fixed binding read-only. For a user preset, a checkbox per framing state
         *          plus a combo for the (mutually exclusive) minigame rebuild its bind_state token list via
         *          set_bind_state; the resolver then auto-applies the most-specific match, so binding more states
         *          (e.g. Aiming + Crouch, or a specific minigame) makes a preset that wins over the single-state
         *          built-ins when they all apply. Default binds it to the empty-mask floor beside the built-in
         *          DEFAULT. With nothing selected the preset is unbound and only previews via Pin. A preset bound to
         *          exactly the states of another one joins its cycle group, which a note under either kind of binding
         *          points out.
         * @param store   The process-wide preset store.
         * @param edited  The selected preset.
         * @param editing The selected preset's row index (target for set_bind_state).
         * @param frame   This frame's reading of the published snapshot.
         */
        void draw_bind_editor(PresetStore &store, const CameraPreset &edited, int editing, const PanelFrame &frame)
        {
            if (!ImGui::CollapsingHeader("Applies to states", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            const std::optional<std::uint32_t> bound = Presets::parse_bind_mask(edited.bind_state);

            if (edited.builtin)
            {
                // Built-in bindings are fixed (the store re-asserts them on load); show them read-only.
                if (!bound || *bound == 0)
                {
                    ImGui::TextDisabled("Fallback: applies when no other preset matches.");
                }
                else
                {
                    ImGui::Text("Auto-applies on: %s", describe_states(*bound).c_str());
                }
                hover_tooltip("Built-in bindings are fixed. Edit the values above, or make a user preset to "
                              "bind a combination of states.");
                draw_cycle_note(frame, edited.name);
                return;
            }

            // User preset: a Default checkbox, editable checkboxes for the framing states, plus a single combo for
            // minigames (which are mutually exclusive, with an "any minigame" option). Rebuild the token list on any
            // change. Default is the empty mask, which an unbound preset has too, so it is tracked on its own: ticking
            // it clears every state, and binding any state leaves it.
            std::uint32_t mask = bound.value_or(0u);
            bool binds_default = bound.has_value() && *bound == 0;
            bool changed = false;
            if (ImGui::Checkbox("Default (when no other state applies)", &binds_default))
            {
                mask = 0;
                changed = true;
            }
            hover_tooltip("Applies wherever DEFAULT does and joins its cycle group, so the cycle keys can switch\n"
                          "your everyday camera. Binding a state below moves the preset to that state instead.");
            for (const BindOption &option : k_bind_options)
            {
                bool on = (mask & state_bit(option.bit)) != 0;
                if (ImGui::Checkbox(option.label, &on))
                {
                    if (on)
                    {
                        mask |= state_bit(option.bit);
                    }
                    else
                    {
                        mask &= ~state_bit(option.bit);
                    }
                    binds_default = false;
                    changed = true;
                }
            }

            // Minigame binding: 0 = none, 1 = any minigame, 2+ = a specific minigame (index into k_minigames + 2).
            int minigame_sel = 0;
            for (std::size_t i = 0; i < k_minigames.size(); ++i)
            {
                if ((mask & state_bit(k_minigames[i].bit)) != 0)
                {
                    minigame_sel = static_cast<int>(i) + 2;
                    break;
                }
            }
            if (minigame_sel == 0 && (mask & state_bit(GameState::Minigame)) != 0)
            {
                minigame_sel = 1;
            }
            const char *minigame_preview = (minigame_sel == 0) ? "None"
                                           : (minigame_sel == 1)
                                               ? k_any_minigame_label
                                               : k_minigames[static_cast<std::size_t>(minigame_sel - 2)].label;
            if (ImGui::BeginCombo("Minigame", minigame_preview))
            {
                const auto item = [&](int value, const char *label)
                {
                    const bool selected = (minigame_sel == value);
                    if (ImGui::Selectable(label, selected))
                    {
                        minigame_sel = value;
                        binds_default = binds_default && value == 0;
                        changed = true;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                };
                item(0, "None");
                item(1, k_any_minigame_label);
                for (std::size_t i = 0; i < k_minigames.size(); ++i)
                {
                    item(static_cast<int>(i) + 2, k_minigames[i].label);
                }
                ImGui::EndCombo();
            }

            // Re-assert the combo selection into the mask each frame so it survives a framing-checkbox toggle:
            // clear all minigame bits, then set the umbrella (plus the chosen child) so a specific-minigame bind
            // stays the 2-bit mask the resolver prefers over a generic "any minigame" bind.
            mask &= ~state_bit(GameState::Minigame);
            for (const MinigameInfo &mg : k_minigames)
            {
                mask &= ~state_bit(mg.bit);
            }
            if (minigame_sel == 1)
            {
                mask |= state_bit(GameState::Minigame);
            }
            else if (minigame_sel >= 2)
            {
                mask |= state_bit(GameState::Minigame) |
                        state_bit(k_minigames[static_cast<std::size_t>(minigame_sel - 2)].bit);
            }

            if (changed)
            {
                // The empty mask means either the DEFAULT floor or unbound. cycle_group_tokens(0) spells the floor
                // ("default"), and bind_mask_to_tokens spells an empty mask as unbound ("none").
                store.set_bind_state(editing, binds_default ? Presets::cycle_group_tokens(0)
                                                            : Presets::bind_mask_to_tokens(mask));
            }

            if (binds_default)
            {
                ImGui::TextDisabled("Auto-applies when no other preset matches, as DEFAULT does.");
            }
            else if (mask == 0)
            {
                ImGui::TextDisabled("Not bound to any state; it only previews while pinned.");
            }
            else
            {
                ImGui::TextDisabled("Auto-applies when ALL selected states are active; more states win over fewer.");
            }
            draw_cycle_note(frame, edited.name);
        }

        /**
         * @brief Draws the live "current follow distance" readout and a zoom-reset button under Follow Distance.
         * @param decimals Display precision (2 or 3), matching the field editors' precision preference.
         * @details The preset's Follow Distance is only the BASE: the frustum detour adds the accumulated
         *          zoom-key offset and clamps the sum to the Min..Max window each engaged frame, so the slider
         *          value alone does not reveal where the camera actually sits after zooming. This reads the same
         *          live atomics the detour uses (settings().follow_distance + camera_state().zoom_offset, clamped
         *          to the live Min/Max) so the number is the true rendered distance, updating in real time as the
         *          zoom keys are held. The button asks the detour to drop the zoom (zoom_reset_requested), which
         *          eases the camera back to the configured Follow Distance. It is disabled when there is no zoom to
         *          clear. A readout that is one frame stale is harmless.
         */
        void draw_applied_follow_distance(int decimals)
        {
            LiveSettings &live = settings();
            CameraState &cam = camera_state();
            const float zoom = cam.zoom_offset.load(std::memory_order_relaxed);
            const float base = live.follow_distance.load(std::memory_order_relaxed);
            const float lo = live.follow_distance_min.load(std::memory_order_relaxed);
            const float hi = live.follow_distance_max.load(std::memory_order_relaxed);
            // Guard the bound order (a user can set Min > Max): std::clamp is UB when hi < lo.
            const float applied = std::clamp(base + zoom, lo, std::max(lo, hi));

            ImGui::TextDisabled("Current follow distance: %.*f m", decimals, applied);
            hover_tooltip("The actual follow distance the camera is using right now: the Follow Distance above\n"
                          "plus the zoom-key offset, clamped to Min..Max. Watch it change as you hold the zoom keys.");

            ImGui::SameLine();
            const bool zoomed = std::abs(zoom) > 1e-3f;
            ImGui::BeginDisabled(!zoomed);
            if (ImGui::SmallButton("Reset zoom"))
            {
                cam.zoom_reset_requested.store(true, std::memory_order_relaxed);
            }
            ImGui::EndDisabled();
            hover_tooltip(
                zoomed ? "Clear the accumulated zoom so the camera returns to the configured Follow Distance above."
                       : "No zoom applied; the camera is already at the configured Follow Distance.");
        }

        /**
         * @brief Read-out: the player's live REAL first-person eye height above the feet (drops on kneel/pray/sit).
         * @details Shows the same value Dynamic Eye Sync uses - camera_state().real_eye_height, published by the
         *          frustum-builder detour each engaged frame. Reading the relaxed atomic from the overlay thread
         *          races only the render thread's own relaxed write, so a stale-by-a-frame readout is harmless.
         */
        void draw_applied_eye_height(int decimals)
        {
            CameraState &cam = camera_state();
            const float real_eye_height = cam.real_eye_height.load(std::memory_order_relaxed);
            ImGui::TextDisabled("Current eye height (real): %.*f m", decimals, real_eye_height);
            hover_tooltip("Your character's actual first-person eye height above the feet right now. It drops when\n"
                          "you kneel, pray, sit, etc. With Dynamic Eye Sync on, the camera re-anchors to this when it\n"
                          "falls well below Eye Height. Updates live as your pose changes.");

            // Live status of the dynamic sync (the ACTIVE preset's, which is the live camera): highlight when the
            // camera is currently driven by the re-anchored real eye, vs. dim when the steady Eye Height is in effect.
            if (settings().dynamic_eye_sync.load(std::memory_order_relaxed))
            {
                if (cam.eye_sync_engaged.load(std::memory_order_relaxed))
                {
                    const float effective = cam.eye_sync_effective.load(std::memory_order_relaxed);
                    ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f),
                                       "  -> Dynamic Eye Sync ACTIVE: anchored to %.*f m (real eye)", decimals,
                                       effective);
                    hover_tooltip("A low pose dropped your eye out of Eye Height range, so the camera is following\n"
                                  "your real eye (head bob included) instead of the configured Eye Height.");
                }
                else
                {
                    ImGui::TextDisabled("  -> Dynamic Eye Sync idle: using Eye Height (pose in range)");
                    hover_tooltip("Dynamic Eye Sync is on, but your pose is within range, so the steady configured\n"
                                  "Eye Height is in effect (no bob).");
                }
            }
        }

        /**
         * @brief Draws the per-field "shared" checkbox that links field @p field across every preset.
         * @details A ticked box means the field is kept identical in all presets: ticking it copies the selected
         *          preset's value into every preset and later edits to it propagate to all of them. Carries a visible
         *          "Shared" label (so it is not mistaken for an enable toggle) and leads each field row, the boxes
         *          lining up as a column. Because enabling OVERWRITES every preset's value of this field, the tick
         *          is confirmed through a modal before it applies; un-ticking only drops the link (no values change)
         *          and applies at once. The popup is scoped under this field's PushID so each row has its own
         *          confirmation.
         * @param store The process-wide preset store (owns the shared-field set).
         * @param field The field this checkbox links.
         */
        void draw_shared_checkbox(PresetStore &store, const PresetField &field)
        {
            ImGui::PushID(field.key);
            bool shared = store.is_field_shared(field.key);
            if (ImGui::Checkbox("Shared##shared", &shared))
            {
                if (shared)
                {
                    // Enabling broadcasts the selected preset's value over every preset's value of this field;
                    // confirm first so a stray tick cannot wipe the other presets' tuning. The checkbox reverts
                    // to unticked on its own next frame (it reflects the not-yet-changed store) until confirmed.
                    ImGui::OpenPopup("##shareconfirm");
                }
                else
                {
                    store.set_field_shared(field.key, false); // dropping the link changes no values; apply at once
                }
            }
            hover_tooltip("Shared: keep this value the same across ALL presets. Ticking it copies the selected\n"
                          "preset's value to every preset; editing it afterwards updates them all at once.");

            // Confirmation modal for the destructive enable.
            if (begin_confirm_modal("##shareconfirm"))
            {
                ImGui::Text("Share \"%s\" across all presets?", field.label);
                ImGui::Spacing();
                ImGui::TextWrapped("This copies the selected preset's value into every preset, overwriting their "
                                   "current values for this setting. Editing it afterwards keeps them all in sync.");
                ImGui::Spacing();
                if (ImGui::Button("Apply to all presets"))
                {
                    store.set_field_shared(field.key, true);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        /**
         * @brief Renders the grouped field editors (Framing / Orbit / Collision) for @p edited.
         * @param store  The process-wide preset store (for mark_dirty on any edit and the precision pref).
         * @param edited The preset bound to the editor (presets()[editing_index()]).
         */
        void draw_field_editors(PresetStore &store, CameraPreset &edited)
        {
            const int decimals = store.value_compact() ? 2 : 3;

            // Per-field reset target: a built-in or a factory starting point resets to its embedded factory values,
            // any other preset to the struct defaults, which equal the DEFAULT preset.
            const CameraPreset defaults = Presets::factory_preset(edited.name).value_or(CameraPreset{});

            // Fields are pre-ordered by group in fields(); open a header per group as it
            // changes, and remember whether the current group's header is expanded.
            const char *current_group = nullptr;
            bool group_open = false;

            for (const PresetField &field : Presets::fields())
            {
                const bool new_group = (current_group == nullptr) || (std::string_view(current_group) != field.group);
                if (new_group)
                {
                    current_group = field.group;
                    group_open = ImGui::CollapsingHeader(field.group, ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (!group_open)
                {
                    continue;
                }

                // The Dynamic Eye Sync toggle only does anything when Eye Height > 0 (it re-anchors the body-lifted
                // anchor; at Eye Height 0 the camera already rides the FP eye). Hide it while Eye Height is 0.
                if (std::string_view(field.key) == "dynamic_eye_sync" && edited.eye_height <= 0.0f)
                {
                    continue;
                }

                // Lead each row with the shared toggle: when ticked the field is kept identical across every
                // preset, so the editor that follows edits every preset at once.
                draw_shared_checkbox(store, field);
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

                bool changed = false;
                if (field.type == FieldType::Float && field.f != nullptr)
                {
                    changed = edit_float(field, edited.*field.f, decimals, defaults.*field.f);
                }
                else if (field.type == FieldType::Bool && field.b != nullptr)
                {
                    ImGui::PushID(field.key);
                    // Per-field reset icon (before the help marker), matching the float row.
                    if (reload_icon_button("##reset"))
                    {
                        edited.*field.b = defaults.*field.b;
                        changed = true;
                    }
                    hover_tooltip("Reset this field to its default value.");
                    ImGui::SameLine();
                    help_marker(field.tooltip);
                    ImGui::SameLine();
                    if (ImGui::Checkbox(field.label, &(edited.*field.b)))
                    {
                        changed = true;
                    }
                    ImGui::PopID();
                }

                if (changed)
                {
                    // Republish immediately so the edit previews on the live camera this frame. A shared field
                    // also pushes its new value into every other preset (broadcast_field marks dirty too).
                    if (store.is_field_shared(field.key))
                    {
                        store.broadcast_field(field.key);
                    }
                    else
                    {
                        store.mark_dirty();
                    }
                }

                // Under the Follow Distance row, surface the LIVE applied distance (base + zoom, clamped) and a
                // button to clear the zoom, since the slider value alone hides where the camera sits after zooming.
                if (field.type == FieldType::Float && std::string_view(field.key) == "follow_distance")
                {
                    draw_applied_follow_distance(decimals);
                }
                // Under Eye Height, surface the live REAL eye height so you can see when a pose (kneel/pray) drops
                // it below Eye Height - the exact case Dynamic Eye Sync corrects.
                if (field.type == FieldType::Float && std::string_view(field.key) == "eye_height")
                {
                    draw_applied_eye_height(decimals);
                }
            }
        }

        /**
         * @brief Renders the overlay-settings section (UI scale). Persisted, not a camera value.
         * @param store The process-wide preset store.
         */
        void draw_overlay_settings(PresetStore &store)
        {
            if (!ImGui::CollapsingHeader("Overlay"))
            {
                return;
            }
            // UI Scale as a dropdown of fixed steps, not a free slider.
            static constexpr float k_scales[] = {0.5f, 0.75f, 0.85f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
            static constexpr const char *k_scale_labels[] = {"0.5x", "0.75x", "0.85x", "1.0x", "1.25x",
                                                             "1.5x", "1.75x", "2.0x",  "2.5x", "3.0x"};
            constexpr int k_scale_count = static_cast<int>(sizeof(k_scales) / sizeof(k_scales[0]));
            const float current_scale = store.ui_scale();
            int scale_sel = 3; // default points at 1.0x
            for (int i = 0; i < k_scale_count; ++i)
            {
                if (current_scale == k_scales[i])
                {
                    scale_sel = i;
                    break;
                }
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (ImGui::Combo("UI Scale", &scale_sel, k_scale_labels, k_scale_count))
            {
                store.set_ui_scale(k_scales[scale_sel]);
            }
            hover_tooltip("Scales the overlay font/UI. Stored in the presets JSON (ui_scale); Save to keep it.");

            bool compact = store.value_compact();
            if (ImGui::Checkbox("Compact precision (2 decimals)", &compact))
            {
                store.set_value_compact(compact);
            }
            hover_tooltip("On: edit and store preset values at 2 decimals (cleaner files, less fiddly).\n"
                          "Off: 3 decimals for finer control. Stored in the presets JSON; Save to keep it.");
        }

    } // namespace

    /**
     * @brief Renders the preset-manager window contents inside the active ImGui frame.
     * @details Begins the "TPV Camera - Presets" window, draws the preset list, pin toggle,
     *          CRUD/save button row, the cycle groups, the state binding and the grouped field
     *          editors, then ends the window. Guards against an empty list / an out-of-range
     *          editing index so a transient store state cannot dereference an invalid preset.
     */
    void draw_ui()
    {
        PresetStore &store = PresetStore::instance();

        // Apply the persisted UI scale for this frame (stacks on the overlay's auto-DPI font size).
        ImGui::GetIO().FontGlobalScale = store.ui_scale();

        // AlwaysAutoResize makes the window hug its content every frame, so it expands to fit the widest
        // row (long preset names, the field rows) instead of cropping or leaving dead space. Value widgets
        // set an explicit width (see edit_float) so they stay readable under auto-resize.
        if (!ImGui::Begin("TPV Camera - Presets", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return;
        }

        auto &presets = store.presets();

        if (presets.empty())
        {
            ImGui::TextDisabled("No presets loaded.");
            ImGui::End();
            return;
        }

        // Read once, before any widget can publish: the sections below then show one consistent snapshot, and
        // a change made this frame shows from the next one.
        const PanelFrame frame = read_panel_frame(store);

        draw_preset_list(store, frame);

        // Clamp the editing index defensively: a removal elsewhere could have shrunk the
        // list since the row loop captured it. set_editing_index clamps in-store, but the
        // value used for the editor below must also be re-read and bounded here.
        int editing = store.editing_index();
        if (editing < 0 || editing >= static_cast<int>(presets.size()))
        {
            editing = 0;
            store.set_editing_index(editing);
        }

        ImGui::Spacing();
        draw_pin_toggle(store);

        ImGui::Spacing();
        // draw_button_row may add / duplicate / remove / reset a preset, which can reallocate the presets
        // vector and move the selection. Pass the current selection by index, then re-resolve the editing
        // index and bind the editor reference AFTER it, so the editors below never dereference a reference
        // invalidated by that mutation.
        draw_button_row(store, presets[static_cast<std::size_t>(editing)]);

        editing = store.editing_index();
        if (editing < 0 || editing >= static_cast<int>(presets.size()))
        {
            editing = 0;
            store.set_editing_index(editing);
        }

        CameraPreset &edited = presets[static_cast<std::size_t>(editing)];

        ImGui::Separator();
        draw_cycle_groups(frame);

        ImGui::Separator();
        draw_bind_editor(store, edited, editing, frame);

        ImGui::Separator();
        draw_field_editors(store, edited);

        ImGui::Separator();
        draw_overlay_settings(store);

        ImGui::End();
    }

} // namespace TPVCamera::Overlay
