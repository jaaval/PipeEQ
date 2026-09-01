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

signals:
    // Emitted while the user drags a point or scrolls on it. The caller is
    // expected to push this to the daemon and update any other UI (e.g. a
    // numeric band table) that mirrors band state.
    void bandEdited(int index, eqcore::EqBand band);

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
    static constexpr double kMaxGainDb = 24.0;
    static constexpr double kSampleRateHz = 48000.0;
    static constexpr double kHitRadiusPx = 10.0;

    std::vector<eqcore::EqBand> bands_;
    int draggingIndex_ = -1;
};

} // namespace pipeeq
