#include "main_window.h"

#include <cmath>

#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
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
    autoConnectCheck_ = new QCheckBox("Auto-connect");
    autoConnectCheck_->setToolTip(
        "Connect this output as soon as its device is available - at startup if it's already there, or "
        "later whenever it appears (e.g. when you power on or plug in the device).");
    gainSlider_ = new QSlider(Qt::Horizontal);
    gainSlider_->setRange(-60, 12);
    gainLabel_ = new QLabel("0 dB");
    gainLabel_->setMinimumWidth(60);

    channelLabel_ = new QLabel("Channels:");
    channelCombo_ = new QComboBox();
    // Let it size to its contents: clipped to its default width, "Front L/R"
    // renders as "Front L/F", which reads as a typo rather than a truncation.
    channelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    channelCombo_->setToolTip(
        "Which stereo pair of the device this output drives. A 4-channel interface can host one output on "
        "outputs 1/2 and another on 3/4, each with its own EQ.");

    controlsRow->addWidget(muteCheck_);
    controlsRow->addWidget(autoConnectCheck_);
    controlsRow->addWidget(channelLabel_);
    controlsRow->addWidget(channelCombo_);
    controlsRow->addWidget(new QLabel("Gain:"));
    controlsRow->addWidget(gainSlider_, 1);
    controlsRow->addWidget(gainLabel_);
    rightLayout->addLayout(controlsRow);

    detailTabs_ = new QTabWidget();

    // --- EQ tab ---
    auto* eqTab = new QWidget();
    auto* eqLayout = new QVBoxLayout(eqTab);

    auto* eqControlsRow = new QHBoxLayout();
    bandCountSpin_ = new QSpinBox();
    bandCountSpin_->setRange(0, 16);
    copyEqButton_ = new QPushButton("Copy EQ to...");
    eqControlsRow->addWidget(new QLabel("Bands:"));
    eqControlsRow->addWidget(bandCountSpin_);
    eqControlsRow->addStretch(1);
    eqControlsRow->addWidget(copyEqButton_);
    eqLayout->addLayout(eqControlsRow);

    bandTable_ = new QTableWidget(0, 4);
    bandTable_->setHorizontalHeaderLabels({"Type", "Freq (Hz)", "Gain (dB)", "Q"});
    bandTable_->horizontalHeader()->setStretchLastSection(true);
    bandTable_->setMaximumHeight(200);
    eqLayout->addWidget(bandTable_);

    curveWidget_ = new EqCurveWidget();
    eqLayout->addWidget(curveWidget_, 1);

    detailTabs_->addTab(eqTab, "EQ");

    // --- Mixer tab ---
    auto* mixerTab = new QWidget();
    auto* mixerLayout = new QVBoxLayout(mixerTab);

    auto* mixerControlsRow = new QHBoxLayout();
    addInputButton_ = new QPushButton("Add Input...");
    removeInputButton_ = new QPushButton("Remove Selected Input");
    mixerControlsRow->addWidget(addInputButton_);
    mixerControlsRow->addWidget(removeInputButton_);
    mixerControlsRow->addStretch(1);
    mixerLayout->addLayout(mixerControlsRow);

    mixerTable_ = new QTableWidget(0, 3);
    mixerTable_->setHorizontalHeaderLabels({"Input", "On", "Level (dB)"});
    mixerTable_->horizontalHeader()->setStretchLastSection(true);
    mixerTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    mixerLayout->addWidget(mixerTable_, 1);

    detailTabs_->addTab(mixerTab, "Mixer");

    rightLayout->addWidget(detailTabs_, 1);

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
    connect(autoConnectCheck_, &QCheckBox::toggled, this, &MainWindow::onAutoConnectToggled);
    connect(channelCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onChannelPairChanged);
    connect(gainSlider_, &QSlider::valueChanged, this, &MainWindow::onGainSliderChanged);
    connect(bandCountSpin_, &QSpinBox::valueChanged, this, &MainWindow::onBandCountChanged);
    connect(copyEqButton_, &QPushButton::clicked, this, &MainWindow::onCopyEqClicked);
    connect(curveWidget_, &EqCurveWidget::bandEdited, this, &MainWindow::onCurveBandEdited);
    connect(addInputButton_, &QPushButton::clicked, this, &MainWindow::onAddInputClicked);
    connect(removeInputButton_, &QPushButton::clicked, this, &MainWindow::onRemoveInputClicked);
    connect(dbus_, &DbusClient::routeChanged, this, &MainWindow::onDaemonRouteChanged);
    connect(dbus_, &DbusClient::inputsChanged, this, &MainWindow::onDaemonInputsChanged);

    refreshDevices();
    refreshInputs();
    refreshRoutes();

    // The daemon connects outputs on its own as devices come and go, and it
    // can't signal that from the thread it happens on, so poll for it. This
    // also picks up the device list for the "Add" combo.
    auto* pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, [this] {
        refreshDevices();
        refreshRoutes();
    });
    pollTimer->start(3000);
}

void MainWindow::onAddRouteClicked() {
    const int idx = deviceCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(devices_.size())) {
        return;
    }
    const DeviceRow& device = devices_[static_cast<std::size_t>(idx)];
    // Name the output after the pair when the device offers more than one, so
    // "Scarlett — Front L/R" and "Scarlett — Rear L/R" are tellable apart.
    const QString baseName = device.description.isEmpty() ? device.nodeName : device.description;
    const QString name = (pairCountForDevice(device.nodeName) > 1 && !device.pairLabel.isEmpty())
                              ? baseName + " — " + device.pairLabel
                              : baseName;
    const QString routeId = dbus_->addRoute(device.nodeName, name, device.leftChannel, device.rightChannel);
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
        mixerTable_->setRowCount(0);
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

void MainWindow::onAutoConnectToggled(bool checked) {
    if (suppressSignals_ || currentRouteId_.isEmpty()) {
        return;
    }
    dbus_->setRouteAutoConnect(currentRouteId_, checked);
    refreshRoutes();
}

void MainWindow::rebuildChannelCombo(const RouteRow& route) {
    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;

    channelCombo_->clear();
    for (const auto& d : devices_) {
        if (d.nodeName == route.deviceName) {
            channelCombo_->addItem(d.pairLabel.isEmpty() ? "Device default" : d.pairLabel,
                                    d.leftChannel + '\n' + d.rightChannel);
        }
    }

    const QString current = route.leftChannel + '\n' + route.rightChannel;
    int index = channelCombo_->findData(current);
    if (index < 0) {
        // The device is gone, or its profile no longer offers this pair. Show
        // the configured pair anyway rather than silently snapping the output
        // onto a different one.
        const QString label = route.leftChannel.isEmpty()
                                   ? "Device default"
                                   : QString("%1/%2 (unavailable)").arg(route.leftChannel, route.rightChannel);
        channelCombo_->addItem(label, current);
        index = channelCombo_->count() - 1;
    }
    channelCombo_->setCurrentIndex(index);

    // Nothing to choose between for an ordinary stereo device.
    const bool hasChoice = channelCombo_->count() > 1;
    channelCombo_->setVisible(hasChoice);
    channelLabel_->setVisible(hasChoice);

    suppressSignals_ = wasSuppressed;
}

void MainWindow::onChannelPairChanged(int index) {
    if (suppressSignals_ || currentRouteId_.isEmpty() || index < 0) {
        return;
    }
    const QStringList parts = channelCombo_->itemData(index).toString().split('\n');
    if (parts.size() != 2) {
        return;
    }
    dbus_->setRouteChannels(currentRouteId_, parts[0], parts[1]);
    refreshRoutes();
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

void MainWindow::onCopyEqClicked() {
    if (currentRouteId_.isEmpty()) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Copy EQ to other outputs");
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Apply this output's EQ curve to:"));

    auto* list = new QListWidget(&dialog);
    for (const auto& r : routes_) {
        if (r.id == currentRouteId_) {
            continue;
        }
        auto* item = new QListWidgetItem(r.displayName.isEmpty() ? r.deviceName : r.displayName);
        item->setData(Qt::UserRole, r.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        list->addItem(item);
    }

    if (list->count() == 0) {
        statusLabel_->setText("No other outputs to copy the EQ to.");
        return;
    }

    layout->addWidget(list);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const std::vector<eqcore::EqBand> bands = curveWidget_->bands();
    int appliedCount = 0;
    for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem* item = list->item(i);
        if (item->checkState() != Qt::Checked) {
            continue;
        }
        const QString targetId = item->data(Qt::UserRole).toString();
        dbus_->setRouteBandCount(targetId, static_cast<uint32_t>(bands.size()));
        for (std::size_t b = 0; b < bands.size(); ++b) {
            const eqcore::EqBand& band = bands[b];
            const QString wire = kFilterTypeOptions[static_cast<int>(band.type)].wire;
            dbus_->setRouteBand(targetId, static_cast<uint32_t>(b), wire, band.freqHz, band.gainDb, band.q);
        }
        ++appliedCount;
    }

    if (appliedCount > 0) {
        statusLabel_->setText(QString("Copied EQ to %1 output(s).").arg(appliedCount));
    }
}

void MainWindow::onAddInputClicked() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Add Input", "Name for the new input (e.g. \"Music\"):",
                                                 QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    const QString inputId = dbus_->addInput(name.trimmed());
    if (inputId.isEmpty()) {
        statusLabel_->setText("Failed to add input - is pipeeq-daemon running?");
        return;
    }
    refreshInputs();
}

void MainWindow::onRemoveInputClicked() {
    const int row = mixerTable_->currentRow();
    if (row < 0) {
        return;
    }
    QTableWidgetItem* nameItem = mixerTable_->item(row, 0);
    if (!nameItem) {
        return;
    }
    const QString inputId = nameItem->data(Qt::UserRole).toString();
    dbus_->removeInput(inputId);
    refreshInputs();
}

void MainWindow::onDaemonRouteChanged(const QString& /*routeId*/) {
    refreshRoutes();
}

void MainWindow::onDaemonInputsChanged() {
    refreshInputs();
}

void MainWindow::applyRouteItem(QListWidgetItem* item, const RouteRow& route) const {
    const QString name = route.displayName.isEmpty() ? route.deviceName : route.displayName;

    if (route.connected) {
        item->setText(name);
        item->setToolTip(route.deviceName);
        item->setForeground(QBrush());
        return;
    }

    // Dimmed *and* labelled: the suffix is what actually carries the state,
    // so it still reads without relying on the color.
    if (channelsUnavailable(route)) {
        // The device is right there, so "waiting for device" would be a lie
        // and leave the user with nothing to act on.
        item->setText(name + "  (channels n/a)");
        item->setToolTip(QString("%1\n\nThe device is available but doesn't currently offer channels %2/%3 - "
                                  "its profile may have changed. Pick another pair under Channels.")
                              .arg(route.deviceName, route.leftChannel, route.rightChannel));
        item->setForeground(routeList_->palette().brush(QPalette::Disabled, QPalette::Text));
        return;
    }

    item->setText(name + (route.autoConnect ? "  (waiting)" : "  (off)"));
    item->setToolTip(route.autoConnect
                          ? QString("%1\n\nDevice isn't available; this output will connect as soon as it is.")
                                .arg(route.deviceName)
                          : QString("%1\n\nDevice isn't available and auto-connect is off for this output.")
                                .arg(route.deviceName));
    item->setForeground(routeList_->palette().brush(QPalette::Disabled, QPalette::Text));
}

void MainWindow::refreshRoutes() {
    const std::vector<RouteRow> updated = dbus_->listRoutes();

    bool sameRoutes = updated.size() == routes_.size();
    for (std::size_t i = 0; sameRoutes && i < updated.size(); ++i) {
        sameRoutes = updated[i].id == routes_[i].id;
    }
    routes_ = updated;

    // Rebuilding the list destroys and recreates its items, which changes the
    // current item and so reloads the whole detail pane. Avoid that unless the
    // set of outputs actually changed, since this also runs on a timer and on
    // every RouteChanged the daemon emits.
    if (sameRoutes) {
        for (int i = 0; i < routeList_->count(); ++i) {
            applyRouteItem(routeList_->item(i), routes_[static_cast<std::size_t>(i)]);
        }
        updateRouteStatus();
        return;
    }

    const QString previous = currentRouteId_;

    routeList_->clear();
    QListWidgetItem* toSelect = nullptr;
    for (const auto& r : routes_) {
        auto* item = new QListWidgetItem();
        applyRouteItem(item, r);
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

void MainWindow::updateRouteStatus() {
    const RouteRow* route = findRoute(currentRouteId_);
    if (!route) {
        return;
    }

    // A band count that no longer matches the table means something other
    // than this window changed the EQ, so the tables really do need rebuilding.
    if (route->bandCount != static_cast<uint32_t>(bandTable_->rowCount())) {
        loadRouteDetail(*route);
        return;
    }

    suppressSignals_ = true;
    muteCheck_->setChecked(route->muted);
    autoConnectCheck_->setChecked(route->autoConnect);
    gainSlider_->setValue(static_cast<int>(std::lround(route->gainDb)));
    gainLabel_->setText(QString::number(route->gainDb, 'f', 1) + " dB");
    suppressSignals_ = false;

    rebuildChannelCombo(*route);
}

int MainWindow::pairCountForDevice(const QString& nodeName) const {
    int count = 0;
    for (const auto& d : devices_) {
        if (d.nodeName == nodeName) {
            ++count;
        }
    }
    return count;
}

bool MainWindow::channelsUnavailable(const RouteRow& route) const {
    bool devicePresent = false;
    for (const auto& d : devices_) {
        if (d.nodeName != route.deviceName) {
            continue;
        }
        devicePresent = true;
        if (d.leftChannel == route.leftChannel && d.rightChannel == route.rightChannel) {
            return false;
        }
    }
    return devicePresent;
}

void MainWindow::refreshDevices() {
    devices_ = dbus_->listDevices();

    const QString previous = deviceCombo_->currentData().toString();
    deviceCombo_->clear();
    for (const auto& d : devices_) {
        const QString name = d.description.isEmpty() ? d.nodeName : d.description;
        // Only spell out the pair for devices that actually offer a choice,
        // so an ordinary stereo sink stays a single plain entry.
        const QString label = (pairCountForDevice(d.nodeName) > 1 && !d.pairLabel.isEmpty())
                                   ? name + "  —  " + d.pairLabel
                                   : name;
        deviceCombo_->addItem(label, d.nodeName + '\n' + d.leftChannel + '\n' + d.rightChannel);
    }
    const int restored = deviceCombo_->findData(previous);
    if (restored >= 0) {
        deviceCombo_->setCurrentIndex(restored);
    }

    // Count real devices, not pair entries - two rows for one interface
    // shouldn't read as two devices.
    QSet<QString> uniqueDevices;
    for (const auto& d : devices_) {
        uniqueDevices.insert(d.nodeName);
    }
    statusLabel_->setText(uniqueDevices.isEmpty()
                               ? "No output devices found (or pipeeq-daemon isn't running)"
                               : QString("%1 output device(s), %2 selectable stereo pair(s)")
                                     .arg(uniqueDevices.size())
                                     .arg(devices_.size()));
}

void MainWindow::refreshInputs() {
    inputs_ = dbus_->listInputs();
    if (!currentRouteId_.isEmpty()) {
        rebuildMixerTable();
    }
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
    autoConnectCheck_->setChecked(route.autoConnect);
    gainSlider_->setValue(static_cast<int>(std::lround(route.gainDb)));
    gainLabel_->setText(QString::number(route.gainDb, 'f', 1) + " dB");
    bandCountSpin_->setValue(static_cast<int>(route.bandCount));
    suppressSignals_ = false;

    rebuildChannelCombo(route);

    const auto bands = dbus_->getRouteBands(route.id);
    rebuildBandTable(bands);
    curveWidget_->setBands(bands);

    rebuildMixerTable();
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

void MainWindow::rebuildMixerTable() {
    if (currentRouteId_.isEmpty()) {
        mixerTable_->setRowCount(0);
        return;
    }

    const auto activeGains = dbus_->getRouteInputGains(currentRouteId_);

    suppressSignals_ = true;
    mixerTable_->setRowCount(static_cast<int>(inputs_.size()));

    for (int row = 0; row < static_cast<int>(inputs_.size()); ++row) {
        const InputRow& input = inputs_[static_cast<std::size_t>(row)];

        auto* nameItem = new QTableWidgetItem(input.displayName);
        nameItem->setData(Qt::UserRole, input.id);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        mixerTable_->setItem(row, 0, nameItem);

        double gainDb = -60.0;
        bool active = false;
        for (const auto& [id, db] : activeGains) {
            if (id == input.id) {
                active = true;
                gainDb = db;
                break;
            }
        }

        auto* onCheck = new QCheckBox();
        onCheck->setChecked(active);
        mixerTable_->setCellWidget(row, 1, onCheck);

        auto* levelSlider = new QSlider(Qt::Horizontal);
        levelSlider->setRange(-60, 12);
        levelSlider->setValue(static_cast<int>(std::lround(gainDb)));
        levelSlider->setEnabled(active);
        mixerTable_->setCellWidget(row, 2, levelSlider);

        connect(onCheck, &QCheckBox::toggled, this, [this, row, levelSlider](bool checked) {
            levelSlider->setEnabled(checked);
            pushMixerRow(row);
        });
        connect(levelSlider, &QSlider::valueChanged, this, [this, row](int) { pushMixerRow(row); });
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

void MainWindow::pushMixerRow(int row) {
    if (suppressSignals_ || currentRouteId_.isEmpty()) {
        return;
    }

    QTableWidgetItem* nameItem = mixerTable_->item(row, 0);
    auto* onCheck = qobject_cast<QCheckBox*>(mixerTable_->cellWidget(row, 1));
    auto* levelSlider = qobject_cast<QSlider*>(mixerTable_->cellWidget(row, 2));
    if (!nameItem || !onCheck || !levelSlider) {
        return;
    }

    const QString inputId = nameItem->data(Qt::UserRole).toString();
    if (onCheck->isChecked()) {
        dbus_->setRouteInputGain(currentRouteId_, inputId, levelSlider->value());
    } else {
        dbus_->removeRouteInput(currentRouteId_, inputId);
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
