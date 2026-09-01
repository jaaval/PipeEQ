#include "detail_panel.h"

#include <algorithm>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "eq_preview.h"
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

    subtitle_ = new QLabel(this);
    subtitle_->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    headerRow->addWidget(subtitle_, 1);

    headerRow->addWidget(new QLabel("Position:", this));
    positionCombo_ = new QComboBox(this);
    connect(positionCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (suppressSignals_ || index < 0) {
            return;
        }
        const StripRow* strip = state_->findStrip(stripId_);
        if (!strip) {
            return;
        }
        const QString position = positionCombo_->itemData(index).toString();
        if (position != strip->position) {
            state_->setChannelPosition(strip->outputId, strip->channelIndex, position);
        }
    });
    headerRow->addWidget(positionCombo_);

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
    auto* bodyRow = new QHBoxLayout;
    bodyRow->setSpacing(10);

    auto* sendColumn = new QVBoxLayout;
    auto* sendHeader = new QHBoxLayout;
    auto* sendTitle = new QLabel("SENDS INTO THIS CHANNEL", this);
    sendTitle->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    sendHeader->addWidget(sendTitle);
    sendCountLabel_ = new QLabel(this);
    sendCountLabel_->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    sendHeader->addWidget(sendCountLabel_);
    sendHeader->addStretch(1);
    addSinkButton_ = new QPushButton("Add sink...", this);
    connect(addSinkButton_, &QPushButton::clicked, this, &DetailPanel::addInputRequested);
    sendHeader->addWidget(addSinkButton_);
    sendColumn->addLayout(sendHeader);

    sendArea_ = new QScrollArea(this);
    sendArea_->setWidgetResizable(true);
    sendArea_->setFrameShape(QFrame::NoFrame);
    sendArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* sendContent = new QWidget(sendArea_);
    sendLayout_ = new QHBoxLayout(sendContent);
    sendLayout_->setContentsMargins(0, 0, 0, 0);
    sendLayout_->setSpacing(5);
    sendLayout_->addStretch(1);
    sendArea_->setWidget(sendContent);
    // Both bounds matter. The maximum keeps the detail area compact, since the
    // mixer row below is the primary surface; the minimum stops a layout with
    // spare stretch elsewhere from squeezing the strips down to a clipped sliver
    // of their content.
    sendArea_->setMinimumHeight(190);
    sendArea_->setMaximumHeight(240);
    sendColumn->addWidget(sendArea_, 1);
    bodyRow->addLayout(sendColumn, 3);

    auto* eqColumn = new QVBoxLayout;
    auto* eqTitle = new QLabel("EQ", this);
    eqTitle->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    eqColumn->addWidget(eqTitle);
    eqPreview_ = new EqPreview(this);
    eqPreview_->setMinimumHeight(160);
    eqPreview_->setMaximumHeight(240);
    connect(eqPreview_, &EqPreview::activated, this,
            [this] { emit eqEditRequested(stripId_); });
    eqColumn->addWidget(eqPreview_, 1);
    bodyRow->addLayout(eqColumn, 2);

    root->addLayout(bodyRow);
    root->addStretch(1);
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

    positionCombo_->setEnabled(have);
    autoConnectButton_->setEnabled(have);
    addSinkButton_->setEnabled(have);

    if (!have) {
        title_->setText(state_->isAvailable() ? "No channel selected" : "Not connected");
        subtitle_->clear();
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

    positionCombo_->clear();
    QVector<QString> options;
    if (const DeviceRow* device = state_->findDevice(strip->deviceName)) {
        options = device->positions;
    }
    // Always offer the channel's CURRENT position even if the device no longer
    // advertises it, so a profile change doesn't silently snap it elsewhere the
    // moment this is rebuilt.
    if (!strip->position.isEmpty() && !options.contains(strip->position)) {
        options.push_back(strip->position);
    }
    for (const QString& position : options) {
        positionCombo_->addItem(position, position);
    }
    const int current = positionCombo_->findData(strip->position);
    if (current >= 0) {
        positionCombo_->setCurrentIndex(current);
    }
    suppressSignals_ = false;
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
