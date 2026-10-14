## Fading Obstacles, See-Through Trees and Camera Rules

- Added the close-up fade: with no room behind Henry, he fades out with his gear, eyes, hair and beard instead of the view jumping to first person (CloseUpFade, CloseUpFadeMinOpacity).
- Added the occluder fade: posts, fences, open carts, lamps, furniture and hanging decorations between the camera and Henry fade out instead of pulling the camera in (UseOccluderFade, OccluderFadeMinOpacity).
- Added the tree see-through: trees, bushes and tall grass in front of Henry open a dithered window around him, from his hips up (UseTreeSeeThrough and the TreeSeeThrough options).
- Added camera rules: choose by folder or by model which objects stop the camera, fade, open in the window or are left alone, other mods' models included, in KCD2_TPVCamera_rules_User.txt or a mod's own rules file, applied while the game runs.
- Added ExportRules to write the built-in camera rules to KCD2_TPVCamera_rules.default.txt for reference.
- Changed the camera to keep its distance and pass through bushes and tree trunks, while buildings, walls, tall fences, platforms, covered wagons, big haystacks, rocks and the ground still stop it.
- Changed leaves and prop corners right at the lens to be cut away instead of filling the view.
- Changed the shaders behind the fades and the window to be prepared at loading screens and pauses, never while you play.
- Changed the INFO log to be quieter; diagnostics moved to DEBUG and TRACE.
- Removed UseCoverageCollision, CoverageThreshold, CameraProbeSize and UseRenderOcclusion, which the fade replaces.
- Fixed the camera jiggling when swung up and down next to a market stall, a roof edge or a counter.
- Fixed the camera stopping at the small invisible collision boxes around buildings and props.
- Fixed closed animated doors letting the camera through and hiding Henry behind them.
