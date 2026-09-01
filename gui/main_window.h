#pragma once

#include <vector>

#include <QMainWindow>

#include "eq_curve_widget.h"
#include "model/app_state.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace pipeeq {

class DetailPanel;
class StripRack;

// Transitional window: the same layout as before, but a row in the left-hand
// list is now one hardware output CHANNEL rather than one stereo-pair output.
// This exists so the daemon rework could land with a working GUI on top of it;
// the TotalMix-style rack replaces it wholesale.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Reads from the store rather than calling a backend directly, so nothing
    // it does can block on the daemon and every write goes through the
    // coalescer. Ownership of the store stays with the caller.
    explicit MainWindow(AppState* state, QWidget* parent = nullptr);

private slots:
    void onAddOutputClicked();
    void onRemoveOutputClicked();
    void onRackSelectionChanged(const QString& stripId);
    void onBandCountChanged(int count);
    void onCurveBandEdited(int index, eqcore::EqBand band);
    void onCurveBandEditBegan(int index);
    void onCurveBandEditFinished(int index);
    void onCopyEqClicked();
    void onAddInputClicked();
    void onTopologyChanged();
    void onStripsUpdated();
    void onChannelDetailUpdated(const QString& outputId, uint32_t channelIndex);
    void onErrorReported(const QString& message);
    void refreshStrips();
    void refreshDevices();
    void refreshInputs();

private:
    void applyDetailSizing(int pageIndex);
    void selectStrip(const QString& stripId);
    void loadStripDetail(const StripRow& strip);
    // Refreshes only what can change without the strip set changing (labels,
    // connected state, gain/mute/auto-connect), leaving the band and mixer
    // tables alone - rebuilding those on a refresh would yank widgets out from
    // under a slider the user is dragging.
    void updateStripStatus();
    void rebuildBandTable(const std::vector<eqcore::EqBand>& bands);
    void pushBandRow(int row);
    const StripRow* findStrip(const QString& stripId) const;
    // The device row for a strip's target, or null when it isn't present.
    const DeviceRow* findDevice(const QString& nodeName) const;

    AppState* state_;

    StripRack* stripRack_ = nullptr;
    DetailPanel* detailPanel_ = nullptr;
    QStackedWidget* detailStack_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;

    // The EQ editor page. Replaced wholesale by the real editor, with its
    // instance list and channel-assignment matrix, in the next phase.
    QSpinBox* bandCountSpin_ = nullptr;
    QPushButton* copyEqButton_ = nullptr;
    QTableWidget* bandTable_ = nullptr;
    EqCurveWidget* curveWidget_ = nullptr;

    QLabel* statusLabel_ = nullptr;

    QString currentStripId_;
    // Still needed for programmatic widget updates - a QSignalBlocker in all
    // but name. It no longer has anything to do with reconciling daemon values;
    // that is EditGuard's job in the store now.
    bool suppressSignals_ = false;
};

} // namespace pipeeq
