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

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onAddRouteClicked();
    void onRemoveRouteClicked();
    void onRouteSelectionChanged();
    void onGainSliderChanged(int value);
    void onMuteToggled(bool checked);
    void onAutoConnectToggled(bool checked);
    void onChannelPairChanged(int index);
    void onBandCountChanged(int count);
    void onCurveBandEdited(int index, eqcore::EqBand band);
    void onCopyEqClicked();
    void onAddInputClicked();
    void onRemoveInputClicked();
    void onDaemonRouteChanged(const QString& routeId);
    void onDaemonInputsChanged();
    void refreshRoutes();
    void refreshDevices();
    void refreshInputs();

private:
    void selectRoute(const QString& routeId);
    void loadRouteDetail(const RouteRow& route);
    // Refreshes only what can change without the route set changing (labels,
    // connected state, gain/mute/auto-connect), leaving the band and mixer
    // tables alone - rebuilding those on a periodic poll would yank widgets
    // out from under a slider the user is dragging.
    void updateRouteStatus();
    // Sets one output list row's text/tooltip/color from its connected and
    // auto-connect state. The label is kept short because the list is narrow;
    // the tooltip carries the detail.
    void applyRouteItem(QListWidgetItem* item, const RouteRow& route) const;
    // Fills the channel-pair dropdown with the pairs the selected output's
    // device offers, and selects the one it currently drives.
    void rebuildChannelCombo(const RouteRow& route);
    // How many entries the device list has for this node.name - used to decide
    // whether a device needs its pair spelled out in the label at all.
    int pairCountForDevice(const QString& nodeName) const;
    // True if the route's device is present but no longer offers its pair,
    // which is why it can be disconnected while the device is plugged in.
    bool channelsUnavailable(const RouteRow& route) const;
    void rebuildBandTable(const std::vector<eqcore::EqBand>& bands);
    void rebuildMixerTable();
    void pushBandRow(int row);
    void pushMixerRow(int row);
    const RouteRow* findRoute(const QString& routeId) const;

    DbusClient* dbus_;

    QListWidget* routeList_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;

    QCheckBox* muteCheck_ = nullptr;
    QCheckBox* autoConnectCheck_ = nullptr;
    QComboBox* channelCombo_ = nullptr;
    QLabel* channelLabel_ = nullptr;
    QSlider* gainSlider_ = nullptr;
    QLabel* gainLabel_ = nullptr;
    QTabWidget* detailTabs_ = nullptr;

    // EQ tab.
    QSpinBox* bandCountSpin_ = nullptr;
    QPushButton* copyEqButton_ = nullptr;
    QTableWidget* bandTable_ = nullptr;
    EqCurveWidget* curveWidget_ = nullptr;

    // Mixer tab: one row per known input, each with an on/off checkbox and
    // a level slider scoped to whichever route is currently selected.
    QPushButton* addInputButton_ = nullptr;
    QPushButton* removeInputButton_ = nullptr;
    QTableWidget* mixerTable_ = nullptr;

    QLabel* statusLabel_ = nullptr;

    std::vector<RouteRow> routes_;
    std::vector<DeviceRow> devices_;
    std::vector<InputRow> inputs_;
    QString currentRouteId_;
    bool suppressSignals_ = false;
};

} // namespace pipeeq
