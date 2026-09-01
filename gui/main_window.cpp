#include "main_window.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
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

// The combo's currentIndex() is used directly as the FilterType value, so this
// list must stay in the enum's order.
const struct {
    eqcore::FilterType type;
    const char* label;
    const char* wireName;
} kFilterTypeOptions[] = {
    {eqcore::FilterType::Peaking, "Peaking", "peaking"},
    {eqcore::FilterType::LowShelf, "Low shelf", "low_shelf"},
    {eqcore::FilterType::HighShelf, "High shelf", "high_shelf"},
    {eqcore::FilterType::LowPass, "Low pass", "low_pass"},
    {eqcore::FilterType::HighPass, "High pass", "high_pass"},
};

constexpr int kMinGainDb = -60;
constexpr int kMaxGainDb = 12;
// The level a mixer row shows for an input the channel isn't routed to at all.
// "Absent" and "-60 dB" are different things to the daemon; this is only how
// the off state is displayed.
constexpr double kOffLevelDb = -60.0;

QString wireNameFor(eqcore::FilterType type) {
    for (const auto& option : kFilterTypeOptions) {
        if (option.type == type) {
            return QString::fromLatin1(option.wireName);
        }
    }
    return QStringLiteral("peaking");
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("PipeEQ");
    resize(1000, 640);

    dbus_ = new DbusClient(this);

    auto* central = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);

    // ---- left: the list of hardware output channels ----
    auto* leftLayout = new QVBoxLayout;
    leftLayout->addWidget(new QLabel("Output channels", central));

    stripList_ = new QListWidget(central);
    connect(stripList_, &QListWidget::itemSelectionChanged, this,
            &MainWindow::onStripSelectionChanged);
    leftLayout->addWidget(stripList_, 1);

    auto* addRow = new QHBoxLayout;
    deviceCombo_ = new QComboBox(central);
    addButton_ = new QPushButton("Add", central);
    connect(addButton_, &QPushButton::clicked, this, &MainWindow::onAddOutputClicked);
    addRow->addWidget(deviceCombo_, 1);
    addRow->addWidget(addButton_);
    leftLayout->addLayout(addRow);

    removeButton_ = new QPushButton("Remove selected output", central);
    connect(removeButton_, &QPushButton::clicked, this, &MainWindow::onRemoveOutputClicked);
    leftLayout->addWidget(removeButton_);

    auto* leftContainer = new QWidget(central);
    leftContainer->setLayout(leftLayout);
    leftContainer->setMaximumWidth(280);
    rootLayout->addWidget(leftContainer);

    // ---- right: the selected channel's controls ----
    auto* rightLayout = new QVBoxLayout;

    auto* controlsRow = new QHBoxLayout;
    muteCheck_ = new QCheckBox("Mute", central);
    connect(muteCheck_, &QCheckBox::toggled, this, &MainWindow::onMuteToggled);
    controlsRow->addWidget(muteCheck_);

    autoConnectCheck_ = new QCheckBox("Auto-connect", central);
    connect(autoConnectCheck_, &QCheckBox::toggled, this, &MainWindow::onAutoConnectToggled);
    controlsRow->addWidget(autoConnectCheck_);

    positionLabel_ = new QLabel("Position:", central);
    positionCombo_ = new QComboBox(central);
    connect(positionCombo_, &QComboBox::currentIndexChanged, this,
            &MainWindow::onChannelPositionChanged);
    controlsRow->addWidget(positionLabel_);
    controlsRow->addWidget(positionCombo_);

    controlsRow->addWidget(new QLabel("Gain:", central));
    gainSlider_ = new QSlider(Qt::Horizontal, central);
    gainSlider_->setRange(kMinGainDb, kMaxGainDb);
    connect(gainSlider_, &QSlider::valueChanged, this, &MainWindow::onGainSliderChanged);
    controlsRow->addWidget(gainSlider_, 1);

    gainLabel_ = new QLabel("0 dB", central);
    gainLabel_->setMinimumWidth(60);
    controlsRow->addWidget(gainLabel_);
    rightLayout->addLayout(controlsRow);

    detailTabs_ = new QTabWidget(central);

    // ---- EQ tab ----
    auto* eqTab = new QWidget(detailTabs_);
    auto* eqLayout = new QVBoxLayout(eqTab);

    auto* eqControlsRow = new QHBoxLayout;
    eqControlsRow->addWidget(new QLabel("Bands:", eqTab));
    bandCountSpin_ = new QSpinBox(eqTab);
    bandCountSpin_->setRange(0, 16);
    connect(bandCountSpin_, &QSpinBox::valueChanged, this, &MainWindow::onBandCountChanged);
    eqControlsRow->addWidget(bandCountSpin_);
    eqControlsRow->addStretch(1);
    copyEqButton_ = new QPushButton("Copy EQ to...", eqTab);
    connect(copyEqButton_, &QPushButton::clicked, this, &MainWindow::onCopyEqClicked);
    eqControlsRow->addWidget(copyEqButton_);
    eqLayout->addLayout(eqControlsRow);

    bandTable_ = new QTableWidget(0, 4, eqTab);
    bandTable_->setHorizontalHeaderLabels({"Type", "Freq (Hz)", "Gain (dB)", "Q"});
    bandTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    bandTable_->setMaximumHeight(200);
    eqLayout->addWidget(bandTable_);

    curveWidget_ = new EqCurveWidget(eqTab);
    connect(curveWidget_, &EqCurveWidget::bandEdited, this, &MainWindow::onCurveBandEdited);
    eqLayout->addWidget(curveWidget_, 1);

    detailTabs_->addTab(eqTab, "EQ");

    // ---- Mixer tab ----
    auto* mixerTab = new QWidget(detailTabs_);
    auto* mixerLayout = new QVBoxLayout(mixerTab);

    auto* mixerControlsRow = new QHBoxLayout;
    addInputButton_ = new QPushButton("Add Input...", mixerTab);
    connect(addInputButton_, &QPushButton::clicked, this, &MainWindow::onAddInputClicked);
    removeInputButton_ = new QPushButton("Remove Selected Input", mixerTab);
    connect(removeInputButton_, &QPushButton::clicked, this, &MainWindow::onRemoveInputClicked);
    mixerControlsRow->addWidget(addInputButton_);
    mixerControlsRow->addWidget(removeInputButton_);
    mixerControlsRow->addStretch(1);
    mixerLayout->addLayout(mixerControlsRow);

    mixerTable_ = new QTableWidget(0, 3, mixerTab);
    mixerTable_->setHorizontalHeaderLabels({"Input", "On", "Level (dB)"});
    mixerTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    mixerLayout->addWidget(mixerTable_, 1);

    detailTabs_->addTab(mixerTab, "Mixer");
    rightLayout->addWidget(detailTabs_, 1);

    auto* rightContainer = new QWidget(central);
    rightContainer->setLayout(rightLayout);
    rootLayout->addWidget(rightContainer, 1);

    setCentralWidget(central);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_);

    connect(dbus_, &DbusClient::outputChanged, this, &MainWindow::onDaemonOutputChanged);
    connect(dbus_, &DbusClient::outputsChanged, this, &MainWindow::refreshStrips);
    connect(dbus_, &DbusClient::inputsChanged, this, &MainWindow::onDaemonInputsChanged);
    connect(dbus_, &DbusClient::devicesChanged, this, &MainWindow::refreshDevices);

    refreshDevices();
    refreshInputs();
    refreshStrips();

    // The daemon now emits DevicesChanged and OutputChanged, so this is only a
    // backstop against a dropped signal rather than the primary refresh path -
    // hence 5 s rather than the 3 s it used to poll at.
    auto* pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, [this] {
        refreshDevices();
        refreshStrips();
    });
    pollTimer->start(5000);
}

// ------------------------------------------------------------------ lookups --

const StripRow* MainWindow::findStrip(const QString& stripId) const {
    auto it = std::find_if(strips_.begin(), strips_.end(),
                            [&](const StripRow& s) { return s.id == stripId; });
    return it == strips_.end() ? nullptr : &*it;
}

const DeviceRow* MainWindow::findDevice(const QString& nodeName) const {
    auto it = std::find_if(devices_.begin(), devices_.end(),
                            [&](const DeviceRow& d) { return d.nodeName == nodeName; });
    return it == devices_.end() ? nullptr : &*it;
}

bool MainWindow::channelUnavailable(const StripRow& strip) const {
    return strip.connected && !strip.driven;
}

// ---------------------------------------------------------------- refreshing --

void MainWindow::refreshDevices() {
    devices_ = dbus_->listDevices();

    const QString previous = deviceCombo_->currentData().toString();
    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    deviceCombo_->clear();
    for (const DeviceRow& device : devices_) {
        QString label = device.description.isEmpty() ? device.nodeName : device.description;
        if (!device.positions.isEmpty()) {
            label += QString(" (%1 ch)").arg(device.positions.size());
        }
        if (device.inUse) {
            label += " - in use";
        }
        deviceCombo_->addItem(label, device.nodeName);
    }
    const int restored = deviceCombo_->findData(previous);
    if (restored >= 0) {
        deviceCombo_->setCurrentIndex(restored);
    }
    suppressSignals_ = wasSuppressed;
}

void MainWindow::refreshStrips() {
    const std::vector<StripRow> updated = dbus_->listStrips();

    // Only rebuild the list widget when the SET of strips changed. Rebuilding
    // it on every refresh would reset the selection and yank the detail pane
    // out from under whatever the user was doing.
    const auto idsOf = [](const std::vector<StripRow>& rows) {
        QStringList ids;
        for (const StripRow& row : rows) {
            ids << row.id;
        }
        return ids;
    };
    const bool setChanged = idsOf(strips_) != idsOf(updated);
    strips_ = updated;

    if (!setChanged) {
        updateStripStatus();
        return;
    }

    const QString previousSelection = currentStripId_;
    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    stripList_->clear();
    for (const StripRow& strip : strips_) {
        auto* item = new QListWidgetItem(stripList_);
        item->setData(Qt::UserRole, strip.id);
        applyStripItem(item, strip);
        stripList_->addItem(item);
    }
    suppressSignals_ = wasSuppressed;

    // Keep the selection if that strip still exists; otherwise fall back to the
    // first one so the detail pane is never left showing nothing.
    if (findStrip(previousSelection)) {
        selectStrip(previousSelection);
    } else if (!strips_.empty()) {
        selectStrip(strips_.front().id);
    } else {
        currentStripId_.clear();
        updateStripStatus();
    }
}

void MainWindow::refreshInputs() {
    inputs_ = dbus_->listInputs();
    rebuildMixerTable();
}

void MainWindow::applyStripItem(QListWidgetItem* item, const StripRow& strip) const {
    QString suffix;
    QString tooltip;
    if (channelUnavailable(strip)) {
        suffix = " (ch n/a)";
        tooltip = "The device is present but its current profile doesn't offer this channel. "
                  "The settings are kept and reapplied if it comes back.";
    } else if (!strip.connected) {
        suffix = strip.autoConnect ? " (waiting)" : " (off)";
        tooltip = strip.autoConnect
                       ? "The device isn't available right now. This channel stays configured and "
                         "connects as soon as the device appears."
                       : "Auto-connect is off, so this output won't connect on its own.";
    } else {
        tooltip = QString("Driving %1 on %2").arg(strip.position, strip.deviceName);
    }
    if (!strip.groupId.isEmpty()) {
        suffix += " [linked]";
    }

    item->setText(strip.label() + suffix);
    item->setToolTip(tooltip);
    // Dim anything that isn't currently carrying audio, while leaving it fully
    // editable - that distinction is the whole point of keeping absent outputs.
    if (strip.connected && strip.driven) {
        item->setForeground(QBrush());
    } else {
        item->setForeground(stripList_->palette().brush(QPalette::Disabled, QPalette::Text));
    }
}

void MainWindow::updateStripStatus() {
    const StripRow* strip = findStrip(currentStripId_);

    // Refresh the list labels in place - cheap, and it can't disturb selection.
    for (int row = 0; row < stripList_->count(); ++row) {
        QListWidgetItem* item = stripList_->item(row);
        if (const StripRow* rowStrip = findStrip(item->data(Qt::UserRole).toString())) {
            applyStripItem(item, *rowStrip);
        }
    }

    const bool haveStrip = strip != nullptr;
    muteCheck_->setEnabled(haveStrip);
    autoConnectCheck_->setEnabled(haveStrip);
    gainSlider_->setEnabled(haveStrip);
    bandCountSpin_->setEnabled(haveStrip);
    copyEqButton_->setEnabled(haveStrip);
    removeButton_->setEnabled(haveStrip);
    detailTabs_->setEnabled(haveStrip);

    if (!haveStrip) {
        statusLabel_->setText(strips_.empty() ? "No outputs configured. Pick a device and press Add."
                                              : "No channel selected.");
        gainLabel_->setText("- dB");
        return;
    }

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    muteCheck_->setChecked(strip->muted);
    autoConnectCheck_->setChecked(strip->autoConnect);
    gainSlider_->setValue(static_cast<int>(std::lround(strip->gainDb)));
    suppressSignals_ = wasSuppressed;

    gainLabel_->setText(QString("%1 dB").arg(strip->gainDb, 0, 'f', 1));

    QString status = QString("%1  -  %2").arg(strip->label(), strip->deviceName);
    if (channelUnavailable(*strip)) {
        status += "  -  channel not offered by the device's current profile";
    } else if (!strip->connected) {
        status += strip->autoConnect ? "  -  waiting for the device" : "  -  auto-connect off";
    }
    if (!strip->groupId.isEmpty()) {
        status += "  -  linked (gain, mute and sends move together)";
    }
    statusLabel_->setText(status);
}

// ----------------------------------------------------------------- selection --

void MainWindow::selectStrip(const QString& stripId) {
    for (int row = 0; row < stripList_->count(); ++row) {
        if (stripList_->item(row)->data(Qt::UserRole).toString() == stripId) {
            const bool wasSuppressed = suppressSignals_;
            suppressSignals_ = true;
            stripList_->setCurrentRow(row);
            suppressSignals_ = wasSuppressed;
            break;
        }
    }
    currentStripId_ = stripId;
    if (const StripRow* strip = findStrip(stripId)) {
        loadStripDetail(*strip);
    }
}

void MainWindow::onStripSelectionChanged() {
    if (suppressSignals_) {
        return;
    }
    QListWidgetItem* item = stripList_->currentItem();
    if (!item) {
        return;
    }
    selectStrip(item->data(Qt::UserRole).toString());
}

void MainWindow::loadStripDetail(const StripRow& strip) {
    rebuildPositionCombo(strip);

    const std::vector<eqcore::EqBand> bands =
        dbus_->getChannelEqBands(strip.outputId, strip.channelIndex);

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    bandCountSpin_->setValue(static_cast<int>(bands.size()));
    suppressSignals_ = wasSuppressed;

    rebuildBandTable(bands);
    curveWidget_->setBands(bands);
    rebuildMixerTable();
    updateStripStatus();
}

void MainWindow::rebuildPositionCombo(const StripRow& strip) {
    const DeviceRow* device = findDevice(strip.deviceName);

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    positionCombo_->clear();

    QVector<QString> options = device ? device->positions : QVector<QString>{};
    // Always offer the strip's CURRENT position even when the device no longer
    // advertises it, so a profile change doesn't silently snap the channel
    // somewhere else the moment the combo is rebuilt.
    if (!strip.position.isEmpty() && !options.contains(strip.position)) {
        options.push_back(strip.position);
    }
    for (const QString& position : options) {
        positionCombo_->addItem(position, position);
    }
    const int current = positionCombo_->findData(strip.position);
    if (current >= 0) {
        positionCombo_->setCurrentIndex(current);
    }
    suppressSignals_ = wasSuppressed;

    // Nothing to choose between is just noise on screen.
    const bool useful = positionCombo_->count() > 1;
    positionCombo_->setVisible(useful);
    positionLabel_->setVisible(useful);
}

// ----------------------------------------------------------------- mutations --

void MainWindow::onAddOutputClicked() {
    const QString deviceName = deviceCombo_->currentData().toString();
    if (deviceName.isEmpty()) {
        QMessageBox::information(this, "PipeEQ", "No output device is available to add.");
        return;
    }
    const QString label = deviceCombo_->currentText();
    const QString outputId = dbus_->addOutput(deviceName, label);
    refreshStrips();
    if (!outputId.isEmpty()) {
        selectStrip(outputId + "#0");
    }
}

void MainWindow::onRemoveOutputClicked() {
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip) {
        return;
    }
    // A strip is one channel, but removal is per output - be explicit about it
    // rather than quietly deleting the sibling channels too.
    const auto answer = QMessageBox::question(
        this, "Remove output",
        QString("Remove the whole output \"%1\" and all of its channels?").arg(strip->outputName));
    if (answer != QMessageBox::Yes) {
        return;
    }
    dbus_->removeOutput(strip->outputId);
    refreshStrips();
}

void MainWindow::onGainSliderChanged(int value) {
    if (suppressSignals_) {
        return;
    }
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip) {
        return;
    }
    gainLabel_->setText(QString("%1 dB").arg(value));
    dbus_->setChannelGain(strip->outputId, strip->channelIndex, value);
}

void MainWindow::onMuteToggled(bool checked) {
    if (suppressSignals_) {
        return;
    }
    if (const StripRow* strip = findStrip(currentStripId_)) {
        dbus_->setChannelMuted(strip->outputId, strip->channelIndex, checked);
    }
}

void MainWindow::onAutoConnectToggled(bool checked) {
    if (suppressSignals_) {
        return;
    }
    if (const StripRow* strip = findStrip(currentStripId_)) {
        dbus_->setOutputAutoConnect(strip->outputId, checked);
    }
}

void MainWindow::onChannelPositionChanged(int index) {
    if (suppressSignals_ || index < 0) {
        return;
    }
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip) {
        return;
    }
    const QString position = positionCombo_->itemData(index).toString();
    if (position == strip->position) {
        return;
    }
    dbus_->setChannelPosition(strip->outputId, strip->channelIndex, position);
    refreshStrips();
}

// ------------------------------------------------------------------- EQ tab --

void MainWindow::onBandCountChanged(int count) {
    if (suppressSignals_) {
        return;
    }
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip) {
        return;
    }
    dbus_->setChannelEqBandCount(strip->outputId, strip->channelIndex,
                                  static_cast<uint32_t>(count));
    const std::vector<eqcore::EqBand> bands =
        dbus_->getChannelEqBands(strip->outputId, strip->channelIndex);
    rebuildBandTable(bands);
    curveWidget_->setBands(bands);
}

void MainWindow::rebuildBandTable(const std::vector<eqcore::EqBand>& bands) {
    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;

    bandTable_->setRowCount(static_cast<int>(bands.size()));
    for (int row = 0; row < static_cast<int>(bands.size()); ++row) {
        const eqcore::EqBand& band = bands[static_cast<std::size_t>(row)];

        auto* typeCombo = new QComboBox(bandTable_);
        for (const auto& option : kFilterTypeOptions) {
            typeCombo->addItem(option.label);
        }
        typeCombo->setCurrentIndex(static_cast<int>(band.type));
        connect(typeCombo, &QComboBox::currentIndexChanged, this,
                [this, row](int) { pushBandRow(row); });
        bandTable_->setCellWidget(row, 0, typeCombo);

        auto* freqSpin = new QDoubleSpinBox(bandTable_);
        freqSpin->setRange(20.0, 20000.0);
        freqSpin->setDecimals(1);
        freqSpin->setValue(band.freqHz);
        connect(freqSpin, &QDoubleSpinBox::valueChanged, this,
                [this, row](double) { pushBandRow(row); });
        bandTable_->setCellWidget(row, 1, freqSpin);

        auto* gainSpin = new QDoubleSpinBox(bandTable_);
        gainSpin->setRange(-24.0, 24.0);
        gainSpin->setDecimals(2);
        gainSpin->setValue(band.gainDb);
        connect(gainSpin, &QDoubleSpinBox::valueChanged, this,
                [this, row](double) { pushBandRow(row); });
        bandTable_->setCellWidget(row, 2, gainSpin);

        auto* qSpin = new QDoubleSpinBox(bandTable_);
        qSpin->setRange(0.1, 10.0);
        qSpin->setDecimals(3);
        qSpin->setSingleStep(0.05);
        qSpin->setValue(band.q);
        connect(qSpin, &QDoubleSpinBox::valueChanged, this,
                [this, row](double) { pushBandRow(row); });
        bandTable_->setCellWidget(row, 3, qSpin);
    }

    suppressSignals_ = wasSuppressed;
}

void MainWindow::pushBandRow(int row) {
    if (suppressSignals_) {
        return;
    }
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip || row < 0 || row >= bandTable_->rowCount()) {
        return;
    }

    auto* typeCombo = qobject_cast<QComboBox*>(bandTable_->cellWidget(row, 0));
    auto* freqSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(row, 1));
    auto* gainSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(row, 2));
    auto* qSpin = qobject_cast<QDoubleSpinBox*>(bandTable_->cellWidget(row, 3));
    if (!typeCombo || !freqSpin || !gainSpin || !qSpin) {
        return;
    }

    const auto type = static_cast<eqcore::FilterType>(typeCombo->currentIndex());
    dbus_->setChannelEqBand(strip->outputId, strip->channelIndex, static_cast<uint32_t>(row),
                             wireNameFor(type), freqSpin->value(), gainSpin->value(), qSpin->value());

    eqcore::EqBand band;
    band.type = type;
    band.freqHz = freqSpin->value();
    band.gainDb = gainSpin->value();
    band.q = qSpin->value();

    std::vector<eqcore::EqBand> bands = curveWidget_->bands();
    if (row < static_cast<int>(bands.size())) {
        bands[static_cast<std::size_t>(row)] = band;
        curveWidget_->setBands(bands);
    }
}

void MainWindow::onCurveBandEdited(int index, eqcore::EqBand band) {
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip || index < 0 || index >= bandTable_->rowCount()) {
        return;
    }

    // Mirror the drag into the numeric row without letting those spinboxes push
    // their own update - one drag should be one write per event, not two.
    const bool wasSuppressed = suppressSignals_;
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
    suppressSignals_ = wasSuppressed;

    dbus_->setChannelEqBand(strip->outputId, strip->channelIndex, static_cast<uint32_t>(index),
                             wireNameFor(band.type), band.freqHz, band.gainDb, band.q);
}

void MainWindow::onCopyEqClicked() {
    const StripRow* source = findStrip(currentStripId_);
    if (!source) {
        return;
    }
    const std::vector<eqcore::EqBand> bands =
        dbus_->getChannelEqBands(source->outputId, source->channelIndex);

    QDialog dialog(this);
    dialog.setWindowTitle("Copy EQ to...");
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        QString("Copy the %1-band curve from \"%2\" to:").arg(bands.size()).arg(source->label()),
        &dialog));

    std::vector<std::pair<QCheckBox*, const StripRow*>> targets;
    for (const StripRow& strip : strips_) {
        if (strip.id == source->id) {
            continue;
        }
        auto* check = new QCheckBox(strip.label(), &dialog);
        layout->addWidget(check);
        targets.emplace_back(check, &strip);
    }
    if (targets.empty()) {
        QMessageBox::information(this, "PipeEQ", "There is no other channel to copy the EQ to.");
        return;
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    for (const auto& [check, target] : targets) {
        if (!check->isChecked()) {
            continue;
        }
        // Copies the band VALUES onto the target channel's own EQ instance
        // (created on demand). Deliberately a copy rather than sharing one
        // instance: sharing is what the assignment matrix in the new UI is for,
        // and doing it implicitly here would surprise anyone who then edited
        // one side.
        dbus_->setChannelEqBandCount(target->outputId, target->channelIndex,
                                      static_cast<uint32_t>(bands.size()));
        for (std::size_t i = 0; i < bands.size(); ++i) {
            dbus_->setChannelEqBand(target->outputId, target->channelIndex,
                                     static_cast<uint32_t>(i), wireNameFor(bands[i].type),
                                     bands[i].freqHz, bands[i].gainDb, bands[i].q);
        }
    }
    refreshStrips();
}

// ---------------------------------------------------------------- Mixer tab --

void MainWindow::rebuildMixerTable() {
    const StripRow* strip = findStrip(currentStripId_);

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;

    mixerTable_->setRowCount(static_cast<int>(inputs_.size()));

    std::vector<std::pair<QString, double>> sends;
    if (strip) {
        sends = dbus_->getChannelSends(strip->outputId, strip->channelIndex);
    }

    for (int row = 0; row < static_cast<int>(inputs_.size()); ++row) {
        const InputRow& input = inputs_[static_cast<std::size_t>(row)];

        QString label = input.displayName;
        if (!input.positions.isEmpty()) {
            label += QString(" (%1 ch)").arg(input.positions.size());
        }
        auto* nameItem = new QTableWidgetItem(label);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        nameItem->setData(Qt::UserRole, input.id);
        mixerTable_->setItem(row, 0, nameItem);

        const auto found = std::find_if(sends.begin(), sends.end(), [&](const auto& send) {
            return send.first == input.id;
        });
        const bool routed = found != sends.end();
        const double levelDb = routed ? found->second : kOffLevelDb;

        auto* onCheck = new QCheckBox(mixerTable_);
        onCheck->setChecked(routed);
        onCheck->setEnabled(strip != nullptr);
        connect(onCheck, &QCheckBox::toggled, this, [this, row](bool) { pushMixerRow(row); });
        mixerTable_->setCellWidget(row, 1, onCheck);

        auto* levelSlider = new QSlider(Qt::Horizontal, mixerTable_);
        levelSlider->setRange(kMinGainDb, kMaxGainDb);
        levelSlider->setValue(static_cast<int>(std::lround(levelDb)));
        levelSlider->setEnabled(strip != nullptr && routed);
        connect(levelSlider, &QSlider::valueChanged, this, [this, row](int) { pushMixerRow(row); });
        mixerTable_->setCellWidget(row, 2, levelSlider);
    }

    suppressSignals_ = wasSuppressed;
}

void MainWindow::pushMixerRow(int row) {
    if (suppressSignals_) {
        return;
    }
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip || row < 0 || row >= mixerTable_->rowCount()) {
        return;
    }

    QTableWidgetItem* nameItem = mixerTable_->item(row, 0);
    auto* onCheck = qobject_cast<QCheckBox*>(mixerTable_->cellWidget(row, 1));
    auto* levelSlider = qobject_cast<QSlider*>(mixerTable_->cellWidget(row, 2));
    if (!nameItem || !onCheck || !levelSlider) {
        return;
    }

    const QString inputId = nameItem->data(Qt::UserRole).toString();
    levelSlider->setEnabled(onCheck->isChecked());

    if (!onCheck->isChecked()) {
        dbus_->removeSend(strip->outputId, strip->channelIndex, inputId);
        return;
    }

    if (!dbus_->setSend(strip->outputId, strip->channelIndex, inputId, levelSlider->value())) {
        // The daemon refused - almost always because this output already sends
        // from the maximum number of inputs. Saying so beats leaving a switch
        // that looks on but does nothing.
        const bool wasSuppressed = suppressSignals_;
        suppressSignals_ = true;
        onCheck->setChecked(false);
        levelSlider->setEnabled(false);
        suppressSignals_ = wasSuppressed;
        statusLabel_->setText(
            QString("Could not route \"%1\" here - this output is already at its send limit.")
                .arg(nameItem->text()));
    }
}

void MainWindow::onAddInputClicked() {
    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, "Add input", "Name for the new virtual sink:", QLineEdit::Normal,
                               QString(), &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }
    dbus_->addInput(name.trimmed());
    refreshInputs();
}

void MainWindow::onRemoveInputClicked() {
    const int row = mixerTable_->currentRow();
    if (row < 0 || row >= mixerTable_->rowCount()) {
        QMessageBox::information(this, "PipeEQ", "Select an input row in the table first.");
        return;
    }
    QTableWidgetItem* nameItem = mixerTable_->item(row, 0);
    if (!nameItem) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, "Remove input",
        QString("Remove \"%1\"? It will be dropped from every output's mix.").arg(nameItem->text()));
    if (answer != QMessageBox::Yes) {
        return;
    }
    dbus_->removeInput(nameItem->data(Qt::UserRole).toString());
    refreshInputs();
}

// -------------------------------------------------------------- daemon events --

void MainWindow::onDaemonOutputChanged(const QString& outputId) {
    // A set on a linked channel moves its partners too, so refresh the whole
    // output's strips rather than just the one that was touched.
    Q_UNUSED(outputId);
    refreshStrips();
}

void MainWindow::onDaemonInputsChanged() {
    refreshInputs();
}

} // namespace pipeeq
