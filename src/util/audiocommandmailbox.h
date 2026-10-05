#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <type_traits>

#include "util/audiocallbackscope.h"
#include "util/boundedatomicvalue.h"

namespace mixxx {

// AI-generated documentation.
// One audio thread publishes and consumes commands for each mailbox. Other
// publishers serialize through the non-audio mutex. Readers may run on any
// thread. Callback publication and reads are bounded and do not allocate or lock.
// Payloads own no resources. Borrowed identity pointers require an externally
// guaranteed lifetime beyond publication and consumption. This mailbox never
// dereferences pointers; callers independently synchronize pointee access.
// End AI-generated documentation.
template<typename T>
class AudioCommandMailbox {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

  public:
    struct Snapshot {
        T value;
        std::uint64_t ticket;
    };

    explicit AudioCommandMailbox(const T& initial)
            : m_audioLane(Snapshot{initial, 0}),
              m_nonAudioLane(Snapshot{initial, 0}) {
    }

    void publish(const T& value) {
        if (AudioCallbackScope::isActive()) {
            publishToLane(value, &m_audioLane);
        } else {
            const std::lock_guard<std::mutex> lock(m_nonAudioWriterMutex);
            publishToLane(value, &m_nonAudioLane);
        }
    }

    bool tryGetPending(Snapshot* snapshot) const {
        Snapshot audio{};
        Snapshot nonAudio{};
        const bool gotAudio = m_audioLane.tryGetValue(&audio);
        const bool gotNonAudio = m_nonAudioLane.tryGetValue(&nonAudio);
        if (!gotAudio && !gotNonAudio) {
            return false;
        }
        const auto& newest = !gotNonAudio || (gotAudio && audio.ticket > nonAudio.ticket)
                ? audio
                : nonAudio;
        if (newest.ticket != m_newestAssignedTicket.load() ||
                newest.ticket <= m_consumedTicket.load()) {
            return false;
        }
        *snapshot = newest;
        return true;
    }

    bool hasPending() const {
        return m_newestAssignedTicket.load() > m_consumedTicket.load();
    }

    void consume(const Snapshot& snapshot) {
        m_consumedTicket.store(snapshot.ticket);
    }

  private:
    friend class AudioCommandMailboxTest;

    void publishToLane(const T& value, BoundedAtomicValue<Snapshot>* lane) {
        const auto ticket = m_newestAssignedTicket.fetch_add(1) + 1;
        lane->setValue(Snapshot{value, ticket});
    }

    BoundedAtomicValue<Snapshot> m_audioLane;
    BoundedAtomicValue<Snapshot> m_nonAudioLane;
    std::mutex m_nonAudioWriterMutex;
    std::atomic<std::uint64_t> m_newestAssignedTicket{0};
    std::atomic<std::uint64_t> m_consumedTicket{0};
};

}
