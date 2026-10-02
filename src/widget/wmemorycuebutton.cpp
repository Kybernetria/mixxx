#include "widget/wmemorycuebutton.h"

#include "control/controlobject.h"
#include "mixer/playerinfo.h"
#include "moc_wmemorycuebutton.cpp"
#include "skin/legacy/skincontext.h"
#include "track/memorycues.h"

WMemoryCueButton::WMemoryCueButton(QWidget* parent, const QString& group)
        : WPushButton(parent),
          m_group(group) {
    setFocusPolicy(Qt::NoFocus);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            [this](const QString& group, TrackPointer, TrackPointer) {
                if (group == m_group && m_popup)
                    m_popup->hide();
            });
}

void WMemoryCueButton::setup(const QDomNode& node, const SkinContext& context) {
    WPushButton::setup(node, context);
    m_popup = make_parented<WCueMenuPopup>(context.getConfig(), this);
}

void WMemoryCueButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !m_popup)
        return;
    const auto track = PlayerInfo::instance().getTrackInfo(m_group);
    if (!track)
        return;
    // playposition/track_samples are the established stereo engine-sample UI
    // controls, even for stems. Convert once to source frames.
    const auto position = mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
            ControlObject::get(ConfigKey(m_group, "playposition")) *
            ControlObject::get(ConfigKey(m_group, "track_samples")));
    const auto cue = MemoryCues::nearest(*track, position, track->getSampleRate());
    if (!cue)
        return;
    m_popup->setTrackCueGroup(track, cue, m_group);
    m_popup->popup(mapToGlobal(QPoint(0, height())));
}
