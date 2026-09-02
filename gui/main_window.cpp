#include "main_window.h"

#include <algorithm>
#include <cmath>

#include <QAbstractSpinBox>
#include <QApplication>
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
    // Width only. The minimum HEIGHT is derived from the content instead, in
    // applyDetailSizing - see the note there.
    setMinimumWidth(940);

    auto* central = new QWidget(this);
    rootLayout_ = new QVBoxLayout(central);
    rootLayout_->setSpacing(8);

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
    rootLayout_->addLayout(topBar);

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

    rootLayout_->addWidget(detailStack_, 1);

    // The two pages want very different amounts of room: the mixer view should
    // leave the strip rack as the dominant surface, while editing an EQ curve
    // wants all the height it can get. So the split changes with the page
    // rather than being a compromise that suits neither.
    connect(detailStack_, &QStackedWidget::currentChanged, this,
            [this](int index) { applyDetailSizing(index); });

    // ---- the mixer row, along the bottom ----
    stripRack_ = new StripRack(state_, central);
    connect(stripRack_, &StripRack::selectionChanged, this, &MainWindow::onRackSelectionChanged);
    // The position badge selects its strip and nothing more - the same as
    // clicking the strip body.
    //
    // It used to force the mixer page as well, so that the position control in
    // the detail panel was on screen. But the badge is the most label-like part
    // of a strip and the most natural "select this channel" target on it, so
    // from the EQ page that closed the editor - which is not what clicking
    // another output while editing an EQ should do. Selecting is enough,
    // because the editor follows the selection: it switches to that channel's
    // curve and stays where it is.
    connect(stripRack_, &StripRack::positionClicked, this,
            &MainWindow::onRackSelectionChanged);
    connect(stripRack_, &StripRack::collapsedDevicesChanged, this, [this] { saveSession(); });
    // The two rows of faders keep the same width. Driven by a signal rather
    // than read during applyDetailSizing, because the order in which the rack
    // and this window handle a resize is not defined - and the rack is the one
    // that computes the scale.
    connect(stripRack_, &StripRack::stripWidthScaleChanged, this,
            [this](double scale) { detailPanel_->setStripWidthScale(scale); });
    connect(stripRack_, &StripRack::statusMessage, this, [this](const QString& message) {
        // The rack's own progress and refusals go to the status bar; an empty
        // message means "nothing to say", so fall back to the selection.
        if (message.isEmpty()) {
            updateStripStatus();
        } else {
            statusLabel_->setText(message);
        }
    });
    rootLayout_->addWidget(stripRack_, 1);

    setCentralWidget(central);
    applyDetailSizing(0);

    // Window-level shortcuts, so linking works wherever focus happens to be.
    auto* linkShortcut = new QShortcut(QKeySequence(Qt::Key_L), this);
    connect(linkShortcut, &QShortcut::activated, stripRack_, &StripRack::linkMarkedChannels);
    // Escape means "back out of what I am in", innermost first.
    //
    // A number field being edited is the innermost thing of all: Escape in a
    // spin box means "forget what I typed", and a QDoubleSpinBox does not
    // consume the key, so without this it reached the window shortcut, the page
    // switched, the box lost focus - and losing focus COMMITTED the half-typed
    // value. The one key that universally cancels an edit was applying it.
    //
    // Then a pending link selection, which the rack reports on so an Escape
    // that cleared something does not also leave the editor. The rack is fully
    // visible from the EQ page and its link marks paint there, so an earlier
    // comment claiming otherwise was simply wrong.
    auto* escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escapeShortcut, &QShortcut::activated, this, [this] {
        if (auto* spin = qobject_cast<QAbstractSpinBox*>(QApplication::focusWidget())) {
            spin->interpretText(); // discards the pending text, keeps the value
            spin->selectAll();
            return;
        }
        if (stripRack_->clearLinkMarks()) {
            return;
        }
        if (detailStack_->currentIndex() == 1) {
            detailStack_->setCurrentIndex(0);
        }
    });

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
// The rack is BOUNDED rather than fixed, and the layout does the arithmetic.
//
// Fixing its height made it a constraint the root layout had to satisfy, which
// meant predicting the split by hand - and every version of that prediction was
// wrong in a way that showed up as widgets drawn on top of each other. The
// window's own minimum was computed assuming the rack sat at its floor, which
// is only true on the EQ page; capping the rack by hand needed the height of
// the window chrome, which cannot be had from size hints (they sum to 81 px
// where the true figure is 99 - QMainWindow contributes the difference around
// its central widget) and measuring it back off the layout converged on
// different answers depending on when it was asked.
//
// With a minimum and a maximum instead, and stretch on both rows, Qt is free to
// trade: the rack takes what it can up to the ceiling, and the detail area's
// own minimum is never violated because the rack can give way to its floor. No
// explicit window minimum either - the layout's is exact, and setting one by
// hand only suppresses it.
void MainWindow::applyDetailSizing(int pageIndex) {
    const bool editingEq = pageIndex == 1;

    // The floor is whatever the rack needs to show a strip completely, asked of
    // the rack rather than assumed: it varies with the devices present, since an
    // absent device's block carries an extra collapse button. The previous
    // constant, 215 px, was too small once that button existed and quietly
    // clipped the mute/link row off the bottom of every strip.
    const int floorHeight = stripRack_->contentMinimumHeight();

    // On the mixer page the rack is the primary surface, so it may grow. The
    // ceiling is the "within reason" part: past roughly 240 px of extra travel
    // a fader is just harder to aim, and the sends and EQ curve make better use
    // of the room. On the EQ page it collapses to its floor so the curve gets
    // everything else - the selection stays visible, which is the point of not
    // hiding it outright.
    const int ceiling =
        editingEq ? floorHeight : std::clamp(height() * 44 / 100, floorHeight, floorHeight + 240);

    if (stripRack_->minimumHeight() != floorHeight) {
        stripRack_->setMinimumHeight(floorHeight);
    }
    if (stripRack_->maximumHeight() != ceiling) {
        stripRack_->setMaximumHeight(ceiling);
    }
    // Ask for the ceiling. The layout hands out space by size hint first and
    // only then by stretch, so a rack that asks for nothing in particular is
    // given nothing in particular - it settled 167 px below its ceiling on a
    // 1150 px window, and the strips stopped growing well before they should.
    // When the window is too small for both rows' hints, Qt shrinks them
    // towards their minimums instead, which is what keeps the detail area above
    // its own.
    stripRack_->setPreferredHeight(ceiling);

    // Grow to the layout's own minimum if we are below it.
    //
    // Qt's minimum constrains what the window manager will let the user drag
    // the window to, but nothing clamps a resize() the program itself made
    // before the layout existed - which is how the window opened 58 px short of
    // its minimum and drew the band controls over the EQ curve. minimumSizeHint
    // is the layout's own figure, so unlike the arithmetic this replaces it
    // cannot disagree with what the layout will actually do.
    const int required = minimumSizeHint().height();
    if (height() < required) {
        resize(width(), required);
    }
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
        // And the editor, which was left showing a removed channel's curve -
        // title, ribbon and knobs all live, all writing nowhere. Its refresh()
        // already renders "No channel selected"; it was simply never called.
        // Back to the mixer too: an editor for nothing is not a page to sit on.
        eqEditor_->setSelection(QString());
        if (detailStack_->currentIndex() == 1) {
            detailStack_->setCurrentIndex(0);
        }
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
    const bool sameStrip = stripId == currentStripId_;
    stripRack_->setSelectedStripId(stripId);
    currentStripId_ = stripId;
    detailPanel_->setSelection(stripId);
    // Only when the selection actually MOVED. setSelection resets the editor to
    // band 1, so re-selecting the strip you are already on - which is now the
    // common case, since clicking a strip's meter is the safe way to select it
    // - threw away the band you were editing, and cost a round trip per click.
    if (!sameStrip) {
        eqEditor_->setSelection(stripId);
    }
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
