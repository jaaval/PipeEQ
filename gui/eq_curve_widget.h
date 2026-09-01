#pragma once

#include <vector>

#include <QWidget>

#include "eq_band.h"
#include "eq_response.h"

namespace pipeeq {

// Draws the combined frequency response of a set of EqBands on a log-frequency
// x-axis, with draggable control points (drag = freq/gain, wheel = Q). Uses
// eqcore::eqResponseCurveDb directly - the same math the daemon applies - so
// the curve always matches what is actually being computed for those bands.
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

signals:
    // Emitted while the user drags a point or scrolls on it. The caller is
    // expected to push this to the daemon and update any other UI (e.g. a
    // numeric band table) that mirrors band state.
    void bandEdited(int index, eqcore::EqBand band);
    // Bracket the drag, so the store can hold off daemon values for that band
    // and flush the final value on release.
    void bandEditBegan(int index);
    void bandEditFinished(int index);
    // Preview mode only: the widget was clicked.
    void activated();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    QRectF plotRect() const;
    double freqToX(double freqHz) const;
    double xToFreq(double x) const;
    double gainToY(double gainDb) const;
    double yToGain(double y) const;
    int hitTestBand(QPointF pos) const;

    static constexpr double kMinFreqHz = 20.0;
    static constexpr double kMaxFreqHz = 20000.0;
    static constexpr double kHitRadiusPx = 10.0;
    static constexpr double kDefaultSampleRateHz = 48000.0;

    std::vector<eqcore::EqBand> bands_;
    int draggingIndex_ = -1;
    bool preview_ = false;
    // Preview gets a tighter vertical range: most curves are within +/-18 dB,
    // and at 48 px tall the extra headroom just flattens everything.
    double maxGainDb_ = 24.0;
    double sampleRateHz_ = kDefaultSampleRateHz;
};

} // namespace pipeeq
