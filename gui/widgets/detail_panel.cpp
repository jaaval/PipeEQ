#include "detail_panel.h"

#include <algorithm>

#include <QFrame>
#include <QInputDialog>
#include <QMenu>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "elided_label.h"
#include "eq_preview.h"
#include "position_selector.h"
#include "model/app_state.h"
#include "send_strip.h"
#include "theme/theme.h"

namespace pipeeq {

DetailPanel::DetailPanel(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
    const theme::Tokens tokens = theme::tokens();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    // ---- header ----
    auto* headerRow = new QHBoxLayout;
    title_ = new QLabel(this);
    QFont titleFont = tokens.uiFont;
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    title_->setFont(titleFont);
    headerRow->addWidget(title_);

    // Elided rather than a QLabel: this line carries the device node name plus
    // any status note, which is easily wider than the window, and a QLabel
    // would run underneath the buttons to its right rather than give way.
    subtitle_ = new ElidedLabel(this);
    subtitle_->setTextColor(tokens.textDim);
    headerRow->addWidget(subtitle_, 1);

    // A menu rather than two buttons: renaming is infrequent, and "which of
    // these two things am I renaming" is clearer as an explicit choice than as
    // two similarly-labelled buttons.
    renameButton_ = new QPushButton("Rename...", this);
    auto* renameMenu = new QMenu(renameButton_);
    renameMenu->addAction("Rename channel...", this, &DetailPanel::renameChannel);
    renameMenu->addAction("Rename output...", this, &DetailPanel::renameOutput);
    renameButton_->setMenu(renameMenu);
    headerRow->addWidget(renameButton_);

    headerRow->addWidget(new QLabel("Position:", this));
    positionButton_ = new QPushButton(this);
    positionButton_->setMinimumWidth(70);
    positionButton_->setToolTip("Which hardware position this channel drives.");
    connect(positionButton_, &QPushButton::clicked, this, &DetailPanel::choosePosition);
    headerRow->addWidget(positionButton_);

    autoConnectButton_ = new QPushButton("Auto-connect", this);
    autoConnectButton_->setCheckable(true);
    connect(autoConnectButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (suppressSignals_) {
            return;
        }
        if (const StripRow* strip = state_->findStrip(stripId_)) {
            state_->setOutputAutoConnect(strip->outputId, checked);
        }
    });
    headerRow->addWidget(autoConnectButton_);
    root->addLayout(headerRow);

    // ---- sends and EQ, side by side ----
    //
    // The sends column takes only the width its strips actually occupy and the
    // EQ absorbs everything left over, rather than the two splitting the window
    // by a fixed ratio. A proportional split looks right at one window size and
    // at any larger one leaves the send strips huddled at the left of a mostly
    // empty column, with the "Add sink" button stranded against the EQ.
    auto* bodyRow = new QHBoxLayout;
    bodyRow->setSpacing(10);

    sendsColumn_ = new QWidget(this);
    auto* sendColumn = new QVBoxLayout(sendsColumn_);
    sendColumn->setContentsMargins(0, 0, 0, 0);
    sendColumn->setSpacing(6);

    sendHeader_ = new QWidget(sendsColumn_);
    auto* sendHeader = new QHBoxLayout(sendHeader_);
    sendHeader->setContentsMargins(0, 0, 0, 0);
    auto* sendTitle = new QLabel("SENDS INTO THIS CHANNEL", sendHeader_);
    sendTitle->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    sendHeader->addWidget(sendTitle);
    sendCountLabel_ = new QLabel(sendHeader_);
    sendCountLabel_->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    sendHeader->addWidget(sendCountLabel_);
    sendHeader->addStretch(1);
    addSinkButton_ = new QPushButton("Add sink...", sendHeader_);
    connect(addSinkButton_, &QPushButton::clicked, this, &DetailPanel::addInputRequested);
    sendHeader->addWidget(addSinkButton_);
    sendColumn->addWidget(sendHeader_);

    sendArea_ = new QScrollArea(sendsColumn_);
    sendArea_->setWidgetResizable(true);
    sendArea_->setFrameShape(QFrame::NoFrame);
    sendArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* sendContent = new QWidget(sendArea_);
    sendLayout_ = new QHBoxLayout(sendContent);
    sendLayout_->setContentsMargins(0, 0, 0, 0);
    sendLayout_->setSpacing(5);
    sendLayout_->addStretch(1);
    sendArea_->setWidget(sendContent);
    // The send strips grow with the panel, the same way the mixer strips grow
    // with the window - but within reason, for the same reason: past this a
    // fader is a tall thin sliver with its label stranded at the far end, and
    // it is no easier to aim. The surplus height goes to the EQ curve beside
    // it, which is the one thing here that genuinely reads better large.
    sendArea_->setMinimumHeight(190);
    sendArea_->setMaximumHeight(400);
    sendColumn->addWidget(sendArea_, 1);
    // Keeps the header and strips at the TOP once the send area hits its height
    // cap. Without it the column's own maximum height stops short of the row's,
    // and a horizontal layout centres a widget that cannot fill - which floated
    // the sends header into the middle of the panel, detached from its strips.
    sendColumn->addStretch(0);
    bodyRow->addWidget(sendsColumn_);

    auto* eqColumn = new QVBoxLayout;
    eqColumn->setSpacing(6);
    auto* eqTitle = new QLabel("EQ", this);
    eqTitle->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    eqColumn->addWidget(eqTitle);
    eqPreview_ = new EqPreview(this);
    eqPreview_->setMinimumHeight(160);
    connect(eqPreview_, &EqPreview::activated, this,
            [this] { emit eqEditRequested(stripId_); });
    eqColumn->addWidget(eqPreview_, 1);
    bodyRow->addLayout(eqColumn, 1);

    // The body takes the slack. It used to be followed by a stretch, which
    // pinned sends and EQ to their minimum heights and turned every extra pixel
    // of window height into a band of empty space above the mixer row.
    root->addLayout(bodyRow, 1);
}

// The sends column is sized to its content so it leaves no gap, but it must not
// be allowed to crowd the EQ out on a narrow window - hence the share cap. The
// header row is a floor: "SENDS INTO THIS CHANNEL  N/M used  [Add sink...]" is
// wider than one or two strips.
void DetailPanel::updateSendsWidth() {
    if (!sendsColumn_ || !sendArea_ || !sendArea_->widget()) {
        return;
    }
    const int strips = sendArea_->widget()->sizeHint().width();
    const int header = sendHeader_ ? sendHeader_->sizeHint().width() : 0;
    const int wanted = std::max(strips, header);
    const int share = std::max(320, width() * 3 / 5);
    sendsColumn_->setMaximumWidth(std::min(wanted, share));
}

void DetailPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateSendsWidth();
}

void DetailPanel::setSelection(const QString& stripId) {
    stripId_ = stripId;
    if (const StripRow* strip = state_->findStrip(stripId)) {
        // Both are async; the panel redraws when they land.
        state_->requestChannelDetail(strip->outputId, strip->channelIndex);
        state_->requestOutputSends(strip->outputId);
    }
    rebuildSends();
    updateHeader();
    updateEqPreview();
}

void DetailPanel::updateHeader() {
    const StripRow* strip = state_->findStrip(stripId_);
    const bool have = strip != nullptr;

    positionButton_->setEnabled(have);
    renameButton_->setEnabled(have);
    autoConnectButton_->setEnabled(have);
    addSinkButton_->setEnabled(have);

    if (!have) {
        title_->setText(state_->isAvailable() ? "No channel selected" : "Not connected");
        subtitle_->clear();
        positionButton_->setText("-");
        return;
    }

    const QString name = strip->channelName.isEmpty() ? strip->position : strip->channelName;
    title_->setText(QString("%1 · %2").arg(strip->outputName, name));

    QString status = strip->deviceName;
    if (strip->connected && !strip->driven) {
        status += "  ·  channel not offered by the device's current profile";
    } else if (!strip->connected) {
        status += strip->autoConnect ? "  ·  waiting for the device" : "  ·  auto-connect off";
    }
    if (!strip->groupId.isEmpty()) {
        status += "  ·  linked (gain, mute, sends and EQ move together)";
    }
    subtitle_->setText(status);

    suppressSignals_ = true;
    autoConnectButton_->setChecked(strip->autoConnect);
    positionButton_->setText(strip->position.isEmpty() ? QStringLiteral("-") : strip->position);
    suppressSignals_ = false;
}

void DetailPanel::choosePosition() {
    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip) {
        return;
    }
    const QString outputId = strip->outputId;
    const uint32_t channelIndex = strip->channelIndex;
    const QString currentPosition = strip->position;

    // Offer everything the device advertises, plus the channel's current
    // position even if the device stopped advertising it - otherwise a profile
    // change makes the position it is actually set to unrepresentable.
    QVector<QString> devicePositions;
    if (const DeviceRow* device = state_->findDevice(strip->deviceName)) {
        devicePositions = device->positions;
    }
    QVector<QString> offered = devicePositions;
    if (!currentPosition.isEmpty() && !offered.contains(currentPosition)) {
        offered.push_back(currentPosition);
    }

    QVector<PositionSelector::Entry> entries;
    for (const QString& position : offered) {
        PositionSelector::Entry entry;
        entry.position = position;
        entry.availableOnDevice = devicePositions.contains(position);
        for (const StripRow& sibling : state_->strips()) {
            if (sibling.outputId == outputId && sibling.channelIndex != channelIndex &&
                sibling.position == position) {
                entry.takenBy = sibling.channelName.isEmpty() ? sibling.position
                                                              : sibling.channelName;
            }
        }
        entries.push_back(entry);
    }

    const QString chosen = PositionSelector::choose(this, entries, currentPosition);
    if (chosen.isEmpty() || chosen == currentPosition) {
        return;
    }

    // If a sibling already drives it, SWAP rather than leaving two channels
    // claiming the same position - which would silently make one of them
    // unreachable by the mix planner.
    for (const StripRow& sibling : state_->strips()) {
        if (sibling.outputId == outputId && sibling.channelIndex != channelIndex &&
            sibling.position == chosen) {
            state_->setChannelPosition(outputId, sibling.channelIndex, currentPosition);
            break;
        }
    }
    state_->setChannelPosition(outputId, channelIndex, chosen);
}

void DetailPanel::renameChannel() {
    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip) {
        return;
    }
    const QString outputId = strip->outputId;
    const uint32_t channelIndex = strip->channelIndex;

    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, "Rename channel",
        QString("Label for %1 (leave empty to show just the position):").arg(strip->position),
        QLineEdit::Normal, strip->channelName, &accepted);
    if (!accepted) {
        return;
    }
    state_->renameChannel(outputId, channelIndex, name.trimmed());
}

void DetailPanel::renameOutput() {
    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip) {
        return;
    }
    // Copied before the dialog, for the same reason as renameChannel above:
    // QInputDialog runs a nested event loop, and a snapshot arriving during it
    // replaces the vector `strip` points into.
    const QString outputId = strip->outputId;
    const QString currentName = strip->outputName;

    bool accepted = false;
    const QString name = QInputDialog::getText(this, "Rename output", "Name for this output:",
                                                QLineEdit::Normal, currentName, &accepted);
    if (accepted && !name.trimmed().isEmpty()) {
        state_->renameOutput(outputId, name.trimmed());
    }
}

void DetailPanel::connectSendStrip(SendStrip* strip) {
    connect(strip, &SendStrip::routedToggled, this, [this, strip](bool routed) {
        const StripRow* selected = state_->findStrip(stripId_);
        if (!selected) {
            return;
        }
        if (routed) {
            state_->setSend(selected->outputId, selected->channelIndex, strip->inputId(), 0.0);
            state_->requestOutputSends(selected->outputId);
        } else {
            state_->removeSend(selected->outputId, selected->channelIndex, strip->inputId());
        }
    });
    connect(strip, &SendStrip::levelEditBegan, this, [this, strip] {
        const StripRow* selected = state_->findStrip(stripId_);
        if (!selected) {
            return;
        }
        state_->beginEdit(EditKey{selected->id + ":" + strip->inputId(), Field::Send, -1});
    });
    connect(strip, &SendStrip::levelChanging, this, [this, strip](double gainDb) {
        if (const StripRow* selected = state_->findStrip(stripId_)) {
            state_->setSend(selected->outputId, selected->channelIndex, strip->inputId(), gainDb);
        }
    });
    connect(strip, &SendStrip::levelEditFinished, this, [this, strip] {
        const StripRow* selected = state_->findStrip(stripId_);
        if (!selected) {
            return;
        }
        state_->endEdit(EditKey{selected->id + ":" + strip->inputId(), Field::Send, -1});
    });
}

void DetailPanel::rebuildSends() {
    QHash<QString, SendStrip*> kept;
    for (const InputRow& input : state_->inputs()) {
        SendStrip* strip = sendStrips_.value(input.id, nullptr);
        if (!strip) {
            strip = new SendStrip(&state_->meters(), sendArea_->widget());
            connectSendStrip(strip);
            // Before the trailing stretch.
            sendLayout_->insertWidget(sendLayout_->count() - 1, strip);
        }
        strip->setInput(input);
        strip->show();
        kept.insert(input.id, strip);
    }
    for (auto it = sendStrips_.begin(); it != sendStrips_.end(); ++it) {
        if (!kept.contains(it.key())) {
            it.value()->deleteLater();
        }
    }
    sendStrips_ = kept;
    updateSendsWidth();
    refreshValues();
}

void DetailPanel::refreshValues() {
    updateHeader();

    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip) {
        for (auto it = sendStrips_.begin(); it != sendStrips_.end(); ++it) {
            it.value()->setSend(false, 0.0);
            it.value()->setCanRoute(false);
        }
        sendCountLabel_->clear();
        return;
    }

    const QVector<QPair<QString, double>> sends =
        state_->channelSends(strip->outputId, strip->channelIndex);
    const int routedInputs = state_->routedInputCount(strip->outputId);
    const int limit = state_->maxSendsPerOutput();
    const bool outputFull = routedInputs >= limit;

    for (auto it = sendStrips_.begin(); it != sendStrips_.end(); ++it) {
        const QString inputId = it.key();
        const auto found = std::find_if(sends.begin(), sends.end(), [&](const auto& send) {
            return send.first == inputId;
        });
        const bool routed = found != sends.end();
        it.value()->setSend(routed, routed ? found->second : 0.0);
        // The slot limit counts DISTINCT inputs per output, so an input already
        // routed to some other channel of this output already holds a slot and
        // can be switched on here even when the output is otherwise full.
        const bool holdsSlot = state_->inputOccupiesSlot(strip->outputId, inputId);
        it.value()->setCanRoute(routed || holdsSlot || !outputFull);
    }

    sendCountLabel_->setText(QString("%1/%2 used").arg(routedInputs).arg(limit));
    addSinkButton_->setToolTip(
        outputFull ? QString("This output already sends from %1 inputs, which is the limit.")
                          .arg(limit)
                   : QString());

    updateEqPreview();
}

void DetailPanel::updateEqPreview() {
    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip) {
        eqPreview_->setBands({});
        eqPreview_->setCaption(QString());
        return;
    }

    const QVector<eqcore::EqBand> cached = state_->channelBands(strip->outputId, strip->channelIndex);
    eqPreview_->setBands(std::vector<eqcore::EqBand>(cached.begin(), cached.end()));

    QString caption = cached.isEmpty()
                           ? QStringLiteral("no EQ on this channel")
                           : QString("%1 band%2").arg(cached.size()).arg(cached.size() == 1 ? "" : "s");
    // Name the channels this curve also applies to. Linked channels share one
    // curve, and that is invisible until someone notices both moved.
    if (!strip->groupId.isEmpty()) {
        QStringList siblings;
        for (const StripRow& other : state_->strips()) {
            if (other.outputId == strip->outputId && other.groupId == strip->groupId &&
                other.id != strip->id) {
                siblings << (other.channelName.isEmpty() ? other.position : other.channelName);
            }
        }
        if (!siblings.isEmpty()) {
            caption += QString("  ·  shared with %1").arg(siblings.join(", "));
        }
    }
    eqPreview_->setCaption(caption);
}

void DetailPanel::refreshMeters() {
    for (auto it = sendStrips_.begin(); it != sendStrips_.end(); ++it) {
        SendStrip* strip = it.value();
        if (!strip->isVisible() || strip->visibleRegion().isEmpty()) {
            continue;
        }
        strip->refreshMeters();
    }
}

} // namespace pipeeq
