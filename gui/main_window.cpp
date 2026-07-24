#include "main_window.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace pipeeq {

namespace {

struct FilterTypeOption {
    const char* label;
    const char* wire;
};

// Order must match eqcore::FilterType's enum values exactly - the combo
// box's currentIndex() is used directly as the FilterType value.
constexpr FilterTypeOption kFilterTypeOptions[] = {
    {"Peaking", "peaking"},
    {"Low Shelf", "low_shelf"},
    {"High Shelf", "high_shelf"},
    {"Low Pass", "low_pass"},
    {"High Pass", "high_pass"},
};

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), dbus_(new DbusClient(this)) {
    setWindowTitle("PipeEQ");
    resize(950, 620);

    auto* central = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);

    auto* leftLayout = new QVBoxLayout();
    leftLayout->addWidget(new QLabel("Outputs"));
    routeList_ = new QListWidget();
    leftLayout->addWidget(routeList_, 1);

    auto* addRow = new QHBoxLayout();
    deviceCombo_ = new QComboBox();
    addButton_ = new QPushButton("Add");
    addRow->addWidget(deviceCombo_, 1);
    addRow->addWidget(addButton_);
    leftLayout->addLayout(addRow);

    removeButton_ = new QPushButton("Remove selected output");
    leftLayout->addWidget(removeButton_);

    auto* leftContainer = new QWidget();
    leftContainer->setLayout(leftLayout);
    leftContainer->setMaximumWidth(260);
    rootLayout->addWidget(leftContainer);

    auto* rightLayout = new QVBoxLayout();

    auto* controlsRow = new QHBoxLayout();
    muteCheck_ = new QCheckBox("Mute");
    gainSlider_ = new QSlider(Qt::Horizontal);
    gainSlider_->setRange(-60, 12);
    gainLabel_ = new QLabel("0 dB");
    gainLabel_->setMinimumWidth(60);
    bandCountSpin_ = new QSpinBox();
    bandCountSpin_->setRange(0, 16);

    controlsRow->addWidget(muteCheck_);
    controlsRow->addWidget(new QLabel("Gain:"));
    controlsRow->addWidget(gainSlider_, 1);
    controlsRow->addWidget(gainLabel_);
    controlsRow->addWidget(new QLabel("Bands:"));
    controlsRow->addWidget(bandCountSpin_);
    rightLayout->addLayout(controlsRow);

    bandTable_ = new QTableWidget(0, 4);
    bandTable_->setHorizontalHeaderLabels({"Type", "Freq (Hz)", "Gain (dB)", "Q"});
    bandTable_->horizontalHeader()->setStretchLastSection(true);
    bandTable_->setMaximumHeight(200);
    rightLayout->addWidget(bandTable_);

    curveWidget_ = new EqCurveWidget();
    rightLayout->addWidget(curveWidget_, 1);

    auto* rightContainer = new QWidget();
    rightContainer->setLayout(rightLayout);
    rootLayout->addWidget(rightContainer, 1);

    setCentralWidget(central);

    statusLabel_ = new QLabel();
    statusBar()->addWidget(statusLabel_);

    connect(routeList_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) { onRouteSelectionChanged(); });
    connect(addButton_, &QPushButton::clicked, this, &MainWindow::onAddRouteClicked);
    connect(removeButton_, &QPushButton::clicked, this, &MainWindow::onRemoveRouteClicked);
    connect(muteCheck_, &QCheckBox::toggled, this, &MainWindow::onMuteToggled);
    connect(gainSlider_, &QSlider::valueChanged, this, &MainWindow::onGainSliderChanged);
    connect(bandCountSpin_, &QSpinBox::valueChanged, this, &MainWindow::onBandCountChanged);
    connect(curveWidget_, &EqCurveWidget::bandEdited, this, &MainWindow::onCurveBandEdited);
    connect(dbus_, &DbusClient::routeChanged, this, &MainWindow::onDaemonRouteChanged);

    refreshDevices();
    refreshRoutes();

    auto* pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, &MainWindow::refreshDevices);
    pollTimer->start(3000);
}

void MainWindow::onAddRouteClicked() {
    const int idx = deviceCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(devices_.size())) {
        return;
    }
    const DeviceRow& device = devices_[static_cast<std::size_t>(idx)];
    const QString routeId = dbus_->addRoute(device.nodeName, device.description);
    if (routeId.isEmpty()) {
        statusLabel_->setText("Failed to add route - is pipeeq-daemon running?");
        return;
    }
    refreshRoutes();
    selectRoute(routeId);
}

void MainWindow::onRemoveRouteClicked() {
    if (currentRouteId_.isEmpty()) {
        return;
    }
    dbus_->removeRoute(currentRouteId_);
    currentRouteId_.clear();
    refreshRoutes();
}

void MainWindow::onRouteSelectionChanged() {
    QListWidgetItem* item = routeList_->currentItem();
    if (!item) {
        currentRouteId_.clear();
        bandTable_->setRowCount(0);
        curveWidget_->setBands({});
        return;
    }
    currentRouteId_ = item->data(Qt::UserRole).toString();
    if (const RouteRow* route = findRoute(currentRouteId_)) {
        loadRouteDetail(*route);
    }
}

void MainWindow::onGainSliderChanged(int value) {
    if (suppressSignals_ || currentRouteId_.isEmpty()) {
        return;
    }
    gainLabel_->setText(QString::number(value) + " dB");
    dbus_->setRouteGain(currentRouteId_, value);
}

void MainWindow::onMuteToggled(bool checked) {
    if (suppressSignals_ || currentRouteId_.isEmpty()) {
        return;
    }
    dbus_->setRouteMute(currentRouteId_, checked);
}

void MainWindow::onBandCountChanged(int count) {
    if (suppressSignals_ || currentRouteId_.isEmpty()) {
        return;
    }
    dbus_->setRouteBandCount(currentRouteId_, static_cast<uint32_t>(count));
    const auto bands = dbus_->getRouteBands(currentRouteId_);
    rebuildBandTable(bands);
    curveWidget_->setBands(bands);
}

void MainWindow::onCurveBandEdited(int index, eqcore::EqBand band) {
    if (currentRouteId_.isEmpty() || index < 0 || index >= bandTable_->rowCount()) {
        return;
    }

    suppressSignals_ = true;
    if (auto* freqSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(index, 1))) {
        freqSpin->setValue(band.freqHz);
    }
    if (auto* gainSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(index, 2))) {
        gainSpin->setValue(band.gainDb);
    }
    if (auto* qSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(index, 3))) {
        qSpin->setValue(band.q);
    }
    suppressSignals_ = false;

    auto* typeCombo = qobject_cast<QComboBox*>(bandTable_->cellWidget(index, 0));
    const QString wire = typeCombo ? kFilterTypeOptions[typeCombo->currentIndex()].wire : "peaking";
    dbus_->setRouteBand(currentRouteId_, static_cast<uint32_t>(index), wire, band.freqHz, band.gainDb, band.q);
}

void MainWindow::onDaemonRouteChanged(const QString& /*routeId*/) {
    refreshRoutes();
}

void MainWindow::refreshRoutes() {
    routes_ = dbus_->listRoutes();
    const QString previous = currentRouteId_;

    routeList_->clear();
    QListWidgetItem* toSelect = nullptr;
    for (const auto& r : routes_) {
        auto* item = new QListWidgetItem(r.displayName.isEmpty() ? r.deviceName : r.displayName);
        item->setData(Qt::UserRole, r.id);
        routeList_->addItem(item);
        if (r.id == previous) {
            toSelect = item;
        }
    }

    if (toSelect) {
        routeList_->setCurrentItem(toSelect);
    } else {
        currentRouteId_.clear();
    }
}

void MainWindow::refreshDevices() {
    devices_ = dbus_->listDevices();

    deviceCombo_->clear();
    for (const auto& d : devices_) {
        deviceCombo_->addItem(d.description.isEmpty() ? d.nodeName : d.description);
    }

    statusLabel_->setText(devices_.empty() ? "No output devices found (or pipeeq-daemon isn't running)"
                                            : QString("%1 output device(s) available").arg(devices_.size()));
}

void MainWindow::selectRoute(const QString& routeId) {
    for (int i = 0; i < routeList_->count(); ++i) {
        QListWidgetItem* item = routeList_->item(i);
        if (item->data(Qt::UserRole).toString() == routeId) {
            routeList_->setCurrentItem(item);
            return;
        }
    }
}

void MainWindow::loadRouteDetail(const RouteRow& route) {
    suppressSignals_ = true;
    muteCheck_->setChecked(route.muted);
    gainSlider_->setValue(static_cast<int>(std::lround(route.gainDb)));
    gainLabel_->setText(QString::number(route.gainDb, 'f', 1) + " dB");
    bandCountSpin_->setValue(static_cast<int>(route.bandCount));
    suppressSignals_ = false;

    const auto bands = dbus_->getRouteBands(route.id);
    rebuildBandTable(bands);
    curveWidget_->setBands(bands);
}

void MainWindow::rebuildBandTable(const std::vector<eqcore::EqBand>& bands) {
    suppressSignals_ = true;
    bandTable_->setRowCount(static_cast<int>(bands.size()));

    for (int row = 0; row < static_cast<int>(bands.size()); ++row) {
        const eqcore::EqBand& band = bands[static_cast<std::size_t>(row)];

        auto* typeCombo = new QComboBox();
        for (const auto& opt : kFilterTypeOptions) {
            typeCombo->addItem(opt.label);
        }
        typeCombo->setCurrentIndex(static_cast<int>(band.type));
        connect(typeCombo, &QComboBox::currentIndexChanged, this, [this, row](int) { pushBandRow(row); });
        bandTable_->setCellWidget(row, 0, typeCombo);

        auto* freqSpin = new QDoubleSpinBox();
        freqSpin->setRange(20.0, 20000.0);
        freqSpin->setDecimals(0);
        freqSpin->setValue(band.freqHz);
        connect(freqSpin, &QDoubleSpinBox::valueChanged, this, [this, row](double) { pushBandRow(row); });
        bandTable_->setCellWidget(row, 1, freqSpin);

        auto* gainSpin = new QDoubleSpinBox();
        gainSpin->setRange(-24.0, 24.0);
        gainSpin->setDecimals(1);
        gainSpin->setValue(band.gainDb);
        connect(gainSpin, &QDoubleSpinBox::valueChanged, this, [this, row](double) { pushBandRow(row); });
        bandTable_->setCellWidget(row, 2, gainSpin);

        auto* qSpin = new QDoubleSpinBox();
        qSpin->setRange(0.1, 10.0);
        qSpin->setDecimals(2);
        qSpin->setSingleStep(0.05);
        qSpin->setValue(band.q);
        connect(qSpin, &QDoubleSpinBox::valueChanged, this, [this, row](double) { pushBandRow(row); });
        bandTable_->setCellWidget(row, 3, qSpin);
    }

    suppressSignals_ = false;
}

void MainWindow::pushBandRow(int row) {
    if (suppressSignals_ || currentRouteId_.isEmpty()) {
        return;
    }

    auto* typeCombo = qobject_cast<QComboBox*>(bandTable_->cellWidget(row, 0));
    auto* freqSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(row, 1));
    auto* gainSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(row, 2));
    auto* qSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(row, 3));
    if (!typeCombo || !freqSpin || !gainSpin || !qSpin) {
        return;
    }

    const QString wire = kFilterTypeOptions[typeCombo->currentIndex()].wire;
    dbus_->setRouteBand(currentRouteId_, static_cast<uint32_t>(row), wire, freqSpin->value(), gainSpin->value(),
                        qSpin->value());

    eqcore::EqBand band;
    band.type = static_cast<eqcore::FilterType>(typeCombo->currentIndex());
    band.freqHz = freqSpin->value();
    band.gainDb = gainSpin->value();
    band.q = qSpin->value();

    std::vector<eqcore::EqBand> bands = curveWidget_->bands();
    if (row >= 0 && row < static_cast<int>(bands.size())) {
        bands[static_cast<std::size_t>(row)] = band;
        curveWidget_->setBands(bands);
    }
}

const RouteRow* MainWindow::findRoute(const QString& routeId) const {
    for (const auto& r : routes_) {
        if (r.id == routeId) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace pipeeq
