#ifndef TRANSITION_MANAGER_H
#define TRANSITION_MANAGER_H

#include "math_utils.h"
#include <chrono>

// Camera state for transitions
struct CameraState
{
    Vector3 position;
    Quaternion rotation;

    CameraState() : position(0, 0, 0), rotation(0, 0, 0, 1) {}
    CameraState(const Vector3 &pos, const Quaternion &rot) : position(pos), rotation(rot) {}
};

/**
 * @class TransitionManager
 * @brief Manages smooth transitions between camera profiles
 *
 * This singleton class handles the interpolation between different camera positions and
 * rotations when switching profiles, providing smooth animations rather than abrupt changes.
 */
class TransitionManager
{
public:
    /**
     * @brief Get the singleton instance
     * @return Reference to the TransitionManager instance
     */
    static TransitionManager &getInstance();

    /**
     * @brief Start a transition to a new profile
     * @param targetPosition The target position to transition to
     * @param targetRotation The target rotation to transition to
     * @param durationSeconds Duration of the transition in seconds, or -1 to use default
     */
    void startTransition(const Vector3 &targetPosition, const Quaternion &targetRotation, float durationSeconds);

    /**
     * @brief Update the transition (call every frame)
     * @param deltaTime Time elapsed since last frame in seconds
     * @param outPosition Output parameter for the interpolated position
     * @param outRotation Output parameter for the interpolated rotation
     * @return true if transition is still in progress, false if completed
     */
    bool updateTransition(float deltaTime, Vector3 &outPosition, Quaternion &outRotation);

    /**
     * @brief Check if a transition is in progress
     * @return true if a transition is currently active
     */
    bool isTransitioning() const;

    /**
     * @brief Manually cancel the current transition
     */
    void cancelTransition();

    // Configuration methods
    void setTransitionDuration(float seconds) { m_defaultDuration = seconds; }
    void setUseSpringPhysics(bool enable) { m_useSpringPhysics = enable; }
    void setSpringStrength(float value) { m_springStrength = value; }
    void setSpringDamping(float value) { m_springDamping = value; }

private:
    TransitionManager();
    ~TransitionManager() = default;

    // Prevent copying
    TransitionManager(const TransitionManager &) = delete;
    TransitionManager &operator=(const TransitionManager &) = delete;

    // Transition state
    bool m_isTransitioning;
    float m_transitionProgress;
    float m_transitionDuration;
    float m_defaultDuration;

    // Spring physics
    bool m_useSpringPhysics;
    float m_springStrength;
    float m_springDamping;
    mutable Vector3 m_springVelocity;

    // Current source and target states
    CameraState m_sourceState;
    CameraState m_targetState;

    /**
     * @brief Smoothstep function for natural easing
     * @param x Input value in range [0,1]
     * @return Smoothed value in range [0,1]
     */
    float smoothstep(float x) const;

    /**
     * @brief Apply spring physics to position
     * @param current Current position
     * @param target Target position
     * @param deltaTime Time elapsed since last frame
     * @return New position after applying spring physics
     */
    Vector3 applySpringPhysics(const Vector3 &current, const Vector3 &target, float deltaTime);
};

#endif // TRANSITION_MANAGER_H
