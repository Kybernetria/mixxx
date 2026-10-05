#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

/// AI-generated documentation.
/// Single-writer, multi-reader scalar snapshot. Publication never waits; a
/// reader gives up after eight attempts. All payload words are atomic, so a
/// failed snapshot does not introduce the data race of a plain-data seqlock.
/// Sequentially consistent word operations preserve the sequence/payload order
/// across platforms. T must contain no owned resources. Borrowed identity
/// pointers require an externally guaranteed lifetime beyond publication and
/// consumption. This helper never owns or dereferences pointers; callers must
/// independently synchronize access to the pointed-to objects. This is
/// intentionally not a replacement for ControlValueAtomic.
/// End AI-generated documentation.
template<typename T>
class BoundedAtomicValue {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static constexpr std::size_t kWords = (sizeof(T) + 7) / 8;

  public:
    explicit BoundedAtomicValue(const T& initial) {
        setValue(initial);
    }
    void setValue(const T& value) {
        std::array<std::uint64_t, kWords> words{};
        std::memcpy(words.data(), &value, sizeof(T));
        m_sequence.fetch_add(1);
        for (std::size_t i = 0; i < kWords; ++i)
            m_words[i].store(words[i]);
        m_sequence.fetch_add(1);
    }
    bool tryGetValue(T* value) const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto before = m_sequence.load();
            if (before & 1)
                continue;
            std::array<std::uint64_t, kWords> words{};
            for (std::size_t i = 0; i < kWords; ++i)
                words[i] = m_words[i].load();
            if (before == m_sequence.load()) {
                std::memcpy(value, words.data(), sizeof(T));
                return true;
            }
        }
        return false;
    }

  private:
    std::atomic<std::uint64_t> m_sequence{0};
    std::array<std::atomic<std::uint64_t>, kWords> m_words{};
};
