#pragma once

#include <vector>

#include <QMainWindow>

#include "dbus_client.h"
#include "eq_curve_widget.h"

class QListWidget;
class QComboBox;
class QPushButton;
class QCheckBox;
class QSlider;
class QLabel;
class QSpinBox;
class QTableWidget;

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
    void onBandCountChanged(int count);
    void onCurveBandEdited(int index, eqcore::EqBand band);
    void onDaemonRouteChanged(const QString& routeId);
    void refreshRoutes();
    void refreshDevices();

private:
    void selectRoute(const QString& routeId);
    void loadRouteDetail(const RouteRow& route);
    void rebuildBandTable(const std::vector<eqcore::EqBand>& bands);
    void pushBandRow(int row);
    const RouteRow* findRoute(const QString& routeId) const;

    DbusClient* dbus_;

    QListWidget* routeList_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;

    QCheckBox* muteCheck_ = nullptr;
    QSlider* gainSlider_ = nullptr;
    QLabel* gainLabel_ = nullptr;
    QSpinBox* bandCountSpin_ = nullptr;
    QTableWidget* bandTable_ = nullptr;
    EqCurveWidget* curveWidget_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    std::vector<RouteRow> routes_;
    std::vector<DeviceRow> devices_;
    QString currentRouteId_;
    bool suppressSignals_ = false;
};

} // namespace pipeeq
