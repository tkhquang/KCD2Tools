/**
 * @file core/frame/channels.hpp
 * @brief Publication from the camera frame to other threads: a value one thread writes and any thread reads whole.
 */
#ifndef TPVCAMERA_CORE_FRAME_CHANNELS_HPP
#define TPVCAMERA_CORE_FRAME_CHANNELS_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace TPVCamera
{
    /**
     * @brief A value of @p T one thread publishes and any thread reads as one coherent copy (a seqlock).
     * @details The publisher marks the sequence odd, stores the value's bytes, then marks it even; a reader that sees
     *          the same even sequence before and after copying the bytes has a value from one publish. The bytes are
     *          held in relaxed atomic words, so a store racing a load is well defined rather than a data race, and the
     *          fences give the ordering on any memory model. Readers never block the publisher and allocate nothing,
     *          so a hook callback may read. Thread: one publisher; any number of readers.
     * @tparam T A trivially copyable value.
     */
    template <typename T> class SeqlockSnapshot
    {
        static_assert(std::is_trivially_copyable_v<T>, "A SeqlockSnapshot copies its value as raw bytes.");

    public:
        explicit SeqlockSnapshot(const T &initial = T{}) noexcept { store_words(initial); }

        /** @brief Publishes @p value. Only the one publishing thread calls this. */
        void publish(const T &value) noexcept
        {
            const std::uint32_t sequence = m_sequence.load(std::memory_order_relaxed);
            m_sequence.store(sequence + 1, std::memory_order_relaxed);
            // Orders the odd mark before the value's stores: a reader that copies a half-written value then sees an
            // odd or a changed sequence on its re-read.
            std::atomic_thread_fence(std::memory_order_release);
            store_words(value);
            m_sequence.store(sequence + 2, std::memory_order_release);
        }

        /**
         * @brief Copies the last published value into @p out.
         * @return False when a publish overlapped every one of k_read_attempts copies; @p out is untouched then.
         */
        [[nodiscard]] bool try_load(T &out) const noexcept
        {
            for (int attempt = 0; attempt < k_read_attempts; ++attempt)
            {
                const std::uint32_t before = m_sequence.load(std::memory_order_acquire);
                if ((before & 1u) != 0u)
                {
                    continue;
                }
                std::array<std::uint64_t, k_words> words{};
                for (std::size_t i = 0; i < k_words; ++i)
                {
                    words[i] = m_words[i].load(std::memory_order_relaxed);
                }
                // Pairs with the publisher's release: the re-read below then catches a publish that overlapped the
                // copies.
                std::atomic_thread_fence(std::memory_order_acquire);
                if (m_sequence.load(std::memory_order_relaxed) != before)
                {
                    continue;
                }
                std::memcpy(&out, words.data(), sizeof(T));
                return true;
            }
            return false;
        }

    private:
        static constexpr std::size_t k_words = (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
        // The publisher is one thread making progress and a publish is a few stores, so a reader that loses this many
        // races in a row is practically impossible; the bound keeps a hook callback from spinning.
        static constexpr int k_read_attempts = 64;

        void store_words(const T &value) noexcept
        {
            std::array<std::uint64_t, k_words> words{};
            std::memcpy(words.data(), &value, sizeof(T));
            for (std::size_t i = 0; i < k_words; ++i)
            {
                m_words[i].store(words[i], std::memory_order_relaxed);
            }
        }

        std::atomic<std::uint32_t> m_sequence{0};
        std::array<std::atomic<std::uint64_t>, k_words> m_words{};
    };
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_FRAME_CHANNELS_HPP
