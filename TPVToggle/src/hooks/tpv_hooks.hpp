#ifndef TPV_HOOKS_HPP
#define TPV_HOOKS_HPP

#include <cstdint>
// ... (Matrix34f struct if needed, or use DirectXMath directly for SViewParams-like structure)

bool initializeTpvHooks(uintptr_t moduleBase, size_t moduleSize);
void cleanupTpvHooks();

// CameraFirstPerson::UpdateView Hook specifics (FUN_180b96198)
// RVA for FUN_180b96198
const uintptr_t RVA_CameraFirstPerson_UpdateView = 0xB96198;

// SViewParams or similar structure that param_2 points to.
// We need to know its layout for Position (Vec3) and Orientation (Matrix33 or Quat).
// Based on your previous findings for FUN_180413e98, the SViewParams-like struct had:
// - Orientation Matrix (3x3, column-major) at offset +0xE8 from some base.
// - Position (Vec3) at offset +0x10C from some base.
// param_2 of FUN_180b96198 IS LIKELY this "base" pointer.

// Typedef for the original C_CameraFirstPerson::UpdateView function
// param_1 is C_CameraFirstPerson* this_ptr
// param_2 is SViewParams* pSViewParams_out (or whatever the game calls it)
typedef void(__fastcall *CameraFirstPerson_UpdateView_Func)(uintptr_t pThisCamera, uintptr_t pSViewParams_out);
extern CameraFirstPerson_UpdateView_Func fpCameraFirstPerson_UpdateView_Original; // Trampoline

// Detour function
void __fastcall Detour_CameraFirstPerson_UpdateView(uintptr_t pThisCamera, uintptr_t pSViewParams_out);

#endif // TPV_HOOKS_HPP
