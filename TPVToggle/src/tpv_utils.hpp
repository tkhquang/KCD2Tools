#ifndef TPV_UTILS_HPP
#define TPV_UTILS_HPP

#include <cstdint>
#include <string>
#include <DirectXMath.h>
#include <DetourModKit/logger.hpp> // For DetourModKit::Logger

namespace TpvUtils
{
    // RVAs
    namespace Rvas
    {
        extern const uintptr_t GetActualComponentFromManager_RVA;
    }

    // Offsets
    namespace Offsets
    {
        const ptrdiff_t CPlayer_To_CEntity = 0x38;
        const ptrdiff_t CCharInstance_To_CDefaultSkeleton = 0xA70;
    }

    // Matrix placeholder
    struct Matrix34_placeholder
    {
        float m[12];
    };

    // Typedefs
    typedef uintptr_t(__fastcall *CEntity_VTableFunc74_Type)(uintptr_t pEntity, int zero_arg);
    typedef uintptr_t(__fastcall *GetActualComponentFromManager_Type)(uintptr_t pProxyManagerAdjusted, unsigned int proxyId);
    typedef int(__fastcall *GetBoneIndexByName_Type)(uintptr_t pDefaultSkeleton, const char *boneName);
    typedef uintptr_t(__fastcall *GetBoneWorldTransform_Type)(uintptr_t pDefaultSkeleton, short boneIndex);

    // SafeRead is removed, will use DMK::Memory::isMemoryReadable directly.

    bool InitializeAndLogPlayerAnimationPointers(int maxRetries, int retryDelayMs);
    bool GetHeadBoneWorldPosition(DirectX::XMFLOAT3 &outPosition);

} // namespace TpvUtils

#endif // TPV_UTILS_HPP
