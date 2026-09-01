#pragma once

#include <vector>

#include <QMainWindow>

#include "eq_curve_widget.h"
#include "model/app_state.h"

class QComboBox;
class QPushButton;
class QCheckBox;
class QSlider;
class QLabel;
class QSpinBox;
class QTableWidget;
class QTabWidget;

namespace pipeeq {

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
    void onGainSliderChanged(int value);
    void onGainSliderPressed();
    void onGainSliderReleased();
    void onMuteToggled(bool checked);
    void onAutoConnectToggled(bool checked);
    void onChannelPositionChanged(int index);
    void onBandCountChanged(int count);
    void onCurveBandEdited(int index, eqcore::EqBand band);
    void onCurveBandEditBegan(int index);
    void onCurveBandEditFinished(int index);
    void onCopyEqClicked();
    void onAddInputClicked();
    void onRemoveInputClicked();
    void onTopologyChanged();
    void onStripsUpdated();
    void onChannelDetailUpdated(const QString& outputId, uint32_t channelIndex);
    void onErrorReported(const QString& message);
    void refreshStrips();
    void refreshDevices();
    void refreshInputs();

private:
    void selectStrip(const QString& stripId);
    void loadStripDetail(const StripRow& strip);
    // Refreshes only what can change without the strip set changing (labels,
    // connected state, gain/mute/auto-connect), leaving the band and mixer
    // tables alone - rebuilding those on a refresh would yank widgets out from
    // under a slider the user is dragging.
    void updateStripStatus();
    // Fills the position dropdown with the channel positions the selected
    // strip's device advertises, and selects the one it currently claims.
    void rebuildPositionCombo(const StripRow& strip);
    // True when the strip's output is connected but this particular channel
    // isn't driven - a channel the device's current profile doesn't offer.
    bool channelUnavailable(const StripRow& strip) const;
    void rebuildBandTable(const std::vector<eqcore::EqBand>& bands);
    void rebuildMixerTable();
    void pushBandRow(int row);
    void pushMixerRow(int row);
    const StripRow* findStrip(const QString& stripId) const;
    // The device row for a strip's target, or null when it isn't present.
    const DeviceRow* findDevice(const QString& nodeName) const;

    AppState* state_;

    StripRack* stripRack_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;

    QCheckBox* muteCheck_ = nullptr;
    QCheckBox* autoConnectCheck_ = nullptr;
    QComboBox* positionCombo_ = nullptr;
    QLabel* positionLabel_ = nullptr;
    QSlider* gainSlider_ = nullptr;
    QLabel* gainLabel_ = nullptr;
    QTabWidget* detailTabs_ = nullptr;

    // EQ tab.
    QSpinBox* bandCountSpin_ = nullptr;
    QPushButton* copyEqButton_ = nullptr;
    QTableWidget* bandTable_ = nullptr;
    EqCurveWidget* curveWidget_ = nullptr;

    // Mixer tab: one row per known input, each with an on/off checkbox and a
    // level slider scoped to whichever channel is currently selected.
    QPushButton* addInputButton_ = nullptr;
    QPushButton* removeInputButton_ = nullptr;
    QTableWidget* mixerTable_ = nullptr;

    QLabel* statusLabel_ = nullptr;

    QString currentStripId_;
    // Still needed for programmatic widget updates - a QSignalBlocker in all
    // but name. It no longer has anything to do with reconciling daemon values;
    // that is EditGuard's job in the store now.
    bool suppressSignals_ = false;
};

} // namespace pipeeq
