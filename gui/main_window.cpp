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
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include "widgets/detail_panel.h"
#include "widgets/strip_rack.h"

namespace pipeeq {

namespace {

// The combo's currentIndex() is used directly as the FilterType value, so this
// list must stay in the enum's order.
const struct {
    eqcore::FilterType type;
    const char* label;
} kFilterTypeOptions[] = {
    {eqcore::FilterType::Peaking, "Peaking"},
    {eqcore::FilterType::LowShelf, "Low shelf"},
    {eqcore::FilterType::HighShelf, "High shelf"},
    {eqcore::FilterType::LowPass, "Low pass"},
    {eqcore::FilterType::HighPass, "High pass"},
};

} // namespace

MainWindow::MainWindow(AppState* state, QWidget* parent)
    : QMainWindow(parent), state_(state) {
    setWindowTitle("PipeEQ");
    resize(1280, 740);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setSpacing(8);

    // ---- top bar: output management ----
    auto* topBar = new QHBoxLayout;
    deviceCombo_ = new QComboBox(central);
    deviceCombo_->setMinimumWidth(280);
    addButton_ = new QPushButton("Add output", central);
    connect(addButton_, &QPushButton::clicked, this, &MainWindow::onAddOutputClicked);
    removeButton_ = new QPushButton("Remove output", central);
    connect(removeButton_, &QPushButton::clicked, this, &MainWindow::onRemoveOutputClicked);
    topBar->addWidget(deviceCombo_);
    topBar->addWidget(addButton_);
    topBar->addWidget(removeButton_);
    topBar->addStretch(1);
    rootLayout->addLayout(topBar);

    // ---- detail area ----
    //
    // Page 0 is the selection's sends and EQ preview; page 1 is the EQ editor.
    // A stacked page rather than a separate window, so there is one window title
    // to capture in a screenshot and no modal dialog stalling the meter timer.
    detailStack_ = new QStackedWidget(central);

    detailPanel_ = new DetailPanel(state_, detailStack_);
    connect(detailPanel_, &DetailPanel::eqEditRequested, this,
            [this](const QString&) { detailStack_->setCurrentIndex(1); });
    connect(detailPanel_, &DetailPanel::addInputRequested, this, &MainWindow::onAddInputClicked);
    detailStack_->addWidget(detailPanel_);

    auto* eqPage = new QWidget(detailStack_);
    auto* eqLayout = new QVBoxLayout(eqPage);

    auto* eqControlsRow = new QHBoxLayout;
    auto* backButton = new QPushButton("< Back to mixer", eqPage);
    connect(backButton, &QPushButton::clicked, this, [this] { detailStack_->setCurrentIndex(0); });
    eqControlsRow->addWidget(backButton);
    eqControlsRow->addWidget(new QLabel("Bands:", eqPage));
    bandCountSpin_ = new QSpinBox(eqPage);
    bandCountSpin_->setRange(0, 16);
    connect(bandCountSpin_, &QSpinBox::valueChanged, this, &MainWindow::onBandCountChanged);
    eqControlsRow->addWidget(bandCountSpin_);
    eqControlsRow->addStretch(1);
    copyEqButton_ = new QPushButton("Copy EQ to...", eqPage);
    connect(copyEqButton_, &QPushButton::clicked, this, &MainWindow::onCopyEqClicked);
    eqControlsRow->addWidget(copyEqButton_);
    eqLayout->addLayout(eqControlsRow);

    bandTable_ = new QTableWidget(0, 4, eqPage);
    bandTable_->setHorizontalHeaderLabels({"Type", "Freq (Hz)", "Gain (dB)", "Q"});
    bandTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    bandTable_->setMaximumHeight(180);
    eqLayout->addWidget(bandTable_);

    curveWidget_ = new EqCurveWidget(eqPage);
    connect(curveWidget_, &EqCurveWidget::bandEdited, this, &MainWindow::onCurveBandEdited);
    connect(curveWidget_, &EqCurveWidget::bandEditBegan, this, &MainWindow::onCurveBandEditBegan);
    connect(curveWidget_, &EqCurveWidget::bandEditFinished, this,
            &MainWindow::onCurveBandEditFinished);
    eqLayout->addWidget(curveWidget_, 1);

    detailStack_->addWidget(eqPage);
    rootLayout->addWidget(detailStack_, 1);

    // The two pages want very different amounts of room: the mixer view should
    // leave the strip rack as the dominant surface, while editing an EQ curve
    // wants all the height it can get. So the split changes with the page
    // rather than being a compromise that suits neither.
    connect(detailStack_, &QStackedWidget::currentChanged, this,
            [this](int index) { applyDetailSizing(index); });

    // ---- the mixer row, along the bottom ----
    stripRack_ = new StripRack(state_, central);
    connect(stripRack_, &StripRack::selectionChanged, this, &MainWindow::onRackSelectionChanged);
    connect(stripRack_, &StripRack::positionClicked, this, [this](const QString& stripId) {
        selectStrip(stripId);
        detailStack_->setCurrentIndex(0);
    });
    connect(stripRack_, &StripRack::linkToggleRequested, this, [this](const QString&) {
        statusLabel_->setText("Linking and unlinking arrive with the grouping work.");
    });
    rootLayout->addWidget(stripRack_);

    setCentralWidget(central);
    applyDetailSizing(0);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_);

    connect(state_, &AppState::topologyChanged, this, &MainWindow::onTopologyChanged);
    connect(state_, &AppState::stripsUpdated, this, &MainWindow::onStripsUpdated);
    connect(state_, &AppState::channelDetailUpdated, this, &MainWindow::onChannelDetailUpdated);
    connect(state_, &AppState::sendsUpdated, this,
            [this](const QString&) { detailPanel_->refreshValues(); });
    connect(state_, &AppState::errorReported, this, &MainWindow::onErrorReported);
    connect(state_, &AppState::availabilityChanged, this, [this](bool) { updateStripStatus(); });
    // One timer in the store drives every meter; each rack decides which of its
    // widgets are actually visible and worth repainting.
    connect(&state_->meters(), &LevelMeters::levelsUpdated, this, [this] {
        stripRack_->refreshMeters();
        detailPanel_->refreshMeters();
    });

    onTopologyChanged();
}

void MainWindow::applyDetailSizing(int pageIndex) {
    // The rack gets a FIXED height and the detail area absorbs the slack, rather
    // than the other way round. Letting the rack stretch made the faders
    // absurdly tall on a big window, and a fader's travel should be a usable
    // size rather than however much room happens to be left over.
    const bool editingEq = pageIndex == 1;
    // Collapsing the rack rather than hiding it while editing: the selection
    // stays visible, so it is obvious which channel's curve is on screen.
    const int rackHeight = editingEq ? 150 : 300;
    stripRack_->setMinimumHeight(rackHeight);
    stripRack_->setMaximumHeight(rackHeight);
}

// ------------------------------------------------------------------ lookups --

const StripRow* MainWindow::findStrip(const QString& stripId) const {
    return state_->findStrip(stripId);
}

const DeviceRow* MainWindow::findDevice(const QString& nodeName) const {
    return state_->findDevice(nodeName);
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
        detailPanel_->setSelection(QString());
        updateStripStatus();
    }
}

void MainWindow::refreshInputs() {
    detailPanel_->rebuildSends();
}

void MainWindow::updateStripStatus() {
    const StripRow* strip = findStrip(currentStripId_);

    // Push current values into the existing widgets rather than rebuilding them.
    stripRack_->refreshValues();
    detailPanel_->refreshValues();

    const bool haveStrip = strip != nullptr;
    bandCountSpin_->setEnabled(haveStrip);
    copyEqButton_->setEnabled(haveStrip);
    removeButton_->setEnabled(haveStrip);

    if (!haveStrip) {
        // "No daemon" and "no outputs yet" used to be indistinguishable, which
        // made a daemon that wasn't running look like a working one with nothing
        // configured.
        if (!state_->isAvailable()) {
            statusLabel_->setText("Not connected to pipeeq-daemon. Is the service running?");
        } else {
            statusLabel_->setText(state_->strips().isEmpty()
                                       ? "No outputs configured. Pick a device and press Add."
                                       : "No channel selected.");
        }
        return;
    }

    QString status = QString("%1  -  %2").arg(strip->label(), strip->deviceName);
    if (strip->connected && !strip->driven) {
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
    detailPanel_->setSelection(stripId);
    if (const StripRow* strip = findStrip(stripId)) {
        loadStripDetail(*strip);
    }
    updateStripStatus();
}

void MainWindow::onRackSelectionChanged(const QString& stripId) {
    selectStrip(stripId);
}

void MainWindow::loadStripDetail(const StripRow& strip) {
    // Draw whatever the store already has and ask for a refresh. The request is
    // async, so selecting a channel is instant even if the daemon is slow;
    // onChannelDetailUpdated redraws when the answer arrives.
    const QVector<eqcore::EqBand> cached = state_->channelBands(strip.outputId, strip.channelIndex);
    const std::vector<eqcore::EqBand> bands(cached.begin(), cached.end());

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    bandCountSpin_->setValue(static_cast<int>(bands.size()));
    suppressSignals_ = wasSuppressed;

    rebuildBandTable(bands);
    curveWidget_->setBands(bands);
    state_->requestChannelDetail(strip.outputId, strip.channelIndex);
}

// ----------------------------------------------------------------- mutations --

void MainWindow::onAddOutputClicked() {
    const QString deviceName = deviceCombo_->currentData().toString();
    if (deviceName.isEmpty()) {
        QMessageBox::information(this, "PipeEQ", "No output device is available to add.");
        return;
    }
    // Fire and forget: the new output's id arrives as part of the next snapshot
    // rather than as a return value, and the store's topologyChanged rebuilds
    // the rack.
    state_->addOutput(deviceName, deviceCombo_->currentText());
}

void MainWindow::onRemoveOutputClicked() {
    const StripRow* strip = findStrip(currentStripId_);
    if (!strip) {
        return;
    }
    // A strip is one channel, but removal is per output - be explicit rather
    // than quietly deleting the sibling channels too.
    const auto answer = QMessageBox::question(
        this, "Remove output",
        QString("Remove the whole output \"%1\" and all of its channels?").arg(strip->outputName));
    if (answer != QMessageBox::Yes) {
        return;
    }
    state_->removeOutput(strip->outputId);
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

// ------------------------------------------------------------------- EQ page --

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
    // their own update - one drag event should be one write, not two.
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

    // One write per drag EVENT is fine now: the store coalesces to at most 25 a
    // second and flushes the final value on release.
    state_->setChannelEqBand(strip->outputId, strip->channelIndex, static_cast<uint32_t>(index),
                              band);
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

    std::vector<std::pair<QCheckBox*, StripRow>> targets;
    for (const StripRow& strip : state_->strips()) {
        if (strip.id == source->id) {
            continue;
        }
        auto* check = new QCheckBox(strip.label(), &dialog);
        layout->addWidget(check);
        targets.emplace_back(check, strip);
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
        // instance: sharing is what the assignment matrix is for, and doing it
        // implicitly here would surprise anyone who then edited one side.
        state_->setChannelEqBandCount(target.outputId, target.channelIndex,
                                       static_cast<uint32_t>(bands.size()));
        for (std::size_t i = 0; i < bands.size(); ++i) {
            state_->setChannelEqBand(target.outputId, target.channelIndex,
                                      static_cast<uint32_t>(i), bands[i]);
        }
    }
}

// ------------------------------------------------------------- store events --

void MainWindow::onTopologyChanged() {
    refreshDevices();
    refreshStrips();
    refreshInputs();
}

void MainWindow::onStripsUpdated() {
    // Values moved but the set didn't: update in place and leave the band table
    // alone, so nothing is destroyed under a control someone is using.
    updateStripStatus();
}

void MainWindow::onChannelDetailUpdated(const QString& outputId, uint32_t channelIndex) {
    detailPanel_->refreshValues();

    const StripRow* strip = findStrip(currentStripId_);
    if (!strip || strip->outputId != outputId || strip->channelIndex != channelIndex) {
        return; // detail for a channel that isn't selected
    }
    const QVector<eqcore::EqBand> cached = state_->channelBands(outputId, channelIndex);
    const std::vector<eqcore::EqBand> bands(cached.begin(), cached.end());

    const bool wasSuppressed = suppressSignals_;
    suppressSignals_ = true;
    bandCountSpin_->setValue(static_cast<int>(bands.size()));
    suppressSignals_ = wasSuppressed;

    rebuildBandTable(bands);
    curveWidget_->setBands(bands);
}

void MainWindow::onErrorReported(const QString& message) {
    statusLabel_->setText(message);
}

} // namespace pipeeq
