#include "engine/controls/memorycuecontrol.h"

#include <QThread>

#include "moc_memorycuecontrol.cpp"
#include "track/memorycues.h"
#include "util/fpclassify.h"

MemoryCueControl::MemoryCueControl(const QString& group, UserSettingsPointer config)
        : EngineControl(group, config),
          m_quantize(group, "quantize"),
          m_closestBeat(group, "beat_closest") {
    static constexpr const char* keys[] = {"memory_cue_set",
            "memory_cue_next",
            "memory_cue_prev",
            "memory_cue_clear",
            "memory_cue_clear_nearest",
            "memory_cue_clear_prev",
            "memory_cue_clear_next",
            "memory_cue_clear_all"};
    for (std::size_t i = 0; i < kCapacity; ++i) {
        m_commands[i].sequence.store(i, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < m_buttons.size(); ++i) {
        m_buttons[i] = std::make_unique<ControlPushButton>(ConfigKey(group, keys[i]));
        m_buttons[i]->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(
                m_buttons[i].get(),
                &ControlObject::valueChanged,
                this,
                [state = m_inputCallbacks, i](double value) {
                    if (!util_isfinite(value) || value <= 0 || !state->control.load())
                        return;
                    // Sequential consistency pairs this owner check with teardown.
                    state->active.fetch_add(1);
                    if (auto* control = state->control.load()) {
                        control->enqueue(static_cast<Operation>(i));
                    }
                    state->active.fetch_sub(1);
                },
                Qt::DirectConnection);
    }
    m_overflowControl = std::make_unique<ControlObject>(
            ConfigKey(group, "memory_cue_overflows"));
    m_overflowControl->setReadOnly();
    m_timer.setInterval(2);
    connect(&m_timer, &QTimer::timeout, this, &MemoryCueControl::drainCommands);
    m_timer.start();
}

MemoryCueControl::~MemoryCueControl() {
    // Owner stops audio, reader-worker and controller emitters first. The
    // latter also protects ControlObject's own private-value forwarding.
    m_inputCallbacks->control.store(nullptr);
    // Off callback: finish already-entered bounded producers before their queue
    // storage is destroyed. Later signals cannot enter this control.
    while (m_inputCallbacks->active.load() != 0)
        QThread::yieldCurrentThread();
    m_timer.stop();
    reclaimNavigationSnapshots();
    delete m_readyNavigation.exchange(nullptr, std::memory_order_acq_rel);
    delete m_retiredNavigation.exchange(nullptr, std::memory_order_acq_rel);
}

void MemoryCueControl::reclaimNavigationSnapshots() {
    delete m_retiredNavigation.exchange(nullptr, std::memory_order_acq_rel);
}

void MemoryCueControl::invalidateTrack() {
    std::lock_guard<std::mutex> lock(m_trackMutex);
    m_generation.fetch_add(1, std::memory_order_release);
    m_track.reset();
}

void MemoryCueControl::trackLoaded(TrackPointer track) {
    std::lock_guard<std::mutex> lock(m_trackMutex);
    m_generation.fetch_add(1, std::memory_order_release);
    m_track = std::move(track);
}

void MemoryCueControl::setFrameInfo(mixxx::audio::FramePos currentPosition,
        mixxx::audio::FramePos,
        mixxx::audio::SampleRate rate) {
    m_position.setValue(Position{currentPosition,
            mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(m_closestBeat.get()),
            rate,
            m_generation.load(std::memory_order_acquire),
            m_seekEpoch.load(std::memory_order_relaxed),
            m_quantize.get() > 0});
}

void MemoryCueControl::notifySeek(mixxx::audio::FramePos) {
    m_seekEpoch.fetch_add(1, std::memory_order_release);
}

void MemoryCueControl::enqueue(Operation operation) {
    Position captured;
    if (!m_position.tryGetValue(&captured)) {
        m_overflows.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Keep position/beat coherent, but apply a toggle immediately before the press.
    captured.quantize = m_quantize.get() > 0;
    const Command command{operation, captured};
    auto position = m_enqueue.load(std::memory_order_relaxed);
    // Bound contention as well as queue capacity; reject newest on overflow.
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto& slot = m_commands[position % kCapacity];
        const auto sequence = slot.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position);
        if (difference < 0)
            break;
        if (difference == 0 &&
                m_enqueue.compare_exchange_strong(
                        position, position + 1, std::memory_order_relaxed)) {
            slot.command = command;
            slot.sequence.store(position + 1, std::memory_order_release);
            return;
        }
        position = m_enqueue.load(std::memory_order_relaxed);
    }
    m_overflows.fetch_add(1, std::memory_order_relaxed);
}

void MemoryCueControl::drainCommands() {
    DEBUG_ASSERT(QThread::currentThread() == thread());
    reclaimNavigationSnapshots();
    for (int count = 0; count < 32; ++count) {
        auto& slot = m_commands[m_dequeue % kCapacity];
        if (slot.sequence.load(std::memory_order_acquire) != m_dequeue + 1)
            break;
        const auto command = slot.command;
        slot.sequence.store(m_dequeue + kCapacity, std::memory_order_release);
        ++m_dequeue;
        std::lock_guard<std::mutex> lock(m_trackMutex);
        const auto& captured = command.position;
        if (!m_track || captured.generation != m_generation.load(std::memory_order_acquire) ||
                !captured.position.isValid())
            continue;
        const bool navigation = command.operation == Operation::Next ||
                command.operation == Operation::Previous;
        const auto navigationEpoch = navigation
                ? m_navigationEpoch.fetch_add(1, std::memory_order_acq_rel) + 1
                : 0;
        if (navigation) {
            delete m_readyNavigation.exchange(nullptr, std::memory_order_acq_rel);
        }
        auto revisionToken = navigation ? m_track->cueRevisionToken() : nullptr;
        const auto cueRevision = navigation
                ? revisionToken->load(std::memory_order_acquire)
                : 0;
        CuePointer target;
        switch (command.operation) {
        case Operation::Create:
            MemoryCues::create(*m_track,
                    captured.quantize && captured.closestBeat.isValid()
                            ? captured.closestBeat
                            : captured.position);
            break;
        case Operation::Next:
            target = MemoryCues::next(*m_track, captured.position);
            break;
        case Operation::Previous:
            target = MemoryCues::previous(*m_track, captured.position);
            break;
        case Operation::ClearCurrent:
            MemoryCues::removeCurrent(*m_track, captured.position);
            break;
        case Operation::ClearNearest:
            MemoryCues::removeNearest(*m_track, captured.position, captured.sampleRate);
            break;
        case Operation::ClearPrevious:
            MemoryCues::removePrevious(*m_track, captured.position);
            break;
        case Operation::ClearNext:
            MemoryCues::removeNext(*m_track, captured.position);
            break;
        case Operation::ClearAll:
            MemoryCues::removeAll(*m_track);
            break;
        }
        if (target && navigation &&
                cueRevision == revisionToken->load(std::memory_order_acquire)) {
            auto* snapshot = new NavigationSnapshot{target->getPosition(),
                    captured.generation,
                    captured.seekEpoch,
                    navigationEpoch,
                    cueRevision,
                    std::move(revisionToken)};
            delete m_readyNavigation.exchange(snapshot, std::memory_order_acq_rel);
        }
    }
    const double overflows = static_cast<double>(overflowCount());
    if (m_overflowControl->get() != overflows) {
        m_overflowControl->forceSet(overflows);
    }
}

void MemoryCueControl::process(double, mixxx::audio::FramePos, std::size_t) {
    if (m_retiredNavigation.load(std::memory_order_acquire) != nullptr)
        return;
    auto* snapshot = m_readyNavigation.exchange(nullptr, std::memory_order_acq_rel);
    if (!snapshot)
        return;
    if (snapshot->generation == m_generation.load(std::memory_order_acquire) &&
            snapshot->seekEpoch == m_seekEpoch.load(std::memory_order_acquire) &&
            snapshot->navigationEpoch == m_navigationEpoch.load(std::memory_order_acquire) &&
            snapshot->cueRevision == snapshot->revisionToken->load(std::memory_order_acquire) &&
            snapshot->position.isValid()) {
        // Exact: this stored cue was already quantized when created. No fresh
        // quantization against a different grid when applying the delayed target.
        seekExact(snapshot->position);
    }
    m_retiredNavigation.store(snapshot, std::memory_order_release);
}
