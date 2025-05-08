
#ifndef CAMERA_PROFILE_H
#define CAMERA_PROFILE_H

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include "math_utils.h"
#include "transition_manager.h"

#include <nlohmann/json.hpp>

// Structure to represent a saved camera profile's persistent state
struct CameraProfile
{
    std::string name;
    Vector3 offset; // The *SAVED* offset for this profile
    std::string category;
    std::string timestamp; // Last saved timestamp

    CameraProfile(const std::string &profile_name = "Default",
                  const Vector3 &profile_offset = Vector3(0.0f, 0.0f, 0.0f),
                  const std::string &profile_category = "General",
                  const std::string &profile_timestamp = "")
        : name(profile_name), offset(profile_offset), category(profile_category), timestamp(profile_timestamp) {}
};

// Manages camera profiles, separating live editing from saved states.
class CameraProfileManager
{
public:
    // Singleton access
    static CameraProfileManager &getInstance();

    // --- Initialization & Persistence ---
    /**
     * @brief Loads profiles from the specified directory (or creates default).
     * @param directory Path to the directory containing the profiles JSON file.
     * @return true if initialization was successful.
     */
    bool loadProfiles(const std::string &directory);
    /**
     * @brief Explicitly saves all profiles to the JSON file immediately.
     * @return true if save was successful.
     */
    bool saveProfilesToJson(); // Public for explicit save if needed outside debouncing

    // --- Profile Lifecycle Actions ---
    /**
     * @brief Creates a NEW profile using the current live camera offset (g_currentCameraOffset).
     *        Assigns a unique generated name and switches active profile to the new one.
     * @param category Optional category for the new profile.
     * @return true if profile creation was successful.
     */
    bool createNewProfileFromLiveState(const std::string &category = "General");
    /**
     * @brief Updates the SAVED state of the currently active profile using the live camera offset.
     *        REFUSES to update the "Default" profile (index 0).
     * @return true if update was successful, false if active profile is "Default" or invalid.
     */
    bool updateActiveProfileWithLiveState();
    /**
     * @brief Deletes the profile at the specified index. Cannot delete "Default" (index 0).
     * @param index The index of the profile to delete.
     * @return true if deletion was successful.
     */
    bool deleteProfile(size_t index);
    /**
     * @brief Helper to delete the currently active profile (unless it's "Default").
     * @return true if deletion was successful.
     */
    bool deleteActiveProfile();

    // --- Profile Selection & Activation ---
    /**
     * @brief Cycles to the next available profile and activates it.
     * @return true if cycling occurred.
     */
    bool cycleToNextProfile();
    /**
     * @brief Sets the active profile by index. Loads its saved offset to live state.
     * @param index Index of the profile to activate.
     * @return true if index is valid.
     */
    bool setProfileByIndex(size_t index);
    /**
     * @brief Activates a profile by index, loads its offset, handles transitions. Core activation logic.
     * @param index Index of the profile to activate.
     * @param useTransition If true, use smooth transition; otherwise, switch instantly.
     */
    void setActiveProfile(size_t index, bool useTransition = true);
    /**
     * @brief Resets the "Default" profile's saved offset to (0,0,0) and activates it.
     */
    void resetToDefault();

    // --- Profile Metadata Modification ---
    /**
     * @brief Renames the profile at the specified index. Cannot rename "Default".
     * @param index Index of the profile.
     * @param newName The new name for the profile.
     * @return true if successful.
     */
    bool renameProfile(size_t index, const std::string &newName);
    /**
     * @brief Sets the category for the profile at the specified index.
     * @param index Index of the profile.
     * @param newCategory The new category string.
     * @return true if successful.
     */
    bool setProfileCategory(size_t index, const std::string &newCategory);

    // --- Getters for Saved State ---
    /**
     * @brief Gets the profile object (saved state) for the currently active profile.
     * @return Const reference to the active CameraProfile. Returns safe default on error.
     */
    const CameraProfile &getCurrentProfile() const;
    /**
     * @brief Gets the SAVED offset Vector3 of the currently active profile.
     * @return The saved offset. Returns (0,0,0) on error.
     */
    Vector3 getSavedOffsetOfCurrentProfile() const;
    /**
     * @brief Gets the total number of saved profiles.
     * @return Profile count.
     */
    size_t getProfileCount() const;
    /**
     * @brief Gets the index of the currently active profile.
     * @return Active profile index.
     */
    size_t getCurrentProfileIndex() const;
    /**
     * @brief Gets a copy of the entire list of saved profiles.
     * @return std::vector<CameraProfile>
     */
    std::vector<CameraProfile> getAllProfiles() const;
    /**
     * @brief Filters profiles by category and returns their indices.
     * @param category Category string to filter by.
     * @return Vector of indices matching the category.
     */
    std::vector<size_t> getProfileIndicesByCategory(const std::string &category) const;

    // --- Live Adjustments (modify ONLY g_currentCameraOffset) ---
    /**
     * @brief Adds delta values to the live camera offset (g_currentCameraOffset).
     * @param x Delta X.
     * @param y Delta Y.
     * @param z Delta Z.
     */
    void adjustOffset(float x, float y, float z);
    /**
     * @brief Sets the live camera offset (g_currentCameraOffset) to absolute values.
     * @param x New X.
     * @param y New Y.
     * @param z New Z.
     */
    void setOffset(float x, float y, float z);

    // --- Transition Configuration ---
    void setTransitionSettings(float duration, bool useSpringPhysics, float springStrength, float springDamping);

private:
    // Constructor/Destructor
    CameraProfileManager();
    ~CameraProfileManager();

    // Delete copy/assignment
    CameraProfileManager(const CameraProfileManager &) = delete;
    CameraProfileManager &operator=(const CameraProfileManager &) = delete;

    // Internal JSON I/O
    bool loadProfilesFromJson();                                                     // Loads file into m_profiles
    void profileToJson(const CameraProfile &profile, nlohmann::json &jsonObj) const; // Profile -> JSON object
    CameraProfile profileFromJson(const nlohmann::json &jsonObj) const;              // JSON object -> Profile

    // Internal helper
    std::string generateTimestamp() const;

    // Internal persistence trigger
    void markProfilesModifiedAndDebounceSave(); // Sets flag and triggers save if needed

    // Member variables
    std::vector<CameraProfile> m_profiles;       // Stores the SAVED states
    size_t m_currentProfileIndex;                // Index of the active profile in m_profiles
    std::string m_profileDirectory;              // Directory containing JSON file
    std::string m_jsonProfilesPath;              // Full path to JSON file
    bool m_isInitialized;                        // Initialization flag
    mutable std::recursive_mutex m_profileMutex; // Protects m_profiles, m_currentProfileIndex, m_profilesModified, m_lastSaveTime

    // Save debouncing state
    bool m_profilesModified; // Tracks if m_profiles content requires saving
    time_t m_lastSaveTime;   // Timestamp of last successful save

    // Constants
    static constexpr int SAVE_DEBOUNCE_SECONDS = 2; // Debounce window
};

#endif // CAMERA_PROFILE_H
