"""KCD2 Animation Framework Bridge: from a Blender action to a clip playing in Kingdom Come: Deliverance II.

Works on the armatures Bohemian Animator builds from the game's skeletons. It samples the action at the game's 30
frames a second (any scene frame rate), compiles it with animfw.exe, the Animation Framework's command line, without
the Resource Compiler and keeping every bone a full-body clip needs, checks it against the game's own rules, and either
drops it into the framework's DEV folder, where "Play in game" reloads it and plays it on Henry in a second, or into a
mod folder that "Make action" and "Pack mod" turn into an installable mod.
"""

bl_info = {
    "name": "KCD2 Animation Framework Bridge",
    "author": "tkhquang",
    "version": (1, 0, 0),
    "blender": (4, 0, 0),
    "location": "View3D > Sidebar > AnimFW",
    "description": "Compile, check and play KCD2 animations in the running game through the Animation Framework",
    "category": "Animation",
}

import os
import re
import tempfile
import time

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty

from . import export, live, tool

REPORT_TEXT = "AnimFW Report"
MOD_ID = re.compile(r"[a-z_]+")
CLIP_NAME = re.compile(r"[a-z0-9_]+")
FRAGMENT_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*")
WORK_PREFIX = "animfw_bridge_"


def preferences(context):
    return context.preferences.addons[__package__].preferences


def show_report(lines):
    text = bpy.data.texts.get(REPORT_TEXT) or bpy.data.texts.new(REPORT_TEXT)
    text.clear()
    text.write("\n".join(lines) + "\n")


def find_armature(context):
    obj = context.active_object
    if obj is not None and obj.type == "ARMATURE":
        return obj
    return next((o for o in context.selected_objects if o.type == "ARMATURE"), None)


def action_names_valid(operator, settings):
    """Whether the action's name and the game fragment it is like are fragment names; reports why not."""
    if FRAGMENT_NAME.fullmatch(settings.fragment) and FRAGMENT_NAME.fullmatch(settings.like):
        return True
    operator.report({"ERROR"}, "Set the action's name and the game fragment it is like")
    return False


class AnimFWPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    game_folder: StringProperty(
        name="Game folder", subtype="DIR_PATH",
        description="Kingdom Come: Deliverance II's folder (holding Data); empty: found in Steam")
    tool_path: StringProperty(
        name="animfw.exe", subtype="FILE_PATH",
        description="The Animation Framework's command line; empty: the one shipped with the add-on, "
                    "else the one next to the framework in the game's Bin folder")

    def draw(self, context):
        self.layout.prop(self, "game_folder")
        self.layout.prop(self, "tool_path")
        found = tool.game_folder(self)
        self.layout.label(text=f"Game: {found or 'not found'}")


class AnimFWSettings(bpy.types.PropertyGroup):
    mod_id: StringProperty(
        name="Mod id", default="my_mod", description="Your mod's id: lowercase letters and underscores")
    clip_name: StringProperty(
        name="Clip", default="my_mod_clip",
        description="The clip's name (lowercase, digits, underscores), best starting with the mod id")
    frames: EnumProperty(name="Frames", items=(("ACTION", "Action", "The action's keyed range"),
                                               ("SCENE", "Scene", "The scene's frame range")), default="ACTION")
    kind: EnumProperty(name="Kind", items=(
        ("FULL", "Full body", "Every bone the armature has: for clips that replace what Henry does"),
        ("OVERLAY", "Overlay",
         "Leaves out bones that never leave their default pose, as rc.exe does: for layers over another clip"),
        ("ADDITIVE", "Additive", "Each frame's change from the first, added on top of the clip below")),
        default="FULL")
    loop: EnumProperty(name="Loop", items=(("AUTO", "Detect", "Loops when every bone ends where it starts"),
                                           ("ON", "Loops", ""), ("OFF", "Plays once", "")), default="AUTO")
    compression: IntProperty(
        name="Compression", default=10, min=0, max=100,
        description="rc.exe's compression value: 10 drops keys the others give within 0.32 mm and 0.036 degrees; "
                    "0 drops exact ones only")
    destination: EnumProperty(name="To", items=(
        ("DEV", "DEV folder", "The framework's DEV folder, for testing in the running game"),
        ("MOD", "Mod folder", "Your mod's source folder, for release")), default="DEV")
    mod_folder: StringProperty(name="Mod folder", subtype="DIR_PATH",
                               description="Your mod's source folder (mod.manifest, Data\\...)")
    install_folder: StringProperty(
        name="Install to", subtype="DIR_PATH",
        description="Where Pack writes the installed mod, such as MO2's mods\\My Mod\\my_mod")
    fragment: StringProperty(name="Action", default="MyMod_Action",
                             description="The new fragment that plays the clip, as scripts call it")
    like: StringProperty(
        name="Like", default="ADLG_Wave",
        description="A game fragment the action is set up like (its body layers, blend and procedural layers)")
    upper: BoolProperty(name="Upper body", default=False,
                        description="Plays over what Henry is doing, on the upper body only (gestures)")
    search: StringProperty(name="Search", default="", description="Text to find in the game's fragment names")
    mocap_root: EnumProperty(name="Root", items=(
        ("path", "Path", "The root follows the pelvis's smoothed path (a walk moves Henry)"),
        ("line", "Line", "The root moves in a straight line at one steady speed (looping locomotion)"),
        ("in-place", "In place", "Neither the root nor the pelvis travels (an in-place cycle)")), default="path")
    mocap_turns: BoolProperty(name="Root turns", default=False,
                              description="The root turns with the body's facing (turning clips)")
    mocap_feet: BoolProperty(
        name="Lock feet", default=False,
        description="Hold planted heels and balls of the feet in place with leg IK, for sources whose feet skate "
                    "(not in place)")
    mocap_from: FloatProperty(
        name="From", default=0.0, min=0.0, unit="TIME_ABSOLUTE",
        description="Where to start in the source, in seconds (mocap takes often start with a T-pose)")
    mocap_to: FloatProperty(name="To", default=0.0, min=0.0, unit="TIME_ABSOLUTE",
                            description="Where to end in the source, in seconds; 0: its end")
    mocap_calibrate: IntProperty(
        name="Reference frame", default=-1, min=-1,
        description="A source frame where it stands neutral, as the reference pose; -1: its rest pose")
    mocap_take: StringProperty(name="Take", default="",
                               description="The take to read from an FBX that holds several; empty: the first")


def clip_target(settings, prefs):
    relative = os.path.join("Animations", "humans", "male", settings.mod_id, settings.clip_name + ".caf")
    if settings.destination == "MOD":
        return os.path.join(tool.bpy_abspath(settings.mod_folder), "Data", relative)
    return os.path.join(tool.dev_folder(prefs), relative)


def compile_options(settings):
    options = []
    if settings.kind == "OVERLAY":
        options.append("--overlay")
    elif settings.kind == "ADDITIVE":
        options.append("--additive")
    if settings.loop == "ON":
        options.append("--loop")
    elif settings.loop == "OFF":
        options.append("--no-loop")
    return options + ["--compression", settings.compression]


def export_clip(operator, context):
    """Samples, writes and compiles the clip; returns the .caf path, or None after reporting why not."""
    settings = context.scene.animfw
    prefs = preferences(context)
    armature = find_armature(context)
    if armature is None:
        operator.report({"ERROR"}, "Select the KCD2 armature")
        return None
    if not MOD_ID.fullmatch(settings.mod_id):
        operator.report({"ERROR"},
                        "The mod id takes lowercase letters and underscores only (the game skips the mod otherwise)")
        return None
    if not CLIP_NAME.fullmatch(settings.clip_name):
        operator.report({"ERROR"}, "The clip name takes lowercase letters, digits and underscores")
        return None
    if settings.destination == "MOD":
        if not settings.mod_folder:
            operator.report({"ERROR"}, "Set the mod folder")
            return None
        folder_id = tool.mod_id_of(tool.bpy_abspath(settings.mod_folder))
        if folder_id != settings.mod_id:
            operator.report({"ERROR"}, f"The mod folder's mod id is {folder_id} (its mod.manifest or its name): "
                                       f"set the mod id to {folder_id}, or pick that mod's folder")
            return None
    if settings.destination == "DEV" and not tool.dev_folder(prefs):
        operator.report({"ERROR"}, "The game was not found: set its folder in the add-on's preferences")
        return None
    first, last = export.frame_range(context, armature, settings.frames)
    try:
        names, tracks = export.sample(context, armature, first, last)
    except ValueError as error:
        operator.report({"ERROR"}, str(error))
        return None
    target = clip_target(settings, prefs)
    # The intermediate file only feeds the compile, so it goes with its folder.
    with tempfile.TemporaryDirectory(prefix=WORK_PREFIX, ignore_cleanup_errors=True) as work:
        intermediate = os.path.join(work, settings.clip_name + ".i_caf")
        export.write_intermediate(intermediate, names, tracks)
        code, lines = tool.run(prefs, "compile", intermediate, target, *compile_options(settings))
    show_report(lines)
    if code == 1:
        operator.report({"ERROR"}, (lines[-1] if lines else "the compile failed") + f" (see the {REPORT_TEXT} text)")
        return None
    warnings = [line.strip() for line in lines if "WARNING" in line]
    summary = lines[0].split(": ", 1)[-1] if lines else "compiled"
    operator.report({"WARNING"} if warnings else {"INFO"},
                    f"{settings.clip_name}: {summary}" + (f"; {warnings[0]}" if warnings else ""))
    return target


class ANIMFW_OT_export(bpy.types.Operator):
    bl_idname = "animfw.export_clip"
    bl_label = "Export clip"
    bl_description = "Compile the action into a game clip (no rc.exe) and check it"

    def execute(self, context):
        return {"FINISHED"} if export_clip(self, context) else {"CANCELLED"}


class ANIMFW_OT_play(bpy.types.Operator):
    bl_idname = "animfw.play_in_game"
    bl_label = "Play in game"
    bl_description = ("Export into the DEV folder, reload the game's clips, make the action and play it on Henry "
                      "(needs the game in a world with the Animation Framework)")

    # Each step sends a request to the game, then waits for the framework's log line that confirms it.
    # The operator is modal and polls the log on a timer, so Blender stays responsive while the game works.
    POLL_SECONDS = 0.1
    STEP_TIMEOUT = 30.0

    def execute(self, context):
        settings = context.scene.animfw
        if not action_names_valid(self, settings):
            return {"CANCELLED"}
        destination = settings.destination
        settings.destination = "DEV"
        try:
            if export_clip(self, context) is None:
                return {"CANCELLED"}
        finally:
            settings.destination = destination
        action = (settings.mod_id, settings.fragment, settings.clip_name, settings.like, settings.loop == "ON",
                  settings.upper)
        # The database line is the last patch of the reload that matters for Henry. The game finishes the whole
        # mn_reload inside one console update, so the play sent after that line runs on a later frame.
        self._steps = [
            (live.reload_clips, "AnimClips: clip reload:", "Reloading the game's clips"),
            (lambda: live.make_action(*action), f"kcd_male_database__{settings.mod_id}.adb", "Making the action"),
        ]
        fragment, upper = settings.fragment, settings.upper
        self._play = lambda: live.play(fragment, upper)
        self._fragment = settings.fragment
        self._watch = live.LogWatch(tool.framework_log(preferences(context)))
        self._step = -1
        if not self.start_next_step(context):
            return {"CANCELLED"}
        self._timer = context.window_manager.event_timer_add(self.POLL_SECONDS, window=context.window)
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}

    def start_next_step(self, context):
        """Sends the next step's command; False after reporting why the game refused it."""
        self._step += 1
        send, _, what = self._steps[self._step]
        context.workspace.status_text_set(f"AnimFW: {what}...")
        ok, reply = send()
        if not ok:
            self.report({"ERROR"}, reply)
            context.workspace.status_text_set(None)
            return False
        self._deadline = time.monotonic() + self.STEP_TIMEOUT
        return True

    def finish(self, context, result):
        context.window_manager.event_timer_remove(self._timer)
        context.workspace.status_text_set(None)
        return result

    def modal(self, context, event):
        if event.type != "TIMER":
            return {"PASS_THROUGH"}
        _, needle, what = self._steps[self._step]
        if self._watch.poll(needle) is None:
            if time.monotonic() < self._deadline:
                return {"PASS_THROUGH"}
            self.report({"ERROR"}, f"{what}: the game did not confirm it (see {tool.LOG_NAME})")
            return self.finish(context, {"CANCELLED"})
        if self._step + 1 < len(self._steps):
            return {"PASS_THROUGH"} if self.start_next_step(context) else self.finish(context, {"CANCELLED"})
        ok, reply = self._play()
        if not ok:
            self.report({"ERROR"}, reply)
            return self.finish(context, {"CANCELLED"})
        self.report({"INFO"}, f"Playing {self._fragment} on Henry")
        return self.finish(context, {"FINISHED"})


class ANIMFW_OT_stop(bpy.types.Operator):
    bl_idname = "animfw.stop_in_game"
    bl_label = "Stop"
    bl_description = "Stop the action on Henry"

    def execute(self, context):
        ok, reply = live.stop()
        self.report({"INFO"} if ok else {"ERROR"}, "Stopped" if ok else reply)
        return {"FINISHED"} if ok else {"CANCELLED"}


class ANIMFW_OT_fragments(bpy.types.Operator):
    bl_idname = "animfw.find_fragments"
    bl_label = "Find game fragments"
    bl_description = "List the game's fragments whose name holds the search text (in the report text)"

    def execute(self, context):
        code, lines = tool.run(preferences(context), "fragments", context.scene.animfw.search)
        show_report(lines)
        self.report({"INFO"} if code == 0 else {"ERROR"},
                    (lines[-1] if lines else "") + f" (see the {REPORT_TEXT} text)")
        return {"FINISHED"} if code == 0 else {"CANCELLED"}


class ANIMFW_OT_make(bpy.types.Operator):
    bl_idname = "animfw.make_action"
    bl_label = "Make action"
    bl_description = "Export the clip into the mod folder and add the action that plays it to the mod"

    def execute(self, context):
        settings = context.scene.animfw
        if not settings.mod_folder:
            self.report({"ERROR"}, "Set the mod folder")
            return {"CANCELLED"}
        if not action_names_valid(self, settings):
            return {"CANCELLED"}
        destination = settings.destination
        settings.destination = "MOD"
        try:
            clip = export_clip(self, context)
        finally:
            settings.destination = destination
        if clip is None:
            return {"CANCELLED"}
        arguments = ["make", tool.bpy_abspath(settings.mod_folder), settings.fragment, clip, settings.like]
        arguments += (["--loop"] if settings.loop == "ON" else []) + (["--upper"] if settings.upper else [])
        code, lines = tool.run(preferences(context), *arguments)
        show_report(lines)
        summary = next((line for line in lines if " plays " in line), lines[-1] if lines else "")
        self.report({"INFO"} if code == 0 else {"ERROR"}, summary + f" (see the {REPORT_TEXT} text)")
        return {"FINISHED"} if code == 0 else {"CANCELLED"}


class ANIMFW_OT_retarget(bpy.types.Operator):
    bl_idname = "animfw.retarget_mocap"
    bl_label = "Retarget mocap"
    bl_description = ("Put an animation made on another skeleton (.bvh or .fbx: motion capture, Mixamo, SMPL, "
                      "Unreal) on Henry's and key it onto the selected armature as a new action, through Bohemian "
                      "Animator's import")

    filepath: StringProperty(subtype="FILE_PATH")
    filter_glob: StringProperty(default="*.bvh;*.fbx", options={"HIDDEN"})

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        settings = context.scene.animfw
        armature = find_armature(context)
        if armature is None:
            self.report({"ERROR"}, "Select the KCD2 armature to key")
            return {"CANCELLED"}
        # bpy.ops answers every attribute, so the registered operator type is what tells whether the add-on is there.
        if not hasattr(bpy.types, "BOHEMIAN_OT_import_anim"):
            self.report({"ERROR"}, "Bohemian Animator is needed to key the clip onto the armature")
            return {"CANCELLED"}
        source = tool.bpy_abspath(self.filepath)
        stem = re.sub(r"[^a-z0-9_]+", "_", os.path.splitext(os.path.basename(source))[0].lower()).strip("_") or "mocap"
        arguments = ["--root", settings.mocap_root, "--compression", "0"]
        arguments += ["--root-turns"] if settings.mocap_turns else []
        arguments += ["--feet", "lock"] if settings.mocap_feet else []
        arguments += ["--from", f"{settings.mocap_from:.4f}"] if settings.mocap_from > 0.0 else []
        arguments += ["--to", f"{settings.mocap_to:.4f}"] if settings.mocap_to > 0.0 else []
        arguments += ["--calibrate", settings.mocap_calibrate] if settings.mocap_calibrate >= 0 else []
        arguments += ["--take", settings.mocap_take] if settings.mocap_take else []
        # The retargeted clip only feeds the import, so it goes with its folder.
        with tempfile.TemporaryDirectory(prefix=WORK_PREFIX, ignore_cleanup_errors=True) as work:
            target = os.path.join(work, stem + ".caf")
            code, lines = tool.run(preferences(context), "retarget", source, target, *arguments)
            show_report(lines)
            if code == 1:
                self.report({"ERROR"},
                            (lines[0] if lines else "the retarget failed") + f" (see the {REPORT_TEXT} text)")
                return {"CANCELLED"}
            try:
                result = bpy.ops.bohemian.import_anim(filepath=target)
            except RuntimeError as error:
                self.report({"ERROR"}, f"Bohemian Animator could not key the retargeted clip: {error}")
                return {"CANCELLED"}
        if "FINISHED" not in result:
            self.report({"ERROR"}, "Bohemian Animator could not key the retargeted clip")
            return {"CANCELLED"}
        action = armature.animation_data.action if armature.animation_data else None
        if action is not None:
            action.name = stem
            first, last = action.frame_range
            context.scene.frame_start, context.scene.frame_end = int(first), int(last)
        feet = next((line.strip() for line in lines if line.strip().startswith("feet:")), "")
        self.report({"INFO"}, f"{stem}: retargeted onto {armature.name}; {feet} (see the {REPORT_TEXT} text)")
        return {"FINISHED"}


class ANIMFW_OT_pack(bpy.types.Operator):
    bl_idname = "animfw.pack_mod"
    bl_label = "Pack mod"
    bl_description = "Write the mod as installed (mod.manifest and Data\\<modid>.pak) into the install folder"

    def execute(self, context):
        settings = context.scene.animfw
        if not settings.mod_folder or not settings.install_folder:
            self.report({"ERROR"}, "Set the mod folder and the install folder")
            return {"CANCELLED"}
        code, lines = tool.run(preferences(context), "pack", tool.bpy_abspath(settings.mod_folder),
                               tool.bpy_abspath(settings.install_folder))
        show_report(lines)
        self.report({"INFO"} if code == 0 else {"ERROR"},
                    (lines[-1] if lines else "") + f" (see the {REPORT_TEXT} text)")
        return {"FINISHED"} if code == 0 else {"CANCELLED"}


class ANIMFW_PT_panel(bpy.types.Panel):
    bl_label = "KCD2 Animation"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "AnimFW"

    def draw(self, context):
        settings = context.scene.animfw
        layout = self.layout
        armature = find_armature(context)
        if armature is None:
            layout.label(text="Select a KCD2 armature", icon="ERROR")
        else:
            layout.label(text=f"{armature.name}: {len(export.game_bones(armature))} game bones", icon="ARMATURE_DATA")

        box = layout.box()
        box.label(text="Mocap", icon="ARMATURE_DATA")
        box.prop(settings, "mocap_root")
        box.prop(settings, "mocap_turns")
        box.prop(settings, "mocap_feet")
        row = box.row(align=True)
        row.prop(settings, "mocap_from")
        row.prop(settings, "mocap_to")
        box.prop(settings, "mocap_calibrate")
        box.prop(settings, "mocap_take")
        box.operator("animfw.retarget_mocap", icon="IMPORT")

        box = layout.box()
        box.label(text="Clip", icon="ACTION")
        box.prop(settings, "mod_id")
        box.prop(settings, "clip_name")
        box.prop(settings, "frames")
        box.prop(settings, "kind")
        box.prop(settings, "loop")
        box.prop(settings, "compression")
        box.prop(settings, "destination")
        if settings.destination == "MOD":
            box.prop(settings, "mod_folder")
        box.operator("animfw.export_clip", icon="EXPORT")

        box = layout.box()
        box.label(text="Action", icon="PLAY")
        box.prop(settings, "fragment")
        box.prop(settings, "like")
        row = box.row(align=True)
        row.prop(settings, "search", text="")
        row.operator("animfw.find_fragments", text="", icon="VIEWZOOM")
        box.prop(settings, "upper")
        row = box.row(align=True)
        row.operator("animfw.play_in_game", icon="PLAY")
        row.operator("animfw.stop_in_game", text="", icon="SNAP_FACE")

        box = layout.box()
        box.label(text="Release", icon="PACKAGE")
        box.prop(settings, "mod_folder")
        box.prop(settings, "install_folder")
        row = box.row(align=True)
        row.operator("animfw.make_action")
        row.operator("animfw.pack_mod")


CLASSES = (AnimFWPreferences, AnimFWSettings, ANIMFW_OT_export, ANIMFW_OT_play, ANIMFW_OT_stop, ANIMFW_OT_fragments,
           ANIMFW_OT_retarget, ANIMFW_OT_make, ANIMFW_OT_pack, ANIMFW_PT_panel)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.animfw = PointerProperty(type=AnimFWSettings)


def unregister():
    del bpy.types.Scene.animfw
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
