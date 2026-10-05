#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace mixxx {

// AI-generated text begins.
// Publishers run off the audio thread. Readers borrow immutable data through a
// fixed hazard slot and never change ownership. Destruction requires all readers
// and publishers to have stopped. Sequentially consistent publication, hazard
// storage and validation ensure a reader either protects a generation before
// reclamation or detects its replacement before accessing it.
// A publisher retains only distinct generations in its captured hazard set,
// so at most ReaderCount retired owners are needed after each publication.
// AI-generated text ends.
template <typename T, std::size_t ReaderCount = 8>
class ImmutableAudioSnapshot {
    static_assert(ReaderCount > 0);
    static_assert(std::atomic<const T*>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    struct ReaderSlot {
        std::atomic<bool> claimed{false};
        std::atomic<const T*> hazard{nullptr};
    };

  public:
    class ReadGuard {
      public:
        ReadGuard() = default;
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        ReadGuard(ReadGuard&& other) noexcept
                : m_pSlot(std::exchange(other.m_pSlot, nullptr)),
                  m_pValue(std::exchange(other.m_pValue, nullptr)) {
        }

        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                release();
                m_pSlot = std::exchange(other.m_pSlot, nullptr);
                m_pValue = std::exchange(other.m_pValue, nullptr);
            }
            return *this;
        }

        ~ReadGuard() {
            release();
        }

        bool acquired() const {
            return m_pSlot != nullptr;
        }

        explicit operator bool() const {
            return m_pValue != nullptr;
        }

        const T* get() const {
            return m_pValue;
        }

        const T* operator->() const {
            return m_pValue;
        }

      private:
        friend class ImmutableAudioSnapshot;

        ReadGuard(ReaderSlot* pSlot, const T* pValue)
                : m_pSlot(pSlot), m_pValue(pValue) {
        }

        void release() {
            if (m_pSlot) {
                m_pSlot->hazard.store(nullptr);
                m_pSlot->claimed.store(false);
                m_pSlot = nullptr;
                m_pValue = nullptr;
            }
        }

        ReaderSlot* m_pSlot{nullptr};
        const T* m_pValue{nullptr};
    };

    ReadGuard acquire() const {
        for (auto& slot : m_readerSlots) {
            bool unclaimed = false;
            if (!slot.claimed.compare_exchange_strong(unclaimed, true)) {
                continue;
            }
            for (int attempt = 0; attempt < 8; ++attempt) {
                const T* pValue = m_pPublished.load();
                slot.hazard.store(pValue);
                if (pValue == m_pPublished.load()) {
                    return ReadGuard(&slot, pValue);
                }
            }
            slot.hazard.store(nullptr);
            slot.claimed.store(false);
            return {};
        }
        return {};
    }

    void publish(std::shared_ptr<const T> pValue) {
        const std::lock_guard<std::mutex> lock(m_publisherMutex);
        auto pPrevious = std::move(m_pOwner);
        m_pOwner = std::move(pValue);
        m_pPublished.store(m_pOwner.get());

        std::array<const T*, ReaderCount> hazards{};
        for (std::size_t i = 0; i < ReaderCount; ++i) {
            hazards[i] = m_readerSlots[i].hazard.load();
        }
        const auto isProtected = [&hazards](const T* pCandidate) {
            for (const T* pHazard : hazards) {
                if (pHazard == pCandidate) {
                    return true;
                }
            }
            return false;
        };
        for (auto& pRetired : m_retired) {
            if (pRetired &&
                    (pRetired.get() == m_pOwner.get() || !isProtected(pRetired.get()))) {
                pRetired.reset();
            }
        }
        if (!pPrevious || pPrevious.get() == m_pOwner.get() ||
                !isProtected(pPrevious.get())) {
            return;
        }
        for (const auto& pRetired : m_retired) {
            if (pRetired.get() == pPrevious.get()) {
                return;
            }
        }
        for (auto& pRetired : m_retired) {
            if (!pRetired) {
                pRetired = std::move(pPrevious);
                return;
            }
        }
        std::terminate();
    }

  private:
    mutable std::array<ReaderSlot, ReaderCount> m_readerSlots{};
    std::atomic<const T*> m_pPublished{nullptr};
    std::mutex m_publisherMutex;
    std::shared_ptr<const T> m_pOwner;
    std::array<std::shared_ptr<const T>, ReaderCount> m_retired{};
};

}
