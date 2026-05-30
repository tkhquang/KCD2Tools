#ifndef TRANSITION_MANAGER_HPP
#define TRANSITION_MANAGER_HPP

#include "math_utils.hpp"

#include <atomic>
#include <mutex>

namespace TPVToggle
{

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
    [[nodiscard]] bool updateTransition(float deltaTime, Vector3 &outPosition, Quaternion &outRotation);

    /**
     * @brief Check if a transition is in progress
     * @return true if a transition is currently active
     */
    [[nodiscard]] bool isTransitioning() const noexcept;

    /**
     * @brief Manually cancel the current transition
     */
    void cancelTransition();

    // Configuration methods
    void setTransitionDuration(float seconds)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_defaultDuration = seconds;
    }
    void setUseSpringPhysics(bool enable)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_useSpringPhysics = enable;
    }
    void setSpringStrength(float value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_springStrength = value;
    }
    void setSpringDamping(float value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_springDamping = value;
    }

private:
    TransitionManager();
    ~TransitionManager() = default;

    // Prevent copying
    TransitionManager(const TransitionManager &) = delete;
    TransitionManager &operator=(const TransitionManager &) = delete;

    // The transition state below is mutated by updateTransition() on the per-frame
    // camera-hook thread and by startTransition()/cancelTransition() on the camera
    // profile thread, so it is guarded by m_mutex. m_isTransitioning is also atomic
    // so the per-frame fast path can skip the lock entirely while idle (no
    // transition running), which is the common case.
    std::atomic<bool> m_isTransitioning;
    mutable std::mutex m_mutex;

    // Transition state (guarded by m_mutex)
    float m_transitionProgress;
    float m_transitionDuration;
    float m_defaultDuration;

    // Spring physics (guarded by m_mutex)
    bool m_useSpringPhysics;
    float m_springStrength;
    float m_springDamping;
    Vector3 m_springVelocity;

    // Current source and target states (guarded by m_mutex)
    CameraState m_sourceState;
    CameraState m_targetState;

    /**
     * @brief Smoothstep function for natural easing
     * @param x Input value in range [0,1]
     * @return Smoothed value in range [0,1]
     */
    float smoothstep(float x) const noexcept;

    /**
     * @brief Apply spring physics to position
     * @param current Current position
     * @param target Target position
     * @param deltaTime Time elapsed since last frame
     * @return New position after applying spring physics
     */
    Vector3 applySpringPhysics(const Vector3 &current, const Vector3 &target, float deltaTime);
};

} // namespace TPVToggle

#endif // TRANSITION_MANAGER_HPP
