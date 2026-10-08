## Third-Person Archery and a Steadier Camera

- In third person, arrows, bolts and handgonne shots now fly at the crosshair and drop straight below it, like a first-person shot.
- Distraction stones thrown in third person now land straight below the crosshair too.
- New optional aim preview shows where a drawn shot or a held distraction stone will land.
- New optional arrow trail shows each arrow's flight and where it landed.
- New options set how much bow, crossbow and handgonne shots drop.
- Aiming a bow or crossbow no longer switches to first person by default.
- The aiming camera now waits until a crossbow is loaded before it zooms in.
- When aiming, throwing or drawing a weapon turns free-look off, Henry now turns to face the camera instead of the camera swinging back behind him.
- Turning free-look on, or getting it back after a menu or an action, no longer moves the camera.
- Starting to walk in free-look now heads the right way from the first step, and a quick tap no longer swings the camera.
- Running right after aiming or crouching steers with the camera again.
- The camera no longer jiggles when you swing it up and down next to a market stall, a roof edge or a counter.
- When the camera has no room behind Henry (a wall right behind, a low doorway), Henry fades out instead of the view jumping to first person. He never disappears completely: a quarter of him stays by default (CloseUpFadeMinOpacity). His shadow stays. CloseUpFade turns this off.
- Everything Henry wears and holds fades out with him: the weapons in his hands and on his belt and back, a drawn bow and the arrow on it, a crossbow, the quiver and the shield.
- His eyes, eyelashes, hair, beard and the thin wet film over his eyes fade out with him too, so his head stays shown instead of being switched off. The game prepares the shaders they fade with in the background while you play in third person, once after installing or updating (up to about a minute of play), so the first close-up normally finds them ready; a close-up before that hides his head instead.
