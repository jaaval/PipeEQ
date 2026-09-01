#pragma once

#include <QVector>
#include <QWidget>

#include "eq_band.h"

class QCheckBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QPushButton;

namespace pipeeq {

class AppState;
class EqCurveWidget;

// The full EQ editor for one channel's curve.
//
// There is deliberately no instance-to-channel assignment UI. Which channels
// share a curve is decided by LINKING them - linked channels share one instance
// automatically - and getting the same curve somewhere else is a copy. A grid of
// instances against channels was the original plan and was cut: it is powerful,
// but for what is nearly always a two-channel decision it asks the user to hold
// a concept they don't otherwise need.
class EqEditor : public QWidget {
    Q_OBJECT

public:
    explicit EqEditor(AppState* state, QWidget* parent = nullptr);

    void setSelection(const QString& stripId);
    // Pulls the current bands out of the store and redraws.
    void refresh();

signals:
    void backRequested();

private:
    void rebuildRibbon();
    void updateBandControls();
    void pushSelectedBand();
    void addBand(double freqHz, double gainDb);
    void removeBand(int index);
    void showCopyDialog();

    AppState* state_;
    QString stripId_;
    QVector<eqcore::EqBand> bands_;
    int selected_ = -1;

    QLabel* title_ = nullptr;
    QLabel* sharedNote_ = nullptr;
    QLabel* bandCountLabel_ = nullptr;
    QPushButton* copyButton_ = nullptr;
    EqCurveWidget* curve_ = nullptr;

    QHBoxLayout* ribbonLayout_ = nullptr;
    QVector<QPushButton*> ribbonChips_;
    QPushButton* addBandButton_ = nullptr;

    QLabel* bandTitle_ = nullptr;
    QVector<QPushButton*> typeButtons_;
    QDoubleSpinBox* freqSpin_ = nullptr;
    QDoubleSpinBox* gainSpin_ = nullptr;
    QDoubleSpinBox* qSpin_ = nullptr;
    QPushButton* removeBandButton_ = nullptr;

    bool suppressSignals_ = false;
};

} // namespace pipeeq
