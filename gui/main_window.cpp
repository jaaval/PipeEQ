#include "main_window.h"

#include "widgets/strip_rack.h"

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

MainWindow::MainWindow(AppState* state, QWidget* parent) : QMainWindow(parent), state_(state) {
    setWindowTitle("PipeEQ");
    resize(1000, 640);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setSpacing(8);

    // ---- top bar: output management ----
    auto* topBar = new QHBoxLayout;
    deviceCombo_ = new QComboBox(central);
    deviceCombo_->setMinimumWidth(260);
    addButton_ = new QPushButton("Add output", central);
    connect(addButton_, &QPushButton::clicked, this, &MainWindow::onAddOutputClicked);
    removeButton_ = new QPushButton("Remove output", central);
    connect(removeButton_, &QPushButton::clicked, this, &MainWindow::onRemoveOutputClicked);
    topBar->addWidget(deviceCombo_);
    topBar->addWidget(addButton_);
    topBar->addWidget(removeButton_);
    topBar->addStretch(1);
    rootLayout->addLayout(topBar);

    // ---- detail area: the selected channel or group ----
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
    connect(gainSlider_, &QSlider::sliderPressed, this, &MainWindow::onGainSliderPressed);
    connect(gainSlider_, &QSlider::sliderReleased, this, &MainWindow::onGainSliderReleased);
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

    // ---- the mixer row, along the bottom ----
    stripRack_ = new StripRack(state_, central);
    connect(stripRack_, &StripRack::selectionChanged, this, &MainWindow::onRackSelectionChanged);
    connect(stripRack_, &StripRack::positionClicked, this, [this](const QString& stripId) {
        // The position badge is the affordance; the combo above is where the
        // choice is actually made until the popup grid exists.
        selectStrip(stripId);
        if (positionCombo_->isVisible()) {
            positionCombo_->showPopup();
        }
    });
    connect(stripRack_, &StripRack::linkToggleRequested, this, [this](const QString& stripId) {
        Q_UNUSED(stripId);
        statusLabel_->setText("Linking and unlinking arrive with the grouping work.");
    });
    rootLayout->addWidget(stripRack_);

    setCentralWidget(central);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_);

    connect(state_, &AppState::topologyChanged, this, &MainWindow::onTopologyChanged);
    connect(state_, &AppState::stripsUpdated, this, &MainWindow::onStripsUpdated);
    connect(state_, &AppState::channelDetailUpdated, this, &MainWindow::onChannelDetailUpdated);
    connect(state_, &AppState::errorReported, this, &MainWindow::onErrorReported);
    connect(state_, &AppState::availabilityChanged, this, [this](bool) { updateStripStatus(); });
    // One timer in the store drives every meter; the rack decides which strips
    // are actually visible and worth repainting.
    connect(&state_->meters(), &LevelMeters::levelsUpdated, this,
            [this] { stripRack_->refreshMeters(); });

    onTopologyChanged();
    // The store owns the safety resync now, off the GUI thread, so there is no
    // poll timer here at all.
}

// ------------------------------------------------------------------ lookups --

const StripRow* MainWindow::findStrip(const QString& stripId) const {
    return state_->findStrip(stripId);
}

const DeviceRow* MainWindow::findDevice(const QString& nodeName) const {
    return state_->findDevice(nodeName);
}

bool MainWindow::channelUnavailable(const StripRow& strip) const {
    return strip.connected && !strip.driven;
}

// ---------------------------------------------------------------- refreshing --

void MainWindow::refreshDevices() {
    const QString previous = deviceCombo_->currentData().toString();
    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    deviceCombo_->clear();
    for (const DeviceRow& device : state_->devices()) {
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
    // Only called when the store says the strip SET moved. The rack reuses its
    // widgets where it can, so a rebuild can't drop a drag in progress.
    const QString previousSelection = currentStripId_;
    stripRack_->rebuild();

    if (findStrip(previousSelection)) {
        selectStrip(previousSelection);
    } else if (!state_->strips().isEmpty()) {
        selectStrip(stripRack_->selectedStripId().isEmpty() ? state_->strips().front().id
                                                            : stripRack_->selectedStripId());
    } else {
        currentStripId_.clear();
        updateStripStatus();
    }
}

void MainWindow::refreshInputs() {
    rebuildMixerTable();
}

void MainWindow::updateStripStatus() {
    const StripRow* strip = findStrip(currentStripId_);

    // Push current values into the existing strips rather than rebuilding them.
    stripRack_->refreshValues();

    const bool haveStrip = strip != nullptr;
    muteCheck_->setEnabled(haveStrip);
    autoConnectCheck_->setEnabled(haveStrip);
    gainSlider_->setEnabled(haveStrip);
    bandCountSpin_->setEnabled(haveStrip);
    copyEqButton_->setEnabled(haveStrip);
    removeButton_->setEnabled(haveStrip);
    detailTabs_->setEnabled(haveStrip);

    if (!haveStrip) {
        // "No daemon" and "no outputs yet" used to be indistinguishable here,
        // which made a daemon that wasn't running look like a working one with
        // nothing configured.
        if (!state_->isAvailable()) {
            statusLabel_->setText("Not connected to pipeeq-daemon. Is the service running?");
        } else {
            statusLabel_->setText(state_->strips().isEmpty()
                                       ? "No outputs configured. Pick a device and press Add."
                                       : "No channel selected.");
        }
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
    stripRack_->setSelectedStripId(stripId);
    currentStripId_ = stripId;
    if (const StripRow* strip = findStrip(stripId)) {
        loadStripDetail(*strip);
    }
}

void MainWindow::onRackSelectionChanged(const QString& stripId) {
    selectStrip(stripId);
}

void MainWindow::loadStripDetail(const StripRow& strip) {
    rebuildPositionCombo(strip);

    // Draw whatever the store already has, and ask for a refresh. The request
    // is async, so selecting a channel is instant even if the daemon is slow;
    // onChannelDetailUpdated redraws when the answer arrives.
    const QVector<eqcore::EqBand> cached = state_->channelBands(strip.outputId, strip.channelIndex);
    const std::vector<eqcore::EqBand> bands(cached.begin(), cached.end());

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    bandCountSpin_->setValue(static_cast<int>(bands.size()));
    suppressSignals_ = wasSuppressed;

    rebuildBandTable(bands);
    curveWidget_->setBands(bands);
    rebuildMixerTable();
    updateStripStatus();

    state_->requestChannelDetail(strip.outputId, strip.channelIndex);
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
    // Fire and forget: the new output's id comes back as part of the next
    // snapshot rather than as a return value, and the store's topologyChanged
    // rebuilds the list. Selection falls back to whatever survives.
    state_->addOutput(deviceName, deviceCombo_->currentText());
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
    state_->removeOutput(strip->outputId);
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
    state_->setChannelGain(strip->outputId, strip->channelIndex, value);
}

void MainWindow::onGainSliderPressed() {
    if (const StripRow* strip = findStrip(currentStripId_)) {
        state_->beginEdit(EditKey{strip->id, Field::Gain, -1});
    }
}

void MainWindow::onGainSliderReleased() {
    if (const StripRow* strip = findStrip(currentStripId_)) {
        state_->endEdit(EditKey{strip->id, Field::Gain, -1});
    }
}

void MainWindow::onMuteToggled(bool checked) {
    if (suppressSignals_) {
        return;
    }
    if (const StripRow* strip = findStrip(currentStripId_)) {
        state_->setChannelMuted(strip->outputId, strip->channelIndex, checked);
    }
}

void MainWindow::onAutoConnectToggled(bool checked) {
    if (suppressSignals_) {
        return;
    }
    if (const StripRow* strip = findStrip(currentStripId_)) {
        state_->setOutputAutoConnect(strip->outputId, checked);
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
    state_->setChannelPosition(strip->outputId, strip->channelIndex, position);
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
    // The store re-requests the detail as part of this, so the table and curve
    // are redrawn by onChannelDetailUpdated rather than from a blocking read.
    state_->setChannelEqBandCount(strip->outputId, strip->channelIndex,
                                  static_cast<uint32_t>(count));
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

    eqcore::EqBand band;
    band.type = static_cast<eqcore::FilterType>(typeCombo->currentIndex());
    band.freqHz = freqSpin->value();
    band.gainDb = gainSpin->value();
    band.q = qSpin->value();

    state_->setChannelEqBand(strip->outputId, strip->channelIndex, static_cast<uint32_t>(row), band);

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

    // One write per drag EVENT is fine now: the store coalesces them to at most
    // 25 a second and flushes the final value on release. Previously this was a
    // blocking round trip per mouse-move.
    state_->setChannelEqBand(strip->outputId, strip->channelIndex, static_cast<uint32_t>(index), band);
}

void MainWindow::onCurveBandEditBegan(int index) {
    if (const StripRow* strip = findStrip(currentStripId_)) {
        state_->beginEdit(EditKey{strip->id, Field::EqBand, index});
    }
}

void MainWindow::onCurveBandEditFinished(int index) {
    if (const StripRow* strip = findStrip(currentStripId_)) {
        state_->endEdit(EditKey{strip->id, Field::EqBand, index});
    }
}

void MainWindow::onCopyEqClicked() {
    const StripRow* source = findStrip(currentStripId_);
    if (!source) {
        return;
    }
    const QVector<eqcore::EqBand> cached =
        state_->channelBands(source->outputId, source->channelIndex);
    const std::vector<eqcore::EqBand> bands(cached.begin(), cached.end());

    QDialog dialog(this);
    dialog.setWindowTitle("Copy EQ to...");
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        QString("Copy the %1-band curve from \"%2\" to:").arg(bands.size()).arg(source->label()),
        &dialog));

    std::vector<std::pair<QCheckBox*, const StripRow*>> targets;
    for (const StripRow& strip : state_->strips()) {
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
        state_->setChannelEqBandCount(target->outputId, target->channelIndex,
                                      static_cast<uint32_t>(bands.size()));
        for (std::size_t i = 0; i < bands.size(); ++i) {
            state_->setChannelEqBand(target->outputId, target->channelIndex,
                                      static_cast<uint32_t>(i), bands[i]);
        }
    }
}

// ---------------------------------------------------------------- Mixer tab --

void MainWindow::rebuildMixerTable() {
    const StripRow* strip = findStrip(currentStripId_);

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;

    mixerTable_->setRowCount(state_->inputs().size());

    QVector<QPair<QString, double>> sends;
    if (strip) {
        sends = state_->channelSends(strip->outputId, strip->channelIndex);
    }

    for (int row = 0; row < state_->inputs().size(); ++row) {
        const InputRow& input = state_->inputs().at(row);

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
        state_->removeSend(strip->outputId, strip->channelIndex, inputId);
        return;
    }

    // A refusal (almost always the per-output send limit) now comes back
    // asynchronously as errorReported, which resyncs - so the switch corrects
    // itself rather than being left on while doing nothing.
    state_->setSend(strip->outputId, strip->channelIndex, inputId, levelSlider->value());
}

void MainWindow::onAddInputClicked() {
    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, "Add input", "Name for the new virtual sink:", QLineEdit::Normal,
                               QString(), &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }
    state_->addInput(name.trimmed());
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
    state_->removeInput(nameItem->data(Qt::UserRole).toString());
}

// -------------------------------------------------------------- daemon events --

void MainWindow::onTopologyChanged() {
    refreshDevices();
    refreshStrips();
    refreshInputs();
}

void MainWindow::onStripsUpdated() {
    // Values moved but the set didn't: update labels and the controls in place
    // and leave the band and mixer tables alone, so nothing is destroyed under
    // a slider someone is dragging.
    updateStripStatus();
}

void MainWindow::onChannelDetailUpdated(const QString& outputId, uint32_t channelIndex) {
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip || strip->outputId != outputId || strip->channelIndex != channelIndex) {
        return; // detail for a channel that isn't on screen
    }
    const QVector<eqcore::EqBand> cached = state_->channelBands(outputId, channelIndex);
    const std::vector<eqcore::EqBand> bands(cached.begin(), cached.end());

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    bandCountSpin_->setValue(static_cast<int>(bands.size()));
    suppressSignals_ = wasSuppressed;

    rebuildBandTable(bands);
    curveWidget_->setBands(bands);
    rebuildMixerTable();
}

void MainWindow::onErrorReported(const QString& message) {
    statusLabel_->setText(message);
}

} // namespace pipeeq
