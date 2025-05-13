#pragma once
/**
 * @file game_structures.h
 * @brief Game-specific structure definitions for Kingdom Come: Deliverance II
 *
 * Contains minimal definitions of game structures required for hooking and memory access.
 * These structures are reverse-engineered approximations and may not match the exact
 * internal game structures, but provide sufficient compatibility for our purposes.
 */

#include <cstdint>
#include "math_utils.h"

namespace GameStructures
{

    /**
     * @brief 3x4 Matrix used by the game engine (CryEngine)
     * @details Stores transformation matrix in row-major format
     *          Rows 0-2: Rotation/Scale, Column 3: Translation
     */
    struct Matrix34f
    {
        float m[3][4]; // Stores matrix row by row. m[0] is row0, etc.

        /**
         * @brief Sets the matrix from a quaternion and position
         * @param q_orientation Quaternion representing rotation
         * @param v_position Vector3 representing translation
         * @details Assumes CryEngine default: Y-Forward, Z-Up, X-Right for entity's local axes
         */
        void Set(const Quaternion &q_orientation, const Vector3 &v_position)
        {
            // Assumes CryEngine default: Y-Forward, Z-Up, X-Right for entity's local axes
            // Matrix rows store these basis vectors in world space.
            Vector3 R = q_orientation.Rotate(Vector3(1.0f, 0.0f, 0.0f)); // Local X axis (Right)
            Vector3 F = q_orientation.Rotate(Vector3(0.0f, 1.0f, 0.0f)); // Local Y axis (Forward)
            Vector3 U = q_orientation.Rotate(Vector3(0.0f, 0.0f, 1.0f)); // Local Z axis (Up)

            // Row 0: Right vector components + Position X
            m[0][0] = R.x;
            m[0][1] = R.y;
            m[0][2] = R.z;
            m[0][3] = v_position.x;
            // Row 1: Forward vector components + Position Y
            m[1][0] = F.x;
            m[1][1] = F.y;
            m[1][2] = F.z;
            m[1][3] = v_position.y;
            // Row 2: Up vector components + Position Z
            m[2][0] = U.x;
            m[2][1] = U.y;
            m[2][2] = U.z;
            m[2][3] = v_position.z;
        }

        /**
         * @brief Gets the raw float pointer to the matrix data
         * @return Pointer to the first element of the matrix
         */
        float *AsFloatPtr() { return &m[0][0]; }
    };

    /**
     * @brief Base entity class (simplified approximation)
     * @details Virtual function table must match game's entity base class
     *          for proper hooking. Order and signatures are critical.
     */
    class CEntity
    {
    public:
        // Virtual function table - order must match game's implementation
        virtual ~CEntity() = default;
        virtual uint32_t GetId() { return 0; }
        virtual void UnknownVFunc1() {}
        virtual class EntityClass *GetClass() { return nullptr; }
        virtual void UnknownVFunc3() {}
        virtual void UnknownVFunc4() {}
        virtual void UnknownVFunc5() {}
        virtual void UnknownVFunc6() {}
        virtual void UnknownVFunc7() {}
        virtual void UnknownVFunc8() {}
        virtual void UnknownVFunc9() {}
        virtual void UnknownVFunc10() {}
        virtual void UnknownVFunc11() {}
        virtual void UnknownVFunc12() {}
        virtual void UnknownVFunc13() {}
        virtual void UnknownVFunc14() {}
        virtual void UnknownVFunc15() {}
        virtual void UnknownVFunc16() {}
        virtual const char *GetName() { return ""; } // VTable index 18

        // Member data layout - padding to match game's memory layout
        char padding_to_matrix[0x50]; // 0x58 (target) - 0x8 (vtable) = 0x50
        Matrix34f m_worldTransform;   // World transformation matrix at offset 0x58
    };

    /**
     * @brief Third-person camera data structure (placeholder)
     * @details Used for passing camera state between functions
     */
    struct TPVCameraData
    {
        char padding[0x10];     // Unknown data
        Quaternion orientation; // Camera orientation quaternion
        // Additional camera-specific data would go here
    };

    /**
     * @brief Input event structure for mouse/keyboard events
     * @details Used by the event handler and input hooks
     */
    struct InputEvent
    {
        uint8_t eventByte0; // 0x00: Event type identifier (0x01 for some events)
        char padding1[3];   // 0x01-0x03: Alignment padding
        int32_t eventType;  // 0x04: Event type (e.g., 0x08 for mouse events)
        char padding2[8];   // 0x08-0x0F: Unknown data
        int32_t eventId;    // 0x10: Specific event ID
        char padding3[4];   // 0x14-0x17: Alignment padding
        float deltaValue;   // 0x18: Delta value (movement amount)
        // Additional event data may follow
    };

    /**
     * @brief Player state structure containing position and rotation
     * @details Used for copying player state between systems
     */
    struct PlayerState
    {
        Vector3 position;    // 0x00: World position
        Quaternion rotation; // 0x0C: World rotation
        // Additional state data up to 0xD4 bytes total
        char padding[0xC4]; // Padding to match full size
    };

} // namespace GameStructures
