/**
 * @file render/silhouette_shader.hpp
 * @brief The mod's own silhouette composite technique (shaders/HenrySensesSilhouette.cfx).
 *
 * The stock composite (DeferredSilhouettesOptimised) brightens and darkens alternate rows, darkens the screen edges
 * and finds edges with three asymmetric taps. The mod ships its own technique instead: the game loads tokenized
 * shaders only (a .cfxb, from ShadersBin.pak and then from the user shader cache on disk), so the embedded binary is
 * written into the user shader cache and loaded through the shader manager, which compiles its permutation at run
 * time. Until it draws, and for good when it cannot, the stock technique stays in use.
 */
#ifndef HENRYSENSES_SILHOUETTE_SHADER_HPP
#define HENRYSENSES_SILHOUETTE_SHADER_HPP

#include <cstdint>

namespace HenrySenses
{
    /**
     * @brief Loads the composite shader, once per session: writes henrysensessilhouette.cfxb into the user shader cache
     *        when the file there differs, then asks CShaderMan::mfForName for it.
     * @details Later calls return at once. Gated on Feature::SilhouetteShader; a failure is logged once and leaves the
     *          stock technique in use.
     * @note Main thread (file I/O and the shader manager).
     */
    void load_silhouette_shader() noexcept;

    /** @brief The loaded CShader, or 0 while it is not loaded, failed, or was rejected. Any thread. */
    [[nodiscard]] std::uintptr_t silhouette_shader() noexcept;

    /**
     * @brief Stops using the shader for the session and says why (the stock technique takes over).
     * @note Render thread.
     */
    void reject_silhouette_shader(const char *reason) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_SILHOUETTE_SHADER_HPP
