#include "transition_manager.h"
#include "logger.h"
#include "global_state.h"
#include <algorithm>
#include <cmath>

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
    // Store the current camera state if we're not already transitioning
    if (!m_isTransitioning)
    {
        m_sourceState = CameraState(g_currentCameraOffset, Quaternion::Identity());
    }

    // Set up the new target
    m_targetState = CameraState(targetPosition, targetRotation);

    // Reset transition parameters
    m_transitionProgress = 0.0f;
    m_transitionDuration = (durationSeconds > 0.0f) ? durationSeconds : m_defaultDuration;
    m_springVelocity = Vector3(0.0f, 0.0f, 0.0f);
    m_isTransitioning = true;

    Logger::getInstance().log(LOG_DEBUG, "TransitionManager: Started transition to: (" +
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

    // Update transition progress
    m_transitionProgress += deltaTime / m_transitionDuration;

    // Check if transition is complete
    if (m_transitionProgress >= 1.0f)
    {
        m_isTransitioning = false;
        m_transitionProgress = 1.0f;

        // Set final position and rotation
        outPosition = m_targetState.position;
        outRotation = m_targetState.rotation;

        Logger::getInstance().log(LOG_DEBUG, "TransitionManager: Transition completed");
        return false; // Transition is complete
    }

    // Calculate smoothed transition factor
    float t = smoothstep(m_transitionProgress);

    // Interpolate position
    Vector3 interpolatedPosition = Vector3(
        m_sourceState.position.x + (m_targetState.position.x - m_sourceState.position.x) * t,
        m_sourceState.position.y + (m_targetState.position.y - m_sourceState.position.y) * t,
        m_sourceState.position.z + (m_targetState.position.z - m_sourceState.position.z) * t);

    // Apply spring physics if enabled
    if (m_useSpringPhysics)
    {
        interpolatedPosition = applySpringPhysics(interpolatedPosition, m_targetState.position, deltaTime);
    }

    // Interpolate rotation using SLERP
    // Note: We're using spherical linear interpolation for smoother rotation transitions
    // The DirectXMath library's XMQuaternionSlerp function handles this for us behind the scenes
    Quaternion interpolatedRotation = Quaternion::Slerp(m_sourceState.rotation, m_targetState.rotation, t);

    // Return result
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
        Logger::getInstance().log(LOG_DEBUG, "TransitionManager: Transition cancelled");
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
