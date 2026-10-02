#pragma once

#include <QTimer>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include "control/controlpushbutton.h"
#include "control/pollingcontrolproxy.h"
#include "engine/controls/enginecontrol.h"
#include "track/cue.h"
#include "util/boundedatomicvalue.h"

/// Commands carry last-published engine-frame positions, not worker-time
/// positions. Mutations are serialized on this QObject's (GUI) thread.
class MemoryCueControl final : public EngineControl {
    Q_OBJECT
  public:
    enum class Operation { Create,
        Next,
        Previous,
        ClearCurrent,
        ClearNearest,
        ClearPrevious,
        ClearNext,
        ClearAll };
    MemoryCueControl(const QString& group, UserSettingsPointer config);
    ~MemoryCueControl() override;
    void trackLoaded(TrackPointer track) override;
    void invalidateTrack(); // reader worker; cancels commands before replacement
    void setFrameInfo(mixxx::audio::FramePos currentPosition,
            mixxx::audio::FramePos endPosition,
            mixxx::audio::SampleRate rate) override;
    void process(double rate, mixxx::audio::FramePos position, std::size_t bufferSize) override;
    void notifySeek(mixxx::audio::FramePos position) override;
    void enqueue(Operation operation);
    std::uint64_t overflowCount() const {
        return m_overflows.load();
    }

  public slots:
    void drainCommands(); // main thread only; bounded per invocation

  private:
    void reclaimNavigationSnapshots(); // GUI only
    struct Position {
        mixxx::audio::FramePos position{};
        mixxx::audio::FramePos closestBeat{};
        mixxx::audio::SampleRate sampleRate{};
        std::uint64_t generation = 0;
        std::uint64_t seekEpoch = 0;
        bool quantize = false;
    };
    struct Command {
        Operation operation;
        Position position;
    };
    struct Slot {
        std::atomic<std::size_t> sequence{0};
        Command command{};
    };
    struct NavigationSnapshot {
        const mixxx::audio::FramePos position;
        const std::uint64_t generation;
        const std::uint64_t seekEpoch;
        const std::uint64_t navigationEpoch;
        const std::uint64_t cueRevision;
        const std::shared_ptr<std::atomic<std::uint64_t>> revisionToken;
    };
    static constexpr std::size_t kCapacity = 64;
    struct InputCallbackState {
        explicit InputCallbackState(MemoryCueControl* owner)
                : control(owner) {
        }
        std::atomic<MemoryCueControl*> control;
        std::atomic<unsigned> active{0};
    };
    static_assert(std::atomic<MemoryCueControl*>::is_always_lock_free &&
            std::atomic<unsigned>::is_always_lock_free &&
            std::atomic<NavigationSnapshot*>::is_always_lock_free);
    // Signal functors may still be executing after QObject disconnection.
    const std::shared_ptr<InputCallbackState> m_inputCallbacks{
            std::make_shared<InputCallbackState>(this)};
    std::array<Slot, kCapacity> m_commands;
    std::atomic<std::size_t> m_enqueue{0};
    std::size_t m_dequeue = 0; // single GUI consumer
    std::atomic<std::uint64_t> m_overflows{0};
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<std::uint64_t> m_seekEpoch{0};
    std::atomic<std::uint64_t> m_navigationEpoch{0};
    // GUI publishes; callback consumes ready and retires without deleting.
    std::atomic<NavigationSnapshot*> m_readyNavigation{nullptr};
    std::atomic<NavigationSnapshot*> m_retiredNavigation{nullptr};
    BoundedAtomicValue<Position> m_position{Position{}};
    std::mutex m_trackMutex; // worker and GUI only, never callback
    TrackPointer m_track;
    QTimer m_timer;
    PollingControlProxy m_quantize;
    PollingControlProxy m_closestBeat;
    std::array<std::unique_ptr<ControlPushButton>, 8> m_buttons;
    std::unique_ptr<ControlObject> m_overflowControl;
};
