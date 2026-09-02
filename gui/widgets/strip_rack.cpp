#include "strip_rack.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFontMetrics>
#include <QScrollBar>
#include <QVBoxLayout>

#include <QApplication>
#include <QMessageBox>

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

    // The owner sets the real height; this is only a floor for a bare instance.
    setMinimumHeight(150);
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

// Resolves a screen position to the strip under it, ignoring the one being
// dragged from. Done by geometry rather than by Qt's drag-and-drop because the
// gesture is a press-drag-release on one custom widget, not a data transfer.
ChannelStrip* StripRack::stripAtGlobalPos(const QPoint& globalPos) const {
    for (auto it = stripWidgets_.begin(); it != stripWidgets_.end(); ++it) {
        ChannelStrip* strip = it.value();
        if (!strip->isVisible()) {
            continue;
        }
        const QRect global(strip->mapToGlobal(QPoint(0, 0)), strip->size());
        if (global.contains(globalPos)) {
            return strip;
        }
    }
    return nullptr;
}

void StripRack::applyLinkMarks() {
    for (auto it = stripWidgets_.begin(); it != stripWidgets_.end(); ++it) {
        it.value()->setMarkedForLink(linkMarks_.contains(it.key()));
    }
}

void StripRack::toggleLinkMark(ChannelStrip* strip) {
    const QString key = stripWidgets_.key(strip);
    if (key.isEmpty()) {
        return;
    }
    if (strip->strips().size() > 1) {
        emit statusMessage("That strip is already a linked group. Click its link badge to unlink.");
        return;
    }
    if (linkMarks_.contains(key)) {
        linkMarks_.remove(key);
    } else {
        linkMarks_.insert(key);
    }
    applyLinkMarks();
    emit statusMessage(linkMarks_.isEmpty()
                            ? QString()
                            : QString("%1 channel(s) marked - press L or click a link badge to link "
                                      "them.")
                                  .arg(linkMarks_.size()));
}

bool StripRack::linkChannels(ChannelStrip* anchor, const QVector<ChannelStrip*>& others) {
    if (!anchor || others.isEmpty()) {
        return false;
    }

    QVector<ChannelStrip*> all = others;
    all.push_back(anchor);

    const QString outputId = anchor->outputId();
    QVector<uint32_t> indices;
    for (ChannelStrip* strip : all) {
        // A group is per output: the daemon's link groups index into one
        // output's channel list, and two devices have no shared fader anyway.
        if (strip->outputId() != outputId) {
            emit statusMessage("Channels can only be linked within one output device.");
            return false;
        }
        if (strip->strips().size() > 1) {
            emit statusMessage("One of those strips is already a linked group; unlink it first.");
            return false;
        }
        indices.push_back(strip->primaryChannelIndex());
    }
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.size() < 2) {
        return false;
    }

    // Linking DISCARDS the non-leader channels' gain, mute, sends and EQ,
    // because the group adopts the lowest-index member's. That is not
    // recoverable by unlinking, so it gets confirmed rather than just done.
    const uint32_t leaderIndex = indices.front();
    QString leaderName;
    QStringList discarded;
    for (ChannelStrip* strip : all) {
        const StripRow& row = strip->strips().front();
        const QString name = row.channelName.isEmpty() ? row.position : row.channelName;
        if (row.channelIndex == leaderIndex) {
            leaderName = name;
        } else {
            discarded << name;
        }
    }

    const auto answer = QMessageBox::question(
        this, "Link channels",
        QString("Link %1 and %2?\n\nThey will share one fader, mute, set of sends and EQ curve, "
                "taken from %1. The current settings of %2 will be replaced.")
            .arg(leaderName, discarded.join(", ")),
        QMessageBox::Ok | QMessageBox::Cancel);
    if (answer != QMessageBox::Ok) {
        return false;
    }

    state_->linkChannels(outputId, indices);
    linkMarks_.clear();
    applyLinkMarks();
    emit statusMessage(QString("Linked %1 and %2.").arg(leaderName, discarded.join(", ")));
    return true;
}

void StripRack::linkMarkedChannels() {
    QVector<ChannelStrip*> marked;
    for (const QString& key : linkMarks_) {
        if (ChannelStrip* strip = stripWidgets_.value(key, nullptr)) {
            marked.push_back(strip);
        }
    }
    if (marked.size() < 2) {
        emit statusMessage("Ctrl+click two or more channels of the same output, then press L.");
        return;
    }
    // Lowest channel index is the anchor, matching the daemon's leader rule.
    std::sort(marked.begin(), marked.end(), [](ChannelStrip* a, ChannelStrip* b) {
        return a->primaryChannelIndex() < b->primaryChannelIndex();
    });
    ChannelStrip* anchor = marked.front();
    marked.removeFirst();
    linkChannels(anchor, marked);
}

void StripRack::clearLinkMarks() {
    linkMarks_.clear();
    applyLinkMarks();
    emit statusMessage(QString());
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

    connect(strip, &ChannelStrip::linkSelectionToggled, this,
            [this, strip] { toggleLinkMark(strip); });

    connect(strip, &ChannelStrip::linkDragStarted, this, [this, strip] {
        linkDragSource_ = strip;
        linkDropTarget_ = nullptr;
    });
    connect(strip, &ChannelStrip::linkDragMoved, this, [this](const QPoint& globalPos) {
        ChannelStrip* target = stripAtGlobalPos(globalPos);
        if (target == linkDragSource_) {
            target = nullptr;
        }
        if (target == linkDropTarget_) {
            return;
        }
        if (linkDropTarget_) {
            linkDropTarget_->setLinkDropTarget(false);
        }
        linkDropTarget_ = target;
        if (linkDropTarget_) {
            linkDropTarget_->setLinkDropTarget(true);
        }
    });
    connect(strip, &ChannelStrip::linkDragFinished, this, [this, strip](const QPoint& globalPos) {
        ChannelStrip* target = stripAtGlobalPos(globalPos);
        if (linkDropTarget_) {
            linkDropTarget_->setLinkDropTarget(false);
            linkDropTarget_ = nullptr;
        }
        linkDragSource_ = nullptr;

        if (target && target != strip) {
            // Dragged onto another strip: link the two.
            linkChannels(strip->primaryChannelIndex() <= target->primaryChannelIndex() ? strip
                                                                                        : target,
                          {strip->primaryChannelIndex() <= target->primaryChannelIndex() ? target
                                                                                          : strip});
            return;
        }

        // Released without moving: a plain click on the badge.
        const QVector<StripRow>& members = strip->strips();
        if (members.size() > 1) {
            // Unlinking is trivially reversible, so it needs no confirmation.
            state_->unlinkGroup(members.front().outputId, members.front().groupId);
            emit statusMessage("Unlinked. Each channel keeps its current settings and its own copy "
                                "of the EQ curve.");
            return;
        }
        // Badge clicks build a selection and then link it. Clicking an
        // already-marked strip just unmarks it - it must NOT then try to link,
        // which would attempt it with one fewer channel than the user has
        // marked and complain that there aren't enough.
        const QString key = stripWidgets_.key(strip);
        const bool wasMarked = linkMarks_.contains(key);
        toggleLinkMark(strip);
        if (!wasMarked && markedCount() >= 2) {
            linkMarkedChannels();
        }
    });
}

void StripRack::setCollapsedDevices(const QStringList& nodeNames) {
    collapsedDevices_ = QSet<QString>(nodeNames.begin(), nodeNames.end());
    rebuild();
}

QStringList StripRack::collapsedDevices() const {
    return QStringList(collapsedDevices_.begin(), collapsedDevices_.end());
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
        // Elided to a cap. A PipeWire description like "GB203 High Definition
        // Audio Controller Digital Stereo (HDMI)" is far wider than the strips
        // beneath it; left alone it stretched a single strip to several times
        // its natural width. The cap plus the trailing stretch below means a
        // long name widens the BLOCK a little but never the strips, and the
        // full name is in the tooltip.
        QFont headerFont = tokens.uiFont;
        headerFont.setBold(true);
        const QFontMetrics headerMetrics(headerFont);
        constexpr int kMaxHeaderPx = 210;
        auto* header = new QLabel(
            headerMetrics.elidedText(first.outputName, Qt::ElideRight, kMaxHeaderPx) + status,
            deviceBlock);
        header->setFont(headerFont);
        header->setToolTip(first.outputName + "\n" + first.deviceName);
        const QColor accent = tokens.accentForDevice(first.deviceName);
        header->setStyleSheet(
            QString("color: %1;").arg(first.connected ? accent.name() : tokens.textDisabled.name()));
        blockLayout->addWidget(header);

        // An absent device collapses to a single placeholder that says why, so a
        // rack with several unplugged interfaces isn't mostly ghost strips. The
        // strips are only hidden, never forgotten: the configuration is still
        // there and still editable once expanded.
        const bool collapsible = !first.connected;
        const bool collapsed = collapsible && collapsedDevices_.contains(first.deviceName);

        if (collapsible) {
            auto* toggle = new QPushButton(collapsed ? "Show channels" : "Hide", deviceBlock);
            toggle->setToolTip(collapsed
                                    ? "This device isn't present. Its channels are hidden."
                                    : "Hide this absent device's channels.");
            connect(toggle, &QPushButton::clicked, this, [this, name = first.deviceName] {
                if (collapsedDevices_.contains(name)) {
                    collapsedDevices_.remove(name);
                } else {
                    collapsedDevices_.insert(name);
                }
                rebuild();
                emit collapsedDevicesChanged();
            });
            blockLayout->addWidget(toggle);
        }

        auto* stripsRow = new QWidget(deviceBlock);
        deviceStripsLayout = new QHBoxLayout(stripsRow);
        deviceStripsLayout->setContentsMargins(0, 0, 0, 0);
        deviceStripsLayout->setSpacing(4);
        // Trailing stretch, so strips keep their natural width instead of
        // expanding to fill whatever the block happens to be.
        deviceStripsLayout->addStretch(1);
        stripsRow->setVisible(!collapsed);
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
        deviceStripsLayout->insertWidget(deviceStripsLayout->count() - 1, strip);
        strip->setVisible(deviceStripsLayout->parentWidget()->isVisibleTo(content_) ||
                           !collapsedDevices_.contains(cluster.deviceName));
        kept.insert(cluster.key, strip);
    }

    // Anything no longer present goes away.
    for (auto it = stripWidgets_.begin(); it != stripWidgets_.end(); ++it) {
        if (!kept.contains(it.key())) {
            it.value()->deleteLater();
        }
    }
    stripWidgets_ = kept;
    // Marks are keyed by cluster, so drop any whose cluster no longer exists.
    for (const QString& key : linkMarks_.values()) {
        if (!stripWidgets_.contains(key)) {
            linkMarks_.remove(key);
        }
    }
    applyLinkMarks();

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

    // If the CLUSTERING changed - a group formed or dissolved - the existing
    // widgets no longer correspond to anything, because a cluster key contains
    // the group id. Updating values into widgets found by key would then be a
    // silent no-op, which is exactly how linking managed to look broken.
    bool clusteringChanged = clusters.size() != stripWidgets_.size();
    if (!clusteringChanged) {
        for (const StripCluster& cluster : clusters) {
            if (!stripWidgets_.contains(cluster.key)) {
                clusteringChanged = true;
                break;
            }
        }
    }
    if (clusteringChanged) {
        rebuild();
        return;
    }

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
