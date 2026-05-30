#include "camera_profile.hpp"
#include <DetourModKit.hpp>
#include "constants.hpp"
#include "global_state.hpp"
#include "utils.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <filesystem>


using json = nlohmann::json;

namespace TPVToggle
{

CameraProfileManager &CameraProfileManager::getInstance()
{
    static CameraProfileManager instance;
    return instance;
}

CameraProfileManager::CameraProfileManager()
    : m_currentProfileIndex(0), // Default to 0, validated after loading
      m_isInitialized(false),
      m_profilesModified(false),
      m_lastSaveTime(0)
{
}

CameraProfileManager::~CameraProfileManager()
{
    // Attempt immediate save if modifications are pending on exit
    if (m_profilesModified)
    {
        DMK::Logger::get_instance().info("CameraProfileManager: Saving modified profiles on exit...");
        saveProfilesToJson();
    }
}

// --- Initialization & Persistence ---

bool CameraProfileManager::loadProfiles(const std::string &directory)
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();
    m_profileDirectory = directory;

    std::filesystem::path dirPath = directory;
    std::filesystem::path filePath = dirPath / (std::string(Constants::MOD_NAME) + "_Profiles.json");
    m_jsonProfilesPath = filePath.lexically_normal().string();

    logger.info("CameraProfileManager: Loading profiles from: {}", m_jsonProfilesPath);

    bool jsonLoadedSuccessfully = loadProfilesFromJson();

    // Ensure "Default" profile exists at index 0
    auto it_default = std::find_if(m_profiles.begin(), m_profiles.end(),
                                   [](const CameraProfile &p)
                                   { return p.name == "Default"; });

    if (it_default == m_profiles.end())
    {
        logger.info("CameraProfileManager: 'Default' profile not found. Creating new default profile.");
        m_profiles.insert(m_profiles.begin(), CameraProfile("Default", Vector3(0.0f, 0.0f, 0.0f), "Default", generateTimestamp()));
        // Don't call debounce save yet, wait until end of load
    }
    else
    {
        size_t found_default_idx = std::distance(m_profiles.begin(), it_default);
        if (found_default_idx != 0)
        {
            logger.debug("CameraProfileManager: Moving 'Default' profile from index {} to 0.", found_default_idx);
            std::rotate(m_profiles.begin(), it_default, it_default + 1);
        }
        else
        {
            logger.debug("CameraProfileManager: 'Default' profile found at index 0.");
        }
    }

    m_isInitialized = true;
    logger.debug("CameraProfileManager: Manager initialized flag set.");

    // Now that initialized flag is true, setActiveProfile can run correctly.
    // Activate the "Default" profile (index 0) initially, loading its saved state.
    setActiveProfile(0, false); // false = no transition on initial load is appropriate.

    // Consolidate modification check and debounce trigger here if Default was created/moved
    if (it_default == m_profiles.end() || (it_default != m_profiles.end() && std::distance(m_profiles.begin(), it_default) != 0))
    {
        // If default was missing OR if it was found but had to be moved
        if (!jsonLoadedSuccessfully && m_profiles.size() == 1)
        {
            // Only mark modified if we just created the default and nothing was loaded from JSON.
            // If JSON was loaded and we just moved default, assume user wants loaded state preserved initially.
            // Maybe even only save if *only* Default exists and it was created now.
            logger.debug("CameraProfileManager: Marking profiles as modified (Default created/moved).");
            markProfilesModifiedAndDebounceSave();
        }
        else if (it_default != m_profiles.end() && std::distance(m_profiles.begin(), it_default) != 0)
        {
            // Also mark modified if we had to rotate the existing Default profile from JSON
            logger.debug("CameraProfileManager: Marking profiles as modified (Default rotated).");
            markProfilesModifiedAndDebounceSave();
        }
    }

    // Log final state *after* potentially setting active profile correctly.
    // Use getCurrentProfile() which respects the now set m_isInitialized flag.
    std::string activeName = "N/A";
    if (m_isInitialized && !m_profiles.empty())
    {
        activeName = getCurrentProfile().name; // Get name safely after init
    }
    else if (!m_profiles.empty())
    {
        activeName = m_profiles[0].name; // Fallback if init failed but profiles exist
    }

    logger.info("CameraProfileManager: Initialization complete. Active profile: '{}'. Total profiles: {}.",
                activeName, m_profiles.size());

    return true; // Return overall success (could refine based on steps)
}

bool CameraProfileManager::loadProfilesFromJson()
{
    // Assumes lock is held by caller (loadProfiles)
    DMK::Logger &logger = DMK::Logger::get_instance();

    m_profiles.clear();

    if (!std::filesystem::exists(m_jsonProfilesPath))
    {
        logger.info("CameraProfileManager: Profiles file not found: {}", m_jsonProfilesPath);
        return false; // Indicate file not found
    }

    std::vector<CameraProfile> loaded_profiles_temp; // Load into temporary vector

    try
    {
        std::ifstream file(m_jsonProfilesPath);
        if (!file.is_open())
        {
            logger.error("CameraProfileManager: Failed to open JSON profiles file for reading: {}", m_jsonProfilesPath);
            return false; // Indicate file error
        }

        json profilesJson;
        file >> profilesJson;
        file.close();

        if (!profilesJson.is_array())
        {
            logger.error("CameraProfileManager: Invalid JSON format in profiles file (expected an array): {}", m_jsonProfilesPath);
            return false; // Indicate format error
        }

        if (profilesJson.empty())
        {
            logger.info("CameraProfileManager: Profiles file is empty: {}", m_jsonProfilesPath);
            return true; // Successfully loaded an empty list
        }

        int errorCount = 0;
        for (const auto &profileJson : profilesJson)
        {
            if (!profileJson.is_object())
            {
                logger.warning("CameraProfileManager: Skipping non-object entry in profiles JSON array.");
                errorCount++;
                continue;
            }
            CameraProfile profile = profileFromJson(profileJson); // Handles errors per object
            if (profile.name.rfind("ErrorProfile", 0) != 0)       // Check it wasn't an error profile
            {
                loaded_profiles_temp.push_back(profile);
            }
            else
            {
                errorCount++;
            }
        }

        if (errorCount > 0)
        {
            logger.warning("CameraProfileManager: Skipped {} invalid profile entries during JSON load.", errorCount);
        }

        if (!loaded_profiles_temp.empty())
        {
            m_profiles = std::move(loaded_profiles_temp);
            logger.debug("CameraProfileManager: Successfully parsed {} profiles from JSON.", m_profiles.size());
        }
        else
        {
            logger.warning("CameraProfileManager: No valid profiles found in JSON file: {}", m_jsonProfilesPath);
        }

        // Mark as unmodified since state now matches the file (or empty if file was bad/empty)
        m_profilesModified = false;
        m_lastSaveTime = std::time(nullptr);

        return true; // Indicate successful processing of the file
    }
    catch (const json::parse_error &e)
    {
        logger.error("CameraProfileManager: JSON parsing error: {} in file: {}", e.what(), m_jsonProfilesPath);
        m_profiles.clear(); // Ensure profiles list is empty on error
        return false;       // Indicate parse error
    }
    catch (const std::exception &e)
    {
        logger.error("CameraProfileManager: Error reading or processing profiles file: {}. File: {}", e.what(), m_jsonProfilesPath);
        m_profiles.clear();
        return false; // Indicate generic error
    }
}

bool CameraProfileManager::saveProfilesToJson()
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!m_isInitialized)
    {
        logger.warning("CameraProfileManager: Attempted to save profiles before initialization.");
        return false;
    }

    // Intentionally allow saving an empty list (e.g., after deleting all user profiles)
    // The load logic handles creating 'Default' if the file is empty or missing.

    json profilesArray = json::array();
    try
    {
        for (const auto &profile : m_profiles)
        {
            json profileJson;
            profileToJson(profile, profileJson);
            profilesArray.push_back(profileJson);
        }
    }
    catch (const json::exception &e)
    {
        logger.error("CameraProfileManager: JSON library error during profile serialization: {}", e.what());
        return false; // Don't proceed if serialization fails
    }

    try
    {
        std::ofstream outFile(m_jsonProfilesPath);
        if (!outFile.is_open())
        {
            logger.error("CameraProfileManager: Failed to open JSON file for writing: {}", m_jsonProfilesPath);
            // Keep m_profilesModified = true if open fails
            return false;
        }

        outFile << std::setw(4) << profilesArray << '\n';
        outFile.flush();
        if (!outFile.good())
        {
            logger.error("CameraProfileManager: Failed to write all profile data to JSON file: {}", m_jsonProfilesPath);
            outFile.close();
            // Keep m_profilesModified = true if write fails
            return false;
        }

        outFile.close();

        // Save successful: reset flag and update timestamp
        m_profilesModified = false;
        m_lastSaveTime = std::time(nullptr);

        logger.info("CameraProfileManager: Successfully saved {} profiles to {}", m_profiles.size(), m_jsonProfilesPath);
        return true;
    }
    catch (const std::exception &e) // Catch potential filesystem errors during write/close
    {
        logger.error("CameraProfileManager: Filesystem error saving profiles to JSON: {}", e.what());
        // Keep m_profilesModified = true on other errors
        return false;
    }
}

// Internal function to trigger save after modifications to m_profiles
void CameraProfileManager::markProfilesModifiedAndDebounceSave()
{
    // Assumes lock is already held by caller
    if (!m_isInitialized)
        return; // Don't try to save if not ready

    m_profilesModified = true;
    time_t now_time = std::time(nullptr);
    if (now_time - m_lastSaveTime >= SAVE_DEBOUNCE_SECONDS)
    {
        saveProfilesToJson(); // This resets m_profilesModified if successful
    }
    else
    {
        DMK::Logger::get_instance().debug("CameraProfileManager: Profile save debounced (change marked).");
    }
}

// --- Profile Lifecycle Actions ---

bool CameraProfileManager::createNewProfileFromLiveState(const std::string &category)
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!m_isInitialized)
    {
        logger.warning("CreateNew: Not initialized.");
        return false;
    }

    // Generate a unique name (timestamp based)
    std::string new_profile_name;
    try
    {
        auto now_chrono = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_chrono.time_since_epoch()) % 1000;
        auto timer = std::chrono::system_clock::to_time_t(now_chrono);
        std::tm timeinfo_tm{};
#ifdef _MSC_VER
        localtime_s(&timeinfo_tm, &timer);
#else
        std::tm *p_tm = std::localtime(&timer);
        if (p_tm)
            timeinfo_tm = *p_tm;
        else
            throw std::runtime_error("std::localtime failed");
#endif

        std::stringstream ss_name;
        ss_name << "Profile_" << std::put_time(&timeinfo_tm, "%H%M%S") << "_" << std::setfill('0') << std::setw(3) << ms.count();
        new_profile_name = ss_name.str();
    }
    catch (const std::exception &e)
    {
        logger.error("CreateNew: Failed to generate profile name: {}", e.what());
        new_profile_name = "Profile_ErrorName"; // Fallback name
    }

    // Snapshot the live offset once so the saved profile and the log line agree.
    const Vector3 live_offset = TPVToggle::camera_state().offset.load();
    CameraProfile new_profile(new_profile_name, live_offset, category.empty() ? "General" : category, generateTimestamp());

    m_profiles.push_back(new_profile);
    m_currentProfileIndex = m_profiles.size() - 1;

    logger.info("CameraProfileManager: Created new profile '{}' from live offset ({}, {}, {}). Switched active profile.",
                new_profile.name, live_offset.x, live_offset.y, live_offset.z);

    markProfilesModifiedAndDebounceSave();

    // Technically TPVToggle::camera_state().offset already matches the new profile's saved offset
    // No need to call setActiveProfile here.

    return true;
}

bool CameraProfileManager::updateActiveProfileWithLiveState()
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!m_isInitialized)
    {
        logger.warning("UpdateActive: Not initialized.");
        return false;
    }

    if (m_profiles.empty() || m_currentProfileIndex >= m_profiles.size())
    {
        logger.error("UpdateActive: Invalid active profile index.");
        return false;
    }

    CameraProfile &active_profile_ref = m_profiles[m_currentProfileIndex];
    active_profile_ref.offset = TPVToggle::camera_state().offset.load();
    active_profile_ref.timestamp = generateTimestamp();
    // Category is intentionally left unchanged; only offset and timestamp update.

    logger.info("CameraProfileManager: Updated saved state for active profile '{}' with live offset.", active_profile_ref.name);

    markProfilesModifiedAndDebounceSave();

    return true;
}

bool CameraProfileManager::deleteProfile(size_t index)
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!m_isInitialized)
    {
        logger.warning("DeleteProfile: Not initialized.");
        return false;
    }

    // Prevent deleting Default (index 0)
    if (index == 0)
    {
        logger.warning("DeleteProfile: Cannot delete the 'Default' profile (index 0).");
        return false;
    }
    if (index >= m_profiles.size())
    {
        logger.error("DeleteProfile: Invalid index {}. Max allowed: {}.", index, m_profiles.size() - 1);
        return false;
    }

    std::string deletedName = m_profiles[index].name;
    m_profiles.erase(m_profiles.begin() + index);
    logger.info("CameraProfileManager: Deleted profile '{}' (index {}).", deletedName, index);

    // Adjust current index if needed and activate a safe profile
    size_t previous_active_index = m_currentProfileIndex;
    bool active_profile_affected = false;

    if (m_profiles.empty())
    { // Should not happen if Default deletion blocked
        logger.error("DeleteProfile: Profile list became empty after deletion. Recreating Default.");
        m_profiles.insert(m_profiles.begin(), CameraProfile("Default", Vector3(0.0f, 0.0f, 0.0f), "Default", generateTimestamp()));
        m_currentProfileIndex = 0;
        active_profile_affected = true;
    }
    else if (previous_active_index == index)
    {
        // Deleted the active profile. Switch to Default.
        logger.info("DeleteProfile: Deleted active profile. Switching to 'Default'.");
        m_currentProfileIndex = 0;
        active_profile_affected = true;
    }
    else if (previous_active_index > index)
    {
        // Deleted profile *before* the active one. Decrement active index.
        m_currentProfileIndex--;
        logger.debug("DeleteProfile: Active index shifted from {} to {}", previous_active_index, m_currentProfileIndex);
        // No need to set active_profile_affected = true, profile itself is same
    }
    // else: Deleted after active, active index is unaffected.

    // If the active profile changed (either by deleting it or because list became empty)
    if (active_profile_affected)
    {
        // Load the state of the *new* current profile (which is likely Default now)
        setActiveProfile(m_currentProfileIndex, false); // Don't trigger transition for delete
    }

    markProfilesModifiedAndDebounceSave();

    return true;
}

bool CameraProfileManager::deleteActiveProfile()
{
    // Lock acquired internally by deleteProfile
    size_t current_index = getCurrentProfileIndex(); // Gets current index safely
    return deleteProfile(current_index);             // deleteProfile handles index 0 check
}

// --- Profile Selection & Activation ---

bool CameraProfileManager::cycleToNextProfile()
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);

    if (!m_isInitialized)
    {
        DMK::Logger::get_instance().warning("Not initialized.");
        return false;
    }
    if (m_profiles.empty())
    {
        DMK::Logger::get_instance().warning("No profiles to cycle.");
        return false;
    }
    if (m_profiles.size() == 1)
    {
        DMK::Logger::get_instance().info("CycleProfile: Only 'Default' profile exists. No cycling possible.");
        // Optionally re-activate Default to reset live offset? No, standard says cycle has no effect here.
        return true; // Cycle "succeeded" vacuously.
    }

    size_t nextIndex = (m_currentProfileIndex + 1) % m_profiles.size();
    setActiveProfile(nextIndex, true); // Handles loading offset, transitions, logging

    return true;
}

bool CameraProfileManager::setProfileByIndex(size_t index)
{
    // Lock acquired within setActiveProfile if called
    if (!m_isInitialized)
    {
        DMK::Logger::get_instance().warning("Not initialized.");
        return false;
    }

    if (index >= getProfileCount())
    { // Use getter for thread-safe count access (though lock is probably already held by caller often)
        DMK::Logger::get_instance().error("setProfileByIndex: Invalid index {}.", index);
        return false;
    }

    setActiveProfile(index, true); // Handles index validation again, but safe
    return true;
}

void CameraProfileManager::setActiveProfile(size_t index, bool useTransition)
{
    // *** NOTE: This is the ONLY function that should load m_profiles[...].offset into TPVToggle::camera_state().offset ***
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!m_isInitialized)
    {
        logger.warning("setActiveProfile called before initialized.");
        TPVToggle::camera_state().offset.store(Vector3()); // Safety reset
        m_currentProfileIndex = 0;
        return;
    }

    if (m_profiles.empty())
    { // Should only happen in extreme error state
        logger.error("setActiveProfile called when profile list is empty. Cannot activate.");
        TPVToggle::camera_state().offset.store(Vector3());
        m_currentProfileIndex = 0;
        return;
    }

    if (index >= m_profiles.size())
    {
        logger.error("setActiveProfile: Invalid index {}. Max allowed: {}. Using index 0 instead.",
                     index, m_profiles.size() - 1);
        index = 0; // Fallback to Default profile on invalid index
    }

    bool switching_to_same_index = (m_currentProfileIndex == index);

    m_currentProfileIndex = index;
    const CameraProfile &targetProfile = m_profiles[m_currentProfileIndex];

    std::string log_prefix = switching_to_same_index ? "Re-activating" : "Activating";
    if (switching_to_same_index)
    {
        logger.info("CameraProfileManager: {} profile '{}'. Reloaded its saved offset, discarding any unsaved live adjustments.",
                    log_prefix, targetProfile.name);
    }
    else
    {
        logger.info("CameraProfileManager: {} profile '{}' ({}/{}). Loaded its saved offset.",
                    log_prefix, targetProfile.name, m_currentProfileIndex + 1, m_profiles.size());
    }

    // --- Transition ---
    if (useTransition)
    {
        TransitionManager::getInstance().startTransition(
            targetProfile.offset,   // Target is the SAVED offset loaded above
            Quaternion::Identity(), // Rotation currently identity
            -1.0f                   // Use manager's default duration
        );
        logger.debug("CameraProfileManager: Started transition to saved offset.");
    }
    else
    {
        // Explicitly cancel any ongoing transition if switching instantly
        TransitionManager::getInstance().cancelTransition();

        logger.debug("CameraProfileManager: Applied saved offset immediately (no transition).");
    }

    TPVToggle::camera_state().offset.store(targetProfile.offset);
}

void CameraProfileManager::resetToDefault()
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!m_isInitialized || m_profiles.empty())
    {
        logger.warning("ResetToDefault: Cannot reset, not initialized or no profiles.");
        // Try setting live offset anyway? Maybe not useful without profiles structure.
        return;
    }

    size_t current_profile_index = getCurrentProfileIndex();
    CameraProfile current_profile = m_profiles[current_profile_index];

    setOffset(0.0f, 0.0f, 0.0f); // Sets live offset only; the saved profile is left untouched.
    logger.info("CameraProfileManager: Reset live camera offset to origin (profile '{}').", current_profile.name);
}

// --- Profile Metadata Modification ---
bool CameraProfileManager::renameProfile(size_t index, const std::string &newName)
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!m_isInitialized)
    {
        logger.warning("RenameProfile: Not initialized.");
        return false;
    }
    if (index >= m_profiles.size())
    {
        logger.error("RenameProfile: Invalid index.");
        return false;
    }
    if (newName.empty())
    {
        logger.error("RenameProfile: New name cannot be empty.");
        return false;
    }

    // Prevent renaming Default (index 0) or renaming TO Default
    if (index == 0)
    {
        logger.warning("RenameProfile: Cannot rename Default profile.");
        return false;
    }
    if (newName == "Default")
    {
        logger.warning("RenameProfile: Cannot rename profile TO 'Default'.");
        return false;
    }

    // Prevent duplicate names (optional but good practice)
    auto it_dup = std::find_if(m_profiles.begin(), m_profiles.end(),
                               [&](const CameraProfile &p)
                               { return p.name == newName; });
    if (it_dup != m_profiles.end())
    {
        logger.warning("RenameProfile: Profile name '{}' already exists.", newName);
        return false;
    }

    std::string oldName = m_profiles[index].name;
    m_profiles[index].name = newName;
    m_profiles[index].timestamp = generateTimestamp(); // Update timestamp on metadata change

    logger.info("CameraProfileManager: Renamed profile (idx {}) from '{}' to '{}'.", index, oldName, newName);

    markProfilesModifiedAndDebounceSave();
    return true;
}

bool CameraProfileManager::setProfileCategory(size_t index, const std::string &newCategory)
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!m_isInitialized)
    {
        logger.warning("SetCategory: Not initialized.");
        return false;
    }
    if (index >= m_profiles.size())
    {
        logger.error("SetCategory: Invalid index.");
        return false;
    }

    std::string categoryToSet = newCategory.empty() ? "General" : newCategory;

    if (index == 0 && categoryToSet != "Default")
    {
        logger.warning("SetCategory: Category for 'Default' profile should ideally remain 'Default'. Setting anyway.");
        // Allow it but warn. Could enforce by returning false here if desired.
    }

    std::string oldCategory = m_profiles[index].category;
    m_profiles[index].category = categoryToSet;
    m_profiles[index].timestamp = generateTimestamp();

    logger.info("CameraProfileManager: Changed category of profile '{}' from '{}' to '{}'.",
                m_profiles[index].name, oldCategory, m_profiles[index].category);

    markProfilesModifiedAndDebounceSave();
    return true;
}

// --- Getters for Saved State ---
const CameraProfile &CameraProfileManager::getCurrentProfile() const
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);

    if (!m_isInitialized || m_profiles.empty() || m_currentProfileIndex >= m_profiles.size())
    {
        static CameraProfile safeDefaultProfile("ErrorSafeDefault", Vector3(0.0f, 0.0f, 0.0f), "Error", "");
        // Avoid logging spam if called frequently in error state
        return safeDefaultProfile;
    }
    // Return reference to the SAVED profile object
    return m_profiles[m_currentProfileIndex];
}

Vector3 CameraProfileManager::getSavedOffsetOfCurrentProfile() const
{
    // getCurrentProfile handles locking and safety checks
    return getCurrentProfile().offset; // Return SAVED offset
}

size_t CameraProfileManager::getProfileCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    if (!m_isInitialized)
        return 0;
    return m_profiles.size();
}

size_t CameraProfileManager::getCurrentProfileIndex() const
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    if (!m_isInitialized)
        return 0;
    return m_currentProfileIndex;
}

std::vector<CameraProfile> CameraProfileManager::getAllProfiles() const
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    if (!m_isInitialized)
        return {};
    return m_profiles; // Return a copy
}

std::vector<size_t> CameraProfileManager::getProfileIndicesByCategory(const std::string &category) const
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    std::vector<size_t> indices;
    if (!m_isInitialized)
        return indices;

    for (size_t i = 0; i < m_profiles.size(); ++i)
    {
        if (m_profiles[i].category == category)
        {
            indices.push_back(i);
        }
    }
    return indices;
}

// --- Live Adjustments ---
// These publish TPVToggle::camera_state().offset, which the render hook reads every frame.
// Hold m_profileMutex so the seqlock writes serialize with each other (and with
// setActiveProfile); the render-thread reader stays lock-free via
// TPVToggle::camera_state().offset.load().
void CameraProfileManager::adjustOffset(float x, float y, float z)
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    TPVToggle::camera_state().offset.add(Vector3(x, y, z));
}

void CameraProfileManager::setOffset(float x, float y, float z)
{
    std::lock_guard<std::recursive_mutex> lock(m_profileMutex);
    TPVToggle::camera_state().offset.store(Vector3(x, y, z));
}

// --- Transition Configuration ---
void CameraProfileManager::setTransitionSettings(
    float duration,
    bool useSpringPhysics,
    float springStrength,
    float springDamping)
{
    // Assuming TransitionManager methods are safe to call without lock here
    TransitionManager &transition = TransitionManager::getInstance();
    transition.setTransitionDuration(duration);
    transition.setUseSpringPhysics(useSpringPhysics);
    transition.setSpringStrength(springStrength);
    transition.setSpringDamping(springDamping);

    DMK::Logger::get_instance().info("CameraProfileManager: Updated transition settings - Duration: {}s, Spring Physics: {}{}",
                                     duration, (useSpringPhysics ? "ON" : "OFF"),
                                     (useSpringPhysics ? ", Strength: " + std::to_string(springStrength) + ", Damping: " + std::to_string(springDamping) : ""));
}

std::string CameraProfileManager::generateTimestamp() const
{
    auto chrono_now = std::chrono::system_clock::now();
    auto time_now_t = std::chrono::system_clock::to_time_t(chrono_now);
    std::tm timeinfo_tm{};
#ifdef _MSC_VER
    localtime_s(&timeinfo_tm, &time_now_t);
#else
    // For non-MSVC, use std::localtime.
    // Note: std::localtime returns a pointer to a static internal buffer.
    // If generateTimestamp could be called concurrently from multiple threads
    // without external locking, this could be an issue.
    // However, its usage within CameraProfileManager methods is generally protected by m_profileMutex.
    std::tm *p_timeinfo_tm = std::localtime(&time_now_t);
    if (p_timeinfo_tm)
    { // Check for null in case of error
        timeinfo_tm = *p_timeinfo_tm;
    }
    else
    {
        // Handle error, e.g., return a default timestamp or log an error
        DMK::Logger::get_instance().error("CameraProfileManager::generateTimestamp: std::localtime failed.");
        return "TIMESTAMP_ERROR";
    }
#endif
    std::stringstream ss;
    ss << std::put_time(&timeinfo_tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void CameraProfileManager::profileToJson(const CameraProfile &profile, json &jsonObj) const
{
    jsonObj["name"] = profile.name;
    jsonObj["category"] = profile.category;
    jsonObj["timestamp"] = profile.timestamp;
    jsonObj["offset"] = {
        {"x", profile.offset.x},
        {"y", profile.offset.y},
        {"z", profile.offset.z}};
}

CameraProfile CameraProfileManager::profileFromJson(const json &jsonObj) const
{
    try
    {
        std::string name = jsonObj.value("name", "Unnamed Profile");
        std::string category = jsonObj.value("category", "General");
        std::string timestamp = jsonObj.value("timestamp", generateTimestamp());
        float x = 0.0f, y = 0.0f, z = 0.0f;

        if (jsonObj.contains("offset") && jsonObj["offset"].is_object())
        {
            const json &offsetObj = jsonObj["offset"];
            x = offsetObj.value("x", 0.0f);
            y = offsetObj.value("y", 0.0f);
            z = offsetObj.value("z", 0.0f);
        }
        return CameraProfile(name, Vector3(x, y, z), category, timestamp);
    }
    catch (const std::exception &e)
    {
        DMK::Logger::get_instance().warning("CameraProfileManager: Error parsing profile JSON: {}. Returning error profile.", e.what());
        return CameraProfile("ErrorProfile", Vector3(0, 0, 0), "Error", generateTimestamp());
    }
}

} // namespace TPVToggle
