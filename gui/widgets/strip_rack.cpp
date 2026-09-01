#include "strip_rack.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QVBoxLayout>

#include "channel_strip.h"
#include "model/app_state.h"
#include "theme/theme.h"

namespace pipeeq {

StripRack::StripRack(AppState* state, QWidget* parent) : QScrollArea(parent), state_(state) {
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);

    content_ = new QWidget(this);
    contentLayout_ = new QHBoxLayout(content_);
    contentLayout_->setContentsMargins(6, 6, 6, 6);
    contentLayout_->setSpacing(10);
    contentLayout_->addStretch(1);
    setWidget(content_);

    setMinimumHeight(270);
}

QVector<StripRack::StripCluster> StripRack::buildClusters() const {
    // Group by (output, link group). An ungrouped channel is its own cluster.
    QVector<StripCluster> clusters;
    QHash<QString, int> byGroupKey;

    for (const StripRow& strip : state_->strips()) {
        const QString groupKey =
            strip.groupId.isEmpty() ? (strip.id + ":solo") : (strip.outputId + ":" + strip.groupId);

        const auto existing = byGroupKey.constFind(groupKey);
        if (existing != byGroupKey.constEnd()) {
            clusters[*existing].members.push_back(strip);
            continue;
        }
        StripCluster cluster;
        cluster.key = groupKey;
        cluster.deviceName = strip.deviceName;
        cluster.members.push_back(strip);
        byGroupKey.insert(groupKey, clusters.size());
        clusters.push_back(std::move(cluster));
    }

    // Members in channel order, so a group's meters read left to right the way
    // the hardware is wired.
    for (StripCluster& cluster : clusters) {
        std::sort(cluster.members.begin(), cluster.members.end(),
                   [](const StripRow& a, const StripRow& b) {
                       return a.channelIndex < b.channelIndex;
                   });
    }
    // A group sits at its lowest member index, so linking two channels never
    // shuffles the rack.
    std::stable_sort(clusters.begin(), clusters.end(),
                      [](const StripCluster& a, const StripCluster& b) {
                          if (a.members.isEmpty() || b.members.isEmpty()) {
                              return false;
                          }
                          if (a.members.front().outputId != b.members.front().outputId) {
                              return a.members.front().outputId < b.members.front().outputId;
                          }
                          return a.members.front().channelIndex < b.members.front().channelIndex;
                      });
    return clusters;
}

void StripRack::connectStrip(ChannelStrip* strip) {
    connect(strip, &ChannelStrip::selectRequested, this, [this, strip] {
        setSelectedStripId(strip->primaryId());
        emit selectionChanged(strip->primaryId());
    });
    connect(strip, &ChannelStrip::gainEditBegan, this, [this, strip] {
        state_->beginEdit(EditKey{strip->primaryId(), Field::Gain, -1});
    });
    connect(strip, &ChannelStrip::gainChanging, this, [this, strip](double gainDb) {
        state_->setChannelGain(strip->outputId(), strip->primaryChannelIndex(), gainDb);
    });
    connect(strip, &ChannelStrip::gainEditFinished, this, [this, strip] {
        state_->endEdit(EditKey{strip->primaryId(), Field::Gain, -1});
    });
    connect(strip, &ChannelStrip::muteToggled, this, [this, strip](bool muted) {
        state_->setChannelMuted(strip->outputId(), strip->primaryChannelIndex(), muted);
    });
    connect(strip, &ChannelStrip::positionClicked, this,
            [this, strip] { emit positionClicked(strip->primaryId()); });
    connect(strip, &ChannelStrip::linkToggleRequested, this,
            [this, strip] { emit linkToggleRequested(strip->primaryId()); });
}

void StripRack::rebuild() {
    const theme::Tokens tokens = theme::tokens();
    const QVector<StripCluster> clusters = buildClusters();

    // Tear down the device blocks but keep the strip widgets: reparenting is
    // cheap, recreating them is what would drop a drag in progress.
    for (auto it = stripWidgets_.begin(); it != stripWidgets_.end(); ++it) {
        it.value()->setParent(nullptr);
    }
    for (QWidget* block : deviceBlocks_) {
        block->deleteLater();
    }
    deviceBlocks_.clear();

    QHash<QString, ChannelStrip*> kept;
    QString currentDevice;
    QWidget* deviceBlock = nullptr;
    QHBoxLayout* deviceStripsLayout = nullptr;

    const auto startDeviceBlock = [&](const StripCluster& cluster) {
        currentDevice = cluster.deviceName;
        const StripRow& first = cluster.members.front();

        deviceBlock = new QWidget(content_);
        auto* blockLayout = new QVBoxLayout(deviceBlock);
        blockLayout->setContentsMargins(6, 4, 6, 4);
        blockLayout->setSpacing(4);

        // Header: the device, plus why it isn't carrying audio when it isn't.
        QString status;
        if (!first.connected) {
            status = first.autoConnect ? " · WAITING" : " · OFF";
        } else if (!first.driven) {
            status = " · CH N/A";
        }
        auto* header = new QLabel(first.outputName + status, deviceBlock);
        QFont headerFont = tokens.uiFont;
        headerFont.setBold(true);
        header->setFont(headerFont);
        const QColor accent = tokens.accentForDevice(first.deviceName);
        header->setStyleSheet(
            QString("color: %1;").arg(first.connected ? accent.name() : tokens.textDisabled.name()));
        blockLayout->addWidget(header);

        auto* stripsRow = new QWidget(deviceBlock);
        deviceStripsLayout = new QHBoxLayout(stripsRow);
        deviceStripsLayout->setContentsMargins(0, 0, 0, 0);
        deviceStripsLayout->setSpacing(4);
        blockLayout->addWidget(stripsRow, 1);

        // Insert before the trailing stretch.
        contentLayout_->insertWidget(contentLayout_->count() - 1, deviceBlock);
        deviceBlocks_.push_back(deviceBlock);
    };

    for (const StripCluster& cluster : clusters) {
        if (cluster.members.isEmpty()) {
            continue;
        }
        if (cluster.deviceName != currentDevice || !deviceBlock) {
            startDeviceBlock(cluster);
        }

        ChannelStrip* strip = stripWidgets_.value(cluster.key, nullptr);
        if (!strip) {
            strip = new ChannelStrip(&state_->meters(), content_);
            connectStrip(strip);
        }
        strip->setStrips(cluster.members);
        strip->setAccentColor(tokens.accentForDevice(cluster.deviceName));
        strip->setSelected(cluster.members.front().id == selectedStripId_ ||
                            std::any_of(cluster.members.begin(), cluster.members.end(),
                                        [&](const StripRow& m) { return m.id == selectedStripId_; }));
        deviceStripsLayout->addWidget(strip);
        strip->show();
        kept.insert(cluster.key, strip);
    }

    // Anything no longer present goes away.
    for (auto it = stripWidgets_.begin(); it != stripWidgets_.end(); ++it) {
        if (!kept.contains(it.key())) {
            it.value()->deleteLater();
        }
    }
    stripWidgets_ = kept;

    // Keep a valid selection so the detail area is never left showing nothing.
    if (!selectedStripId_.isEmpty() && !state_->findStrip(selectedStripId_)) {
        selectedStripId_.clear();
    }
    if (selectedStripId_.isEmpty() && !state_->strips().isEmpty()) {
        setSelectedStripId(state_->strips().front().id);
        emit selectionChanged(selectedStripId_);
    }
}

void StripRack::refreshValues() {
    const QVector<StripCluster> clusters = buildClusters();
    for (const StripCluster& cluster : clusters) {
        if (ChannelStrip* strip = stripWidgets_.value(cluster.key, nullptr)) {
            strip->setStrips(cluster.members);
        }
    }
}

void StripRack::refreshMeters() {
    for (auto it = stripWidgets_.begin(); it != stripWidgets_.end(); ++it) {
        ChannelStrip* strip = it.value();
        // Skip anything scrolled out of view: a rack wider than the window must
        // not pay for meters nobody can see.
        if (!strip->isVisible() || strip->visibleRegion().isEmpty()) {
            continue;
        }
        strip->refreshMeters();
    }
}

void StripRack::setSelectedStripId(const QString& stripId) {
    selectedStripId_ = stripId;
    for (auto it = stripWidgets_.begin(); it != stripWidgets_.end(); ++it) {
        ChannelStrip* strip = it.value();
        const bool selected = std::any_of(
            strip->strips().begin(), strip->strips().end(),
            [&](const StripRow& member) { return member.id == stripId; });
        strip->setSelected(selected);
    }
}

} // namespace pipeeq
