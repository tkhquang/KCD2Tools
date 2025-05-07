#include "camera_profile.h"
#include "logger.h"
#include "constants.h"
#include "global_state.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <algorithm>

// Singleton implementation
CameraProfileManager &CameraProfileManager::getInstance()
{
    static CameraProfileManager instance;
    return instance;
}

CameraProfileManager::CameraProfileManager()
    : m_currentProfileIndex(0), m_isInitialized(false)
{
    // Add a default profile
    m_profiles.push_back(CameraProfile("Default", Vector3(0.0f, 0.0f, 0.0f)));
}

CameraProfileManager::~CameraProfileManager()
{
    // Nothing special needed
}

bool CameraProfileManager::loadProfiles(const std::string &directory)
{
    Logger &logger = Logger::getInstance();
    m_profileDirectory = directory;

    // Ensure we have the default profile
    if (m_profiles.empty())
    {
        m_profiles.push_back(CameraProfile("Default", Vector3(0.0f, 0.0f, 0.0f)));
    }

    ensureDirectoryExists();

    try
    {
        namespace fs = std::filesystem;

        // Load saved profiles
        size_t profileCount = 0;

        for (const auto &entry : fs::directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".profile")
            {
                std::ifstream file(entry.path(), std::ios::in);
                if (file.is_open())
                {
                    std::string name;
                    float x = 0.0f, y = 0.0f, z = 0.0f;

                    // Read profile data (basic format: name,x,y,z)
                    std::string line;
                    if (std::getline(file, line))
                    {
                        std::istringstream iss(line);
                        std::string name_part;

                        if (std::getline(iss, name_part, ','))
                        {
                            name = name_part;

                            iss >> x;
                            iss.ignore(); // skip comma
                            iss >> y;
                            iss.ignore(); // skip comma
                            iss >> z;

                            // Add profile (skip default if we already have it)
                            if (name != "Default" || m_profiles.size() <= 1)
                            {
                                m_profiles.push_back(CameraProfile(name, Vector3(x, y, z)));
                                profileCount++;
                            }
                        }
                    }
                    file.close();
                }
            }
        }

        logger.log(LOG_INFO, "CameraProfileManager: Loaded " + std::to_string(profileCount) +
                                 " camera profiles from " + directory);
        m_isInitialized = true;
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "CameraProfileManager: Error loading profiles: " + std::string(e.what()));
        return false;
    }
}

bool CameraProfileManager::saveCurrentProfile(const std::string &name)
{
    CameraProfile profile(name.empty() ? "Profile" : name, getCurrentOffset());
    return saveProfileToFile(profile);
}

bool CameraProfileManager::saveProfileToFile(const CameraProfile &profile)
{
    Logger &logger = Logger::getInstance();

    if (m_profileDirectory.empty())
    {
        logger.log(LOG_ERROR, "CameraProfileManager: Profile directory not set");
        return false;
    }

    ensureDirectoryExists();

    try
    {
        std::string filename = generateUniqueFilename(profile.name);
        std::string filepath = m_profileDirectory + "/" + filename + ".profile";

        std::ofstream file(filepath, std::ios::out);
        if (!file.is_open())
        {
            logger.log(LOG_ERROR, "CameraProfileManager: Failed to open file: " + filepath);
            return false;
        }

        // Write profile data (format: name,x,y,z)
        file << profile.name << ","
             << profile.offset.x << ","
             << profile.offset.y << ","
             << profile.offset.z;

        file.close();

        // Add to our list of profiles
        m_profiles.push_back(profile);
        m_currentProfileIndex = m_profiles.size() - 1;

        logger.log(LOG_INFO, "CameraProfileManager: Saved profile '" + profile.name +
                                 "' to " + filepath);
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "CameraProfileManager: Error saving profile: " + std::string(e.what()));
        return false;
    }
}

std::string CameraProfileManager::generateUniqueFilename(const std::string &baseName)
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << baseName << "_" << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S");
    return ss.str();
}

void CameraProfileManager::ensureDirectoryExists()
{
    namespace fs = std::filesystem;

    if (!m_profileDirectory.empty() && !fs::exists(m_profileDirectory))
    {
        try
        {
            fs::create_directories(m_profileDirectory);
            Logger::getInstance().log(LOG_INFO, "CameraProfileManager: Created directory: " + m_profileDirectory);
        }
        catch (const std::exception &e)
        {
            Logger::getInstance().log(LOG_ERROR, "CameraProfileManager: Failed to create directory: " +
                                                     std::string(e.what()));
        }
    }
}

bool CameraProfileManager::cycleToNextProfile()
{
    if (m_profiles.empty())
    {
        return false;
    }

    // Calculate next profile index (with wraparound)
    size_t nextIndex = (m_currentProfileIndex + 1) % m_profiles.size();

    // Set new profile with transition
    setActiveProfile(nextIndex, true);

    Logger::getInstance().log(LOG_INFO, "CameraProfileManager: Switched to profile '" + m_profiles[m_currentProfileIndex].name + "' (" +
                                            std::to_string(m_currentProfileIndex + 1) + "/" +
                                            std::to_string(m_profiles.size()) + ")");

    return true;
}

bool CameraProfileManager::setProfileByIndex(size_t index)
{
    if (index >= m_profiles.size())
    {
        return false;
    }

    m_currentProfileIndex = index;
    Logger::getInstance().log(LOG_INFO, "CameraProfileManager: Set profile to '" +
                                            m_profiles[m_currentProfileIndex].name + "'");
    return true;
}

void CameraProfileManager::resetToDefault()
{
    // // Look for the default profile or use the first one
    // for (size_t i = 0; i < m_profiles.size(); i++)
    // {
    //     if (m_profiles[i].name == "Default")
    //     {
    //         m_currentProfileIndex = i;
    //         Logger::getInstance().log(LOG_INFO, "CameraProfileManager: Reset to Default profile");
    //         return;
    //     }
    // }

    // // If no default found, create one
    // if (!m_profiles.empty())
    // {
    //     m_currentProfileIndex = 0;
    // }
    // else
    // {
    //     m_profiles.push_back(CameraProfile("Default", Vector3(0.0f, 0.0f, 0.0f)));
    //     m_currentProfileIndex = 0;
    //     Logger::getInstance().log(LOG_INFO, "CameraProfileManager: Created Default profile");
    // }

    adjustOffset(0.0f, 0.0f, 0.0f);
}

const CameraProfile &CameraProfileManager::getCurrentProfile() const
{
    if (m_profiles.empty())
    {
        static CameraProfile defaultProfile("Default", Vector3(0.0f, 0.0f, 0.0f));
        return defaultProfile;
    }
    return m_profiles[m_currentProfileIndex];
}

const Vector3 &CameraProfileManager::getCurrentOffset() const
{
    return getCurrentProfile().offset;
}

size_t CameraProfileManager::getProfileCount() const
{
    return m_profiles.size();
}

size_t CameraProfileManager::getCurrentProfileIndex() const
{
    return m_currentProfileIndex;
}

void CameraProfileManager::adjustOffset(float x, float y, float z)
{
    // We modify the current profile directly for live adjustment
    if (!m_profiles.empty())
    {
        Vector3 &offset = m_profiles[m_currentProfileIndex].offset;
        offset.x += x;
        offset.y += y;
        offset.z += z;

        // If this is verbose, we could limit to debug level or add rate limiting
        Logger::getInstance().log(LOG_DEBUG, "CameraProfileManager: Adjusted offset to (" +
                                                 std::to_string(offset.x) + ", " +
                                                 std::to_string(offset.y) + ", " +
                                                 std::to_string(offset.z) + ")");

        // Update the global state - this is what the camera hook uses
        g_currentCameraOffset = offset;
    }
}

void CameraProfileManager::setActiveProfile(size_t index, bool useTransition)
{
    if (index >= m_profiles.size())
    {
        return;
    }

    // Get the target profile
    const CameraProfile &targetProfile = m_profiles[index];

    // Set as current profile
    m_currentProfileIndex = index;

    // Start transition if enabled
    if (useTransition)
    {
        // We only transition position, not rotation (which stays identity)
        TransitionManager::getInstance().startTransition(
            targetProfile.offset,
            Quaternion::Identity(),
            -1.0f // Use default duration
        );
    }
    else
    {
        // Immediate switch without transition
        g_currentCameraOffset = targetProfile.offset;
    }

    Logger::getInstance().log(LOG_INFO, "CameraProfileManager: Set profile to '" +
                                            targetProfile.name + "'" + (useTransition ? " with transition" : " immediately"));
}

void CameraProfileManager::setTransitionSettings(
    float duration,
    bool useSpringPhysics,
    float springStrength,
    float springDamping)
{

    TransitionManager &transition = TransitionManager::getInstance();
    transition.setTransitionDuration(duration);
    transition.setUseSpringPhysics(useSpringPhysics);
    transition.setSpringStrength(springStrength);
    transition.setSpringDamping(springDamping);

    Logger::getInstance().log(LOG_INFO, "CameraProfileManager: Updated transition settings - Duration: " + std::to_string(duration) + "s, " +
                                            "Spring Physics: " + (useSpringPhysics ? "ON" : "OFF") + ", " +
                                            "Spring Strength: " + std::to_string(springStrength) + ", " +
                                            "Spring Damping: " + std::to_string(springDamping));
}
