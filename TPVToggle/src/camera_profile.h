#ifndef CAMERA_PROFILE_H
#define CAMERA_PROFILE_H

#include <string>
#include <vector>
#include <filesystem>
#include "math_utils.h"
#include "transition_manager.h"

// Structure to represent a camera profile
struct CameraProfile
{
    std::string name;
    Vector3 offset;

    CameraProfile(const std::string &name = "Default",
                  const Vector3 &offset = Vector3(0.0f, 0.0f, 0.0f))
        : name(name), offset(offset) {}
};

class CameraProfileManager
{
public:
    static CameraProfileManager &getInstance();

    // Profile management
    bool loadProfiles(const std::string &directory);
    bool saveCurrentProfile(const std::string &name);
    bool cycleToNextProfile();
    bool setProfileByIndex(size_t index);
    void resetToDefault();

    // Get current settings
    const CameraProfile &getCurrentProfile() const;
    const Vector3 &getCurrentOffset() const;
    size_t getProfileCount() const;
    size_t getCurrentProfileIndex() const;

    // Live adjustment
    void adjustOffset(float x, float y, float z);

    void setActiveProfile(size_t index, bool useTransition = true);
    void setTransitionSettings(float duration, bool useSpringPhysics, float springStrength, float springDamping);

private:
    CameraProfileManager();
    ~CameraProfileManager();

    // Prevent copying
    CameraProfileManager(const CameraProfileManager &) = delete;
    CameraProfileManager &operator=(const CameraProfileManager &) = delete;

    // Internal methods
    bool saveProfileToFile(const CameraProfile &profile);
    void ensureDirectoryExists();
    std::string generateUniqueFilename(const std::string &baseName);

    // Member variables
    std::vector<CameraProfile> m_profiles;
    size_t m_currentProfileIndex;
    std::string m_profileDirectory;
    bool m_isInitialized;
};

#endif // CAMERA_PROFILE_H
