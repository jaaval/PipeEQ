#pragma once

#include <vector>

#include <QWidget>

#include "eq_band.h"
#include "eq_response.h"

namespace pipeeq {

// Draws the combined frequency response of a set of EqBands on a log-frequency
// axis, with draggable control points.
//
// Uses eqcore::eqResponseCurveDb - the same math the daemon applies - so the
// curve is always the curve actually being computed for those bands. That is the
// widget's entire reason to exist, and why a charting library would be a
// regression rather than a shortcut.
class EqCurveWidget : public QWidget {
    Q_OBJECT

public:
    explicit EqCurveWidget(QWidget* parent = nullptr);

    void setBands(const std::vector<eqcore::EqBand>& bands);
    const std::vector<eqcore::EqBand>& bands() const { return bands_; }

    // Preview mode: compact, no grid labels, no drag handles, and clicks are
    // passed through to the parent rather than editing. At preview size the
    // handles would be a few pixels across, so an editable preview mostly
    // produces accidental edits.
    void setPreviewMode(bool preview);
    bool isPreviewMode() const { return preview_; }

    // The rate the daemon actually negotiated. The curve MUST be drawn at the
    // same rate the coefficients are computed for, or it shows a response that
    // isn't the one being applied.
    void setSampleRateHz(double sampleRateHz);

    // Greys everything out and stops accepting edits, for a bypassed EQ.
    void setBypassed(bool bypassed);

    int selectedBand() const { return selectedIndex_; }
    void setSelectedBand(int index);

signals:
    // Emitted while the user drags a point or scrolls on it. The caller pushes
    // this to the store, which coalesces the writes.
    void bandEdited(int index, eqcore::EqBand band);
    // Bracket the drag, so the store can hold off daemon values for that band
    // and flush the final value on release.
    void bandEditBegan(int index);
    void bandEditFinished(int index);

    void selectedBandChanged(int index);
    // Double-click on empty plot area: add a band centred there.
    void bandAddRequested(double freqHz, double gainDb);
    void bandRemoveRequested(int index);
    void bandTypeChangeRequested(int index, eqcore::FilterType type);

    // Preview mode only: the widget was clicked.
    void activated();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    QRectF plotRect() const;
    double freqToX(double freqHz) const;
    double xToFreq(double x) const;
    double gainToY(double gainDb) const;
    double yToGain(double y) const;
    int hitTestBand(QPointF pos) const;
    void drawHandle(QPainter& painter, const eqcore::EqBand& band, int index, bool selected) const;

    static constexpr double kMinFreqHz = 20.0;
    static constexpr double kMaxFreqHz = 20000.0;
    static constexpr double kHitRadiusPx = 10.0;
    static constexpr double kDefaultSampleRateHz = 48000.0;

    std::vector<eqcore::EqBand> bands_;
    int draggingIndex_ = -1;
    int selectedIndex_ = -1;
    bool preview_ = false;
    bool bypassed_ = false;
    // Preview gets a tighter vertical range: most curves are within +/-18 dB,
    // and at preview height the extra headroom just flattens everything.
    double maxGainDb_ = 24.0;
    double sampleRateHz_ = kDefaultSampleRateHz;
};

} // namespace pipeeq
