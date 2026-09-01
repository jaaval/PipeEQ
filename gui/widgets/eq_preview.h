#pragma once

#include <QWidget>

#include "eq_band.h"

class QLabel;

namespace pipeeq {

class EqCurveWidget;

// A small, non-interactive EQ curve with a caption, which opens the full editor
// when clicked.
//
// Non-interactive on purpose: at this size the drag targets would be a few
// pixels across, so an editable preview would mostly produce accidental edits.
// One click to the real editor is both more honest and less frustrating.
class EqPreview : public QWidget {
    Q_OBJECT

public:
    explicit EqPreview(QWidget* parent = nullptr);

    void setBands(const std::vector<eqcore::EqBand>& bands);
    // e.g. "Mains · shared by 2 ch", or "no EQ on this channel".
    void setCaption(const QString& caption);
    void setSampleRateHz(double sampleRateHz);

signals:
    void activated();

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    EqCurveWidget* curve_ = nullptr;
    QLabel* caption_ = nullptr;
};

} // namespace pipeeq
