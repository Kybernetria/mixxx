#pragma once

#include <atomic>

#include "engine/controls/enginecontrol.h"
#include "preferences/usersettings.h"
#include "track/beats.h"
#include "track/track_decl.h"
#include "util/immutableaudiosnapshot.h"

class ControlObject;
class ControlPushButton;

class QuantizeControl : public EngineControl {
    Q_OBJECT
  public:
    QuantizeControl(const QString& group, UserSettingsPointer pConfig);
    ~QuantizeControl() override;

    void setFrameInfo(mixxx::audio::FramePos currentPosition,
            mixxx::audio::FramePos trackEndPosition,
            mixxx::audio::SampleRate sampleRate) override;
    void trackLoaded(TrackPointer pNewTrack) override;
    void trackBeatsUpdated(mixxx::BeatsPointer pBeats) override;

  private:
    // Update positions of previous and next beats from beatgrid.
    void lookupBeatPositions(mixxx::audio::FramePos position, const mixxx::Beats* pBeats);
    // Update position of the closest beat based on existing previous and
    // next beat values.  Usually callers will call lookupBeatPositions first.
    void updateClosestBeat(mixxx::audio::FramePos position, const mixxx::Beats* pBeats);
    void playPosChanged(mixxx::audio::FramePos position);

    ControlPushButton* m_pCOQuantizeEnabled;
    ControlObject* m_pCONextBeat;
    ControlObject* m_pCOPrevBeat;
    ControlObject* m_pCOClosestBeat;

    mixxx::ImmutableAudioSnapshot<mixxx::Beats> m_beatsSnapshot;
    std::atomic<bool> m_beatsChanged{true};
};
