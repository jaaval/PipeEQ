#include "main_window.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QCloseEvent>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "widgets/detail_panel.h"
#include "widgets/eq_editor.h"
#include "widgets/strip_rack.h"

namespace pipeeq {

MainWindow::MainWindow(AppState* state, QWidget* parent)
    : QMainWindow(parent), state_(state) {
    setWindowTitle("PipeEQ");
    resize(1280, 740);
    // A floor, not the real constraint: the layout's own minimum governs above
    // this. It was 660 when the rack's height was a constant Qt had to violate
    // to fit, and violating it made the rack vanish outright. The rack now
    // holds a hard minimum of its own, so this only has to be low enough not to
    // exclude a display the window would otherwise fit - a 1920x1200 screen at
    // 2x scaling is 600 logical pixels tall, and 660 ruled it out.
    setMinimumSize(940, 600);

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
    connect(detailPanel_, &DetailPanel::eqEditRequested, this, [this](const QString& stripId) {
        eqEditor_->setSelection(stripId);
        detailStack_->setCurrentIndex(1);
    });
    connect(detailPanel_, &DetailPanel::addInputRequested, this, &MainWindow::onAddInputClicked);
    detailStack_->addWidget(detailPanel_);

    eqEditor_ = new EqEditor(state_, detailStack_);
    connect(eqEditor_, &EqEditor::backRequested, this,
            [this] { detailStack_->setCurrentIndex(0); });
    detailStack_->addWidget(eqEditor_);

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
    connect(stripRack_, &StripRack::collapsedDevicesChanged, this, [this] { saveSession(); });
    connect(stripRack_, &StripRack::statusMessage, this, [this](const QString& message) {
        // The rack's own progress and refusals go to the status bar; an empty
        // message means "nothing to say", so fall back to the selection.
        if (message.isEmpty()) {
            updateStripStatus();
        } else {
            statusLabel_->setText(message);
        }
    });
    rootLayout->addWidget(stripRack_);

    setCentralWidget(central);
    applyDetailSizing(0);

    // Window-level shortcuts, so linking works wherever focus happens to be.
    auto* linkShortcut = new QShortcut(QKeySequence(Qt::Key_L), this);
    connect(linkShortcut, &QShortcut::activated, stripRack_, &StripRack::linkMarkedChannels);
    auto* clearShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(clearShortcut, &QShortcut::activated, stripRack_, &StripRack::clearLinkMarks);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_);

    connect(state_, &AppState::topologyChanged, this, &MainWindow::onTopologyChanged);
    connect(state_, &AppState::stripsUpdated, this, &MainWindow::onStripsUpdated);
    // A local optimistic change to one channel: refresh only that output's
    // strips, not the whole window. This fires once per mouse-move during a
    // drag, so it must stay cheap.
    connect(state_, &AppState::channelValueChanged, this,
            [this](const QString&, uint32_t) { stripRack_->refreshValues(); });
    connect(state_, &AppState::channelDetailUpdated, this, &MainWindow::onChannelDetailUpdated);
    connect(state_, &AppState::sendsUpdated, this,
            [this](const QString&) { detailPanel_->refreshValues(); });
    connect(state_, &AppState::errorReported, this, &MainWindow::onErrorReported);
    connect(detailStack_, &QStackedWidget::currentChanged, this, [this](int) { saveSession(); });
    connect(state_, &AppState::availabilityChanged, this, [this](bool) { updateStripStatus(); });
    // One timer in the store drives every meter; each rack decides which of its
    // widgets are actually visible and worth repainting.
    connect(&state_->meters(), &LevelMeters::levelsUpdated, this, [this] {
        stripRack_->refreshMeters();
        detailPanel_->refreshMeters();
    });

    onTopologyChanged();
    restoreSession();
}

// Window geometry, which page was open, which channel was selected and which
// absent devices were collapsed. Small conveniences, but their absence is felt
// every single launch - and none of it belongs in the daemon's config, because
// it is per-machine UI state rather than anything about the audio.
void MainWindow::restoreSession() {
    QSettings settings;
    settings.beginGroup("mainWindow");
    const QByteArray geometry = settings.value("geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    stripRack_->setCollapsedDevices(settings.value("collapsedDevices").toStringList());
    const QString selection = settings.value("selectedStrip").toString();
    settings.endGroup();

    // The selection can only be restored once the first snapshot has arrived,
    // and only if that channel still exists - a device may have been unplugged
    // or its profile changed since.
    if (!selection.isEmpty()) {
        pendingSelection_ = selection;
    }
}

void MainWindow::saveSession() const {
    QSettings settings;
    settings.beginGroup("mainWindow");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("selectedStrip", currentStripId_);
    settings.setValue("collapsedDevices", stripRack_->collapsedDevices());
    settings.endGroup();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSession();
    QMainWindow::closeEvent(event);
}

// How the window's height is divided between the mixer row and the detail area
// above it.
//
// The rack is given a definite height and the detail area absorbs the rest,
// because a fader's travel should be a usable size rather than however much
// room happens to be left over. But that height used to be a CONSTANT, which
// meant every pixel of extra window height went to a detail area whose contents
// were pinned to their minimums - so a tall window grew nothing but a band of
// empty space between the two. It now scales with the window, within bounds.
void MainWindow::applyDetailSizing(int pageIndex) {
    const bool editingEq = pageIndex == 1;

    // The floor is whatever the rack needs to show a strip completely, asked of
    // the rack rather than assumed: it varies with the devices present, since an
    // absent device's block carries an extra collapse button. The previous
    // constant, 215 px, was too small once that button existed and quietly
    // clipped the mute/link row off the bottom of every strip.
    const int floorHeight = stripRack_->contentMinimumHeight();

    int rackHeight = floorHeight;
    if (!editingEq) {
        // On the mixer page the rack is the primary surface, so it takes a share
        // of the height. The ceiling is the "within reason" part: past roughly
        // 240 px of extra travel a fader is just harder to aim, and the sends
        // and EQ curve make better use of the room.
        // The WINDOW's height, not the central widget's. In resizeEvent the
        // central widget has not been laid out to the new size yet, so asking it
        // returns the previous height - which on the way up from the default
        // geometry is small enough that the clamp always chose the floor, and
        // the rack never grew at all.
        rackHeight = std::clamp(height() * 44 / 100, floorHeight, floorHeight + 240);
    }

    if (stripRack_->maximumHeight() == rackHeight && stripRack_->minimumHeight() == rackHeight) {
        return; // nothing to do, and re-setting it would relayout on every resize
    }
    stripRack_->setMinimumHeight(rackHeight);
    stripRack_->setMaximumHeight(rackHeight);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    applyDetailSizing(detailStack_->currentIndex());
}

// The first honest chance to measure the rack.
//
// A hidden widget contributes nothing to its layout's minimum size, and the
// first snapshot arrives - and so the rack is first built - during the
// constructor, before show(). Every measurement taken then reports the bare
// content margins, which is how the rack came out pinned at 254 px on a
// 1150 px window: the floor read as 14, so floor + 240 was the ceiling too.
void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    applyDetailSizing(detailStack_->currentIndex());
}

void MainWindow::showEqEditor() {
    eqEditor_->setSelection(currentStripId_);
    detailStack_->setCurrentIndex(1);
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
    QString previousSelection = currentStripId_;

    // A selection restored from the last session applies as soon as that
    // channel actually exists, and is dropped if it never appears.
    if (!pendingSelection_.isEmpty()) {
        if (findStrip(pendingSelection_)) {
            previousSelection = pendingSelection_;
            pendingSelection_.clear();
        } else if (!state_->strips().isEmpty()) {
            pendingSelection_.clear();
        }
    }

    stripRack_->rebuild();
    // The set of devices just changed, so the height the rack needs may have
    // changed with it.
    applyDetailSizing(detailStack_->currentIndex());

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
        status += "  -  linked (gain, mute, sends and EQ move together)";
    }
    statusLabel_->setText(status);
}

// ----------------------------------------------------------------- selection --

void MainWindow::selectStrip(const QString& stripId) {
    stripRack_->setSelectedStripId(stripId);
    currentStripId_ = stripId;
    detailPanel_->setSelection(stripId);
    eqEditor_->setSelection(stripId);
    if (const StripRow* strip = findStrip(stripId)) {
        loadStripDetail(*strip);
    }
    updateStripStatus();
}

void MainWindow::onRackSelectionChanged(const QString& stripId) {
    selectStrip(stripId);
}

void MainWindow::loadStripDetail(const StripRow& strip) {
    // The request is async, so selecting a channel is instant even if the daemon
    // is slow; the panels redraw when the answer arrives.
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
    // COPIED before the dialog. `strip` points into the store's cached vector,
    // and QMessageBox::question runs a nested event loop in which the resync
    // timer or any daemon signal can replace that vector - freeing the row out
    // from under this pointer while the dialog is still open.
    const QString outputId = strip->outputId;
    const QString outputName = strip->outputName;

    // A strip is one channel, but removal is per output - be explicit rather
    // than quietly deleting the sibling channels too.
    const auto answer = QMessageBox::question(
        this, "Remove output",
        QString("Remove the whole output \"%1\" and all of its channels?").arg(outputName));
    if (answer != QMessageBox::Yes) {
        return;
    }
    state_->removeOutput(outputId);
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








// ------------------------------------------------------------- store events --

void MainWindow::onTopologyChanged() {
    refreshDevices();
    refreshStrips();
    refreshInputs();
}

void MainWindow::onStripsUpdated() {
    // Values moved but the set didn't: update in place, so nothing is destroyed
    // under a control someone is using.
    //
    // The device combo is refreshed too. Its contents depend on DeviceRow
    // fields the topology comparison ignores - notably `inUse` - so a device
    // being claimed by another output left the combo advertising it without the
    // "in use" suffix until some unrelated topology change came along.
    refreshDevices();
    updateStripStatus();
}

void MainWindow::onChannelDetailUpdated(const QString& outputId, uint32_t channelIndex) {
    detailPanel_->refreshValues();

    const StripRow* strip = findStrip(currentStripId_);
    if (!strip || strip->outputId != outputId || strip->channelIndex != channelIndex) {
        return; // detail for a channel that isn't selected
    }
    eqEditor_->refresh();
}

void MainWindow::onErrorReported(const QString& message) {
    statusLabel_->setText(message);
}

} // namespace pipeeq
