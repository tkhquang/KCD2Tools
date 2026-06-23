## [Title for next release]

- Fixed the third-person camera shaking or drifting while the game plays an animation such as opening a door, looting, or fighting
- Added two options to keep the view steady (Stable Aim Basis and Aim Basis Smoothing), both on by default and adjustable in the INI
- Strengthened the camera's ability to keep working after game updates
- Camera collision now sees through thin posts, rails and fences by default, pulling in only when something actually hides you
- Added a head-priority rule that keeps the camera back as long as your head stays visible
- Collision now follows your real size and pose, so crouching, lying down, or riding no longer over-tightens the view
- Made the camera smoother when moving through tight or cluttered areas
- The cloth-roof clamp now also ignores thin props you can clearly see past
