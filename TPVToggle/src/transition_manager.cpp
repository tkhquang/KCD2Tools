#include "transition_manager.hpp"
#include <DetourModKit.hpp>
#include "global_state.hpp"
#include <algorithm>
#include <cmath>

using DetourModKit::LogLevel;

TransitionManager &TransitionManager::getInstance()
{
    static TransitionManager instance;
    return instance;
}

TransitionManager::TransitionManager()
    : m_isTransitioning(false),
      m_transitionProgress(0.0f),
      m_transitionDuration(0.5f),
      m_defaultDuration(0.5f),
      m_useSpringPhysics(false),
      m_springStrength(10.0f),
      m_springDamping(0.8f),
      m_springVelocity(0.0f, 0.0f, 0.0f)
{
}

void TransitionManager::startTransition(
    const Vector3 &targetPosition,
    const Quaternion &targetRotation,
    float durationSeconds)
{
    // Snapshot the live offset as the source only on a fresh transition, so a
    // re-target mid-flight blends from the current pose rather than snapping back.
    if (!m_isTransitioning)
    {
        m_sourceState = CameraState(g_currentCameraOffset.load(), Quaternion::Identity());
    }

    m_targetState = CameraState(targetPosition, targetRotation);

    m_transitionProgress = 0.0f;
    m_transitionDuration = (durationSeconds > 0.0f) ? durationSeconds : m_defaultDuration;
    m_springVelocity = Vector3(0.0f, 0.0f, 0.0f);
    m_isTransitioning = true;

    DMKLogger::get_instance().log(LogLevel::Debug, "TransitionManager: Started transition to: (" +
                                             std::to_string(targetPosition.x) + ", " +
                                             std::to_string(targetPosition.y) + ", " +
                                             std::to_string(targetPosition.z) + ") over " +
                                             std::to_string(m_transitionDuration) + " seconds");
}

bool TransitionManager::updateTransition(float deltaTime, Vector3 &outPosition, Quaternion &outRotation)
{
    if (!m_isTransitioning)
    {
        return false;
    }

    m_transitionProgress += deltaTime / m_transitionDuration;

    if (m_transitionProgress >= 1.0f)
    {
        m_isTransitioning = false;
        m_transitionProgress = 1.0f;

        // Snap exactly to the target on completion to avoid float drift left by
        // the interpolation.
        outPosition = m_targetState.position;
        outRotation = m_targetState.rotation;

        DMKLogger::get_instance().log(LogLevel::Debug, "TransitionManager: Transition completed");
        return false; // Transition is complete
    }

    float t = smoothstep(m_transitionProgress);

    Vector3 interpolatedPosition = Vector3(
        m_sourceState.position.x + (m_targetState.position.x - m_sourceState.position.x) * t,
        m_sourceState.position.y + (m_targetState.position.y - m_sourceState.position.y) * t,
        m_sourceState.position.z + (m_targetState.position.z - m_sourceState.position.z) * t);

    if (m_useSpringPhysics)
    {
        interpolatedPosition = applySpringPhysics(interpolatedPosition, m_targetState.position, deltaTime);
    }

    // Spherical linear interpolation keeps angular velocity constant across the
    // transition and avoids the gimbal artifacts a component-wise lerp would
    // introduce on rotations.
    Quaternion interpolatedRotation = Quaternion::Slerp(m_sourceState.rotation, m_targetState.rotation, t);

    outPosition = interpolatedPosition;
    outRotation = interpolatedRotation;

    return true; // Transition is still in progress
}

bool TransitionManager::isTransitioning() const
{
    return m_isTransitioning;
}

void TransitionManager::cancelTransition()
{
    if (m_isTransitioning)
    {
        m_isTransitioning = false;
        DMKLogger::get_instance().log(LogLevel::Debug, "TransitionManager: Transition cancelled");
    }
}

float TransitionManager::smoothstep(float x) const
{
    // Smoothstep provides a smooth ease-in/ease-out curve
    // This is a common function in computer graphics for natural interpolation
    x = std::max(0.0f, std::min(1.0f, x));
    return x * x * (3.0f - 2.0f * x);
}

Vector3 TransitionManager::applySpringPhysics(const Vector3 &current, const Vector3 &target, float deltaTime)
{
    // Calculate spring force
    Vector3 displacement = target - current;
    Vector3 springForce = displacement * m_springStrength;

    // Apply damping to velocity
    m_springVelocity = m_springVelocity * (1.0f - m_springDamping * deltaTime);

    // Apply spring force to velocity
    m_springVelocity = m_springVelocity + springForce * deltaTime;

    // Apply velocity to position
    return current + m_springVelocity * deltaTime;
}
