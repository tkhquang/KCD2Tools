// transition_manager.h
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

class TransitionManager
{
public:
    static TransitionManager &getInstance();

    // Start a transition to a new profile
    void startTransition(const Vector3 &targetPosition, const Quaternion &targetRotation, float durationSeconds);

    // Update the transition (call every frame)
    bool updateTransition(float deltaTime, Vector3 &outPosition, Quaternion &outRotation);

    // Check if a transition is in progress
    bool isTransitioning() const;

    // Cancel current transition
    void cancelTransition();

    // Configuration
    void setTransitionDuration(float seconds) { m_defaultDuration = seconds; }
    void setUseSpringPhysics(bool enable) { m_useSpringPhysics = enable; }
    void setSpringStrength(float value) { m_springStrength = value; }
    void setSpringDamping(float value) { m_springDamping = value; }

private:
    TransitionManager();
    ~TransitionManager() = default;

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
    Vector3 m_springVelocity;

    // Current source and target states
    CameraState m_sourceState;
    CameraState m_targetState;

    // Smoothstep function for natural easing
    float smoothstep(float x) const;

    // Apply spring physics to position
    Vector3 applySpringPhysics(const Vector3 &current, const Vector3 &target, float deltaTime);
};

#endif // TRANSITION_MANAGER_H
