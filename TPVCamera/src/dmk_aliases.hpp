/**
 * @file dmk_aliases.hpp
 * @brief Short local names for the DetourModKit memory vocabulary the mod uses on every guarded read.
 *
 * DetourModKit spells a location as Address, a range as Region, and every fallible read as Result<T>.
 * The mod's own arithmetic stays in raw uintptr_t, because its offsets come from constants.hpp and the
 * RTTI self-heal and are added to walked game pointers. Address therefore appears only at the library
 * boundary, wrapping the argument at the call.
 *
 * These aliases sit inside namespace TPVCamera, so every mod translation unit sees them and the global
 * namespace stays clean.
 */
#ifndef TPVCAMERA_DMK_ALIASES_HPP
#define TPVCAMERA_DMK_ALIASES_HPP

#include <DetourModKit.hpp>

namespace TPVCamera
{
    /// The guarded-memory module: read / read_into / walk / write_in_place / is_plausible_ptr.
    namespace mem = DMK::memory;

    using DMK::Address;
    using DMK::Region;
} // namespace TPVCamera

#endif // TPVCAMERA_DMK_ALIASES_HPP
