#pragma once

#include <vector>

#include <QMainWindow>

#include "dbus_client.h"
#include "eq_curve_widget.h"

class QListWidget;
class QListWidgetItem;
class QComboBox;
class QPushButton;
class QCheckBox;
class QSlider;
class QLabel;
class QSpinBox;
class QTableWidget;
class QTabWidget;

namespace pipeeq {

// Transitional window: the same layout as before, but a row in the left-hand
// list is now one hardware output CHANNEL rather than one stereo-pair output.
// This exists so the daemon rework could land with a working GUI on top of it;
// the TotalMix-style rack replaces it wholesale.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onAddOutputClicked();
    void onRemoveOutputClicked();
    void onStripSelectionChanged();
    void onGainSliderChanged(int value);
    void onMuteToggled(bool checked);
    void onAutoConnectToggled(bool checked);
    void onChannelPositionChanged(int index);
    void onBandCountChanged(int count);
    void onCurveBandEdited(int index, eqcore::EqBand band);
    void onCopyEqClicked();
    void onAddInputClicked();
    void onRemoveInputClicked();
    void onDaemonOutputChanged(const QString& outputId);
    void onDaemonInputsChanged();
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
    // Sets one list row's text/tooltip/color from its connected and
    // auto-connect state. The label is kept short because the list is narrow;
    // the tooltip carries the detail.
    void applyStripItem(QListWidgetItem* item, const StripRow& strip) const;
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

    DbusClient* dbus_;

    QListWidget* stripList_ = nullptr;
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

    std::vector<StripRow> strips_;
    std::vector<DeviceRow> devices_;
    std::vector<InputRow> inputs_;
    QString currentStripId_;
    bool suppressSignals_ = false;
};

} // namespace pipeeq
