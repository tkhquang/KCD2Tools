#include "transition_manager.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <cmath>

namespace TPVToggle
{

namespace
{
    // Floor for the transition duration so the per-frame progress division can
    // never divide by zero when a zero default duration is configured.
    constexpr float k_minTransitionDuration = 1e-4f;
} // namespace

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
    float started_duration = 0.0f;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Snapshot the live offset as the source only on a fresh transition, so a
        // re-target mid-flight blends from the current pose rather than snapping back.
        if (!m_isTransitioning.load(std::memory_order_relaxed))
        {
            m_sourceState = CameraState(TPVToggle::camera_state().offset.load(), Quaternion::Identity());
        }

        m_targetState = CameraState(targetPosition, targetRotation);

        m_transitionProgress = 0.0f;
        m_transitionDuration = std::max((durationSeconds > 0.0f) ? durationSeconds : m_defaultDuration, k_minTransitionDuration);
        m_springVelocity = Vector3(0.0f, 0.0f, 0.0f);
        started_duration = m_transitionDuration;

        // Publish the populated state before the per-frame reader's fast path can
        // observe the transition as active.
        m_isTransitioning.store(true, std::memory_order_release);
    }

    DMK::Logger::get_instance().debug("TransitionManager: Started transition to: ({}, {}, {}) over {} seconds",
                                      targetPosition.x, targetPosition.y, targetPosition.z, started_duration);
}

bool TransitionManager::updateTransition(float deltaTime, Vector3 &outPosition, Quaternion &outRotation)
{
    // Lock-free fast path: skip the mutex entirely while no transition is running
    // (the common per-frame case on the camera-hook thread).
    if (!m_isTransitioning.load(std::memory_order_acquire))
    {
        return false;
    }

    bool completed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Re-check under the lock: a concurrent completion or cancel may have
        // cleared the flag between the fast-path load and acquiring the mutex.
        if (!m_isTransitioning.load(std::memory_order_relaxed))
        {
            return false;
        }

        m_transitionProgress += deltaTime / m_transitionDuration;

        if (m_transitionProgress >= 1.0f)
        {
            m_isTransitioning.store(false, std::memory_order_release);
            m_transitionProgress = 1.0f;

            // Snap exactly to the target on completion to avoid float drift left
            // by the interpolation.
            outPosition = m_targetState.position;
            outRotation = m_targetState.rotation;
            completed = true;
        }
        else
        {
            const float t = smoothstep(m_transitionProgress);

            Vector3 interpolatedPosition = Vector3(
                m_sourceState.position.x + (m_targetState.position.x - m_sourceState.position.x) * t,
                m_sourceState.position.y + (m_targetState.position.y - m_sourceState.position.y) * t,
                m_sourceState.position.z + (m_targetState.position.z - m_sourceState.position.z) * t);

            if (m_useSpringPhysics)
            {
                interpolatedPosition = applySpringPhysics(interpolatedPosition, m_targetState.position, deltaTime);
            }

            // Spherical linear interpolation keeps angular velocity constant across
            // the transition and avoids the gimbal artifacts a component-wise lerp
            // would introduce on rotations.
            outPosition = interpolatedPosition;
            outRotation = Quaternion::Slerp(m_sourceState.rotation, m_targetState.rotation, t);
        }
    }

    // Log outside the lock so the Logger's own locks never nest under m_mutex.
    if (completed)
    {
        DMK::Logger::get_instance().debug("TransitionManager: Transition completed");
        return false; // Transition is complete
    }

    return true; // Transition is still in progress
}

bool TransitionManager::isTransitioning() const noexcept
{
    return m_isTransitioning.load(std::memory_order_acquire);
}

void TransitionManager::cancelTransition()
{
    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_isTransitioning.load(std::memory_order_relaxed))
        {
            m_isTransitioning.store(false, std::memory_order_release);
            was_active = true;
        }
    }

    if (was_active)
    {
        DMK::Logger::get_instance().debug("TransitionManager: Transition cancelled");
    }
}

float TransitionManager::smoothstep(float x) const noexcept
{
    // Classic Hermite ease-in/ease-out curve: 3x^2 - 2x^3, clamped to [0,1].
    x = std::max(0.0f, std::min(1.0f, x));
    return x * x * (3.0f - 2.0f * x);
}

Vector3 TransitionManager::applySpringPhysics(const Vector3 &current, const Vector3 &target, float deltaTime)
{
    // Spring force pulls the current position toward the target.
    const Vector3 displacement = target - current;
    const Vector3 springForce = displacement * m_springStrength;

    // Damp the velocity with an exponential decay rather than the linear factor
    // (1 - damping*dt): the linear form goes negative once damping*dt > 1 (a long
    // frame or a hitch), which would flip the velocity sign and make the spring
    // diverge. exp(-damping*dt) is in (0,1] for any non-negative dt, so it is
    // unconditionally stable.
    m_springVelocity = m_springVelocity * std::exp(-m_springDamping * deltaTime);

    // Integrate the spring force into the velocity, then the velocity into position.
    m_springVelocity = m_springVelocity + springForce * deltaTime;

    return current + m_springVelocity * deltaTime;
}

} // namespace TPVToggle
