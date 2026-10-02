#pragma once

#include "util/parented_ptr.h"
#include "widget/wcuemenupopup.h"
#include "widget/wpushbutton.h"

/// Opens label/color/delete actions for the nearest memory cue (one second).
class WMemoryCueButton final : public WPushButton {
    Q_OBJECT
  public:
    WMemoryCueButton(QWidget* parent, const QString& group);
    void setup(const QDomNode& node, const SkinContext& context) override;

  protected:
    void mousePressEvent(QMouseEvent* event) override;

  private:
    const QString m_group;
    parented_ptr<WCueMenuPopup> m_popup;
};
