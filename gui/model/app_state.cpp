#include "app_state.h"

#include <algorithm>

#include <QSet>

#include <nlohmann/json.hpp>

#include "app_config.h"

namespace pipeeq {

namespace {

QString filterTypeName(eqcore::FilterType type) {
    return QString::fromStdString(nlohmann::json(type).get<std::string>());
}

} // namespace

AppState::AppState(bool demo, QObject* parent) : QObject(parent) {
    qRegisterMetaType<DaemonSnapshot>();
    qRegisterMetaType<WriteOp>();
    qRegisterMetaType<QVector<WriteOp>>();
    qRegisterMetaType<QVector<MeterRow>>();
    qRegisterMetaType<QVector<eqcore::EqBand>>("QVector<eqcore::EqBand>");
    qRegisterMetaType<QVector<QPair<QString, double>>>("QVector<QPair<QString,double>>");
    qRegisterMetaType<SendEntry>();
    qRegisterMetaType<QVector<SendEntry>>("QVector<SendEntry>");
    qRegisterMetaType<QVector<uint32_t>>("QVector<uint32_t>");

    worker_ = new BackendWorker(demo);
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);

    connect(worker_, &BackendWorker::snapshotReady, this, &AppState::onSnapshotReady);
    connect(worker_, &BackendWorker::writesCompleted, this, &AppState::onWritesCompleted);
    connect(worker_, &BackendWorker::availabilityChanged, this, &AppState::availabilityChanged);
    connect(worker_, &BackendWorker::channelDetailReady, this,
            [this](const QString& outputId, uint32_t channelIndex,
                    const QVector<eqcore::EqBand>& bands) {
                // Held bands are being dragged right now, and the incoming copy
                // is from before the drag started. Merging per band rather than
                // dropping the whole reply, so an untouched band still updates.
                const QString stripId = outputId + "#" + QString::number(channelIndex);
                QVector<eqcore::EqBand> merged = bands;
                const QVector<eqcore::EqBand>& local =
                    details_[detailKey(outputId, channelIndex)].bands;
                for (int i = 0; i < merged.size() && i < local.size(); ++i) {
                    if (guard_.isHeld(EditKey{stripId, Field::EqBand, i})) {
                        merged[i] = local[i];
                    }
                }
                if (guard_.isHeld(EditKey{stripId, Field::EqBandCount, -1}) &&
                    merged.size() != local.size()) {
                    return; // a band add/remove is still in flight
                }
                details_[detailKey(outputId, channelIndex)].bands = merged;
                emit channelDetailUpdated(outputId, channelIndex);
            });
    connect(worker_, &BackendWorker::outputSendsReady, this,
            [this](const QString& outputId, const QVector<SendEntry>& sends) {
                QVector<SendEntry> merged = sends;
                const QVector<SendEntry>& local = sends_[outputId];
                for (SendEntry& entry : merged) {
                    const QString key = outputId + "#" + QString::number(entry.channelIndex) + ":" +
                                        entry.inputId;
                    if (!guard_.isHeld(EditKey{key, Field::Send, -1})) {
                        continue;
                    }
                    // Keep the level the user is dragging.
                    for (const SendEntry& existing : local) {
                        if (existing.channelIndex == entry.channelIndex &&
                            existing.inputId == entry.inputId) {
                            entry.gainDb = existing.gainDb;
                        }
                    }
                }
                sends_.insert(outputId, merged);
                emit sendsUpdated(outputId);
            });

    // Every daemon change notification becomes a resync request. Coarse, but
    // the reads are off-thread and cheap, and it removes any chance of the
    // cache and the daemon disagreeing about topology.
    connect(worker_, &BackendWorker::daemonOutputChanged, this, [this](const QString&) { refresh(); });
    connect(worker_, &BackendWorker::daemonOutputsChanged, this, &AppState::refresh);
    connect(worker_, &BackendWorker::daemonInputsChanged, this, &AppState::refresh);
    connect(worker_, &BackendWorker::daemonDevicesChanged, this, &AppState::refresh);
    connect(worker_, &BackendWorker::metersReceived, this,
            [this](const QVector<MeterRow>& outputs, const QVector<MeterRow>& inputs) {
                meters_.ingest(outputs, inputs);
            });

    // Routed through the store rather than straight to the worker, so it can
    // record which channels a batch touches and re-read them once the write has
    // actually landed.
    connect(&coalescer_, &WriteCoalescer::writesReady, this, &AppState::onWritesReady);

    workerThread_.start();
    QMetaObject::invokeMethod(worker_, &BackendWorker::initialize, Qt::QueuedConnection);
    refresh();

    resyncTimer_.setInterval(kResyncIntervalMs);
    connect(&resyncTimer_, &QTimer::timeout, this, &AppState::refresh);
    resyncTimer_.start();

    // The daemon's metering lease expires on its own, so a watcher has to
    // re-arm. That is deliberate: a GUI that crashes then stops metering with
    // no cleanup on either side.
    meterRearmTimer_.setInterval(kMeterRearmIntervalMs);
    connect(&meterRearmTimer_, &QTimer::timeout, this, [this] {
        if (meteringWanted_) {
            QMetaObject::invokeMethod(worker_, "setMeteringEnabled", Qt::QueuedConnection,
                                       Q_ARG(bool, true));
        }
    });
}

AppState::~AppState() {
    workerThread_.quit();
    workerThread_.wait();
}

QString AppState::detailKey(const QString& outputId, uint32_t channelIndex) const {
    return outputId + "#" + QString::number(channelIndex);
}

const StripRow* AppState::findStrip(const QString& stripId) const {
    auto it = std::find_if(snapshot_.strips.begin(), snapshot_.strips.end(),
                            [&](const StripRow& s) { return s.id == stripId; });
    return it == snapshot_.strips.end() ? nullptr : &*it;
}

const DeviceRow* AppState::findDevice(const QString& nodeName) const {
    auto it = std::find_if(snapshot_.devices.begin(), snapshot_.devices.end(),
                            [&](const DeviceRow& d) { return d.nodeName == nodeName; });
    return it == snapshot_.devices.end() ? nullptr : &*it;
}

QVector<eqcore::EqBand> AppState::channelBands(const QString& outputId, uint32_t channelIndex) const {
    const auto it = details_.constFind(detailKey(outputId, channelIndex));
    return it == details_.constEnd() ? QVector<eqcore::EqBand>{} : it->bands;
}

QVector<QPair<QString, double>> AppState::channelSends(const QString& outputId,
                                                        uint32_t channelIndex) const {
    QVector<QPair<QString, double>> result;
    const auto it = sends_.constFind(outputId);
    if (it == sends_.constEnd()) {
        return result;
    }
    for (const SendEntry& entry : *it) {
        if (entry.channelIndex == channelIndex) {
            result.push_back({entry.inputId, entry.gainDb});
        }
    }
    return result;
}

bool AppState::inputOccupiesSlot(const QString& outputId, const QString& inputId) const {
    const auto it = sends_.constFind(outputId);
    if (it == sends_.constEnd()) {
        return false;
    }
    return std::any_of(it->begin(), it->end(),
                        [&](const SendEntry& entry) { return entry.inputId == inputId; });
}

int AppState::routedInputCount(const QString& outputId) const {
    const auto it = sends_.constFind(outputId);
    if (it == sends_.constEnd()) {
        return 0;
    }
    QSet<QString> ids;
    for (const SendEntry& entry : *it) {
        ids.insert(entry.inputId);
    }
    return static_cast<int>(ids.size());
}

void AppState::requestChannelDetail(const QString& outputId, uint32_t channelIndex) {
    QMetaObject::invokeMethod(worker_, "requestChannelDetail", Qt::QueuedConnection,
                               Q_ARG(QString, outputId), Q_ARG(uint32_t, channelIndex));
}

void AppState::requestOutputSends(const QString& outputId) {
    QMetaObject::invokeMethod(worker_, "requestOutputSends", Qt::QueuedConnection,
                               Q_ARG(QString, outputId));
}

void AppState::refresh() {
    QMetaObject::invokeMethod(worker_, "requestSnapshot", Qt::QueuedConnection);
}

void AppState::onSnapshotReady(const DaemonSnapshot& snapshot) {
    // Grouping is part of the topology, not a value.
    //
    // Linking two channels leaves the set of strip IDS identical and only moves
    // their groupId - so keying the comparison on ids alone reported a mere
    // value update, and the rack, which groups linked channels into one widget,
    // had nothing to rebuild from. Linking and unlinking appeared to do nothing
    // at all.
    const auto stripIds = [](const QVector<StripRow>& strips) {
        QStringList ids;
        for (const StripRow& strip : strips) {
            ids << (strip.id + "/" + strip.groupId);
        }
        return ids;
    };
    const auto deviceNames = [](const QVector<DeviceRow>& devices) {
        QStringList names;
        for (const DeviceRow& device : devices) {
            names << device.nodeName;
        }
        return names;
    };
    const auto inputIds = [](const QVector<InputRow>& inputs) {
        QStringList ids;
        for (const InputRow& input : inputs) {
            ids << input.id;
        }
        return ids;
    };

    const bool topologyMoved = stripIds(snapshot_.strips) != stripIds(snapshot.strips) ||
                                deviceNames(snapshot_.devices) != deviceNames(snapshot.devices) ||
                                inputIds(snapshot_.inputs) != inputIds(snapshot.inputs);

    // Field-wise merge for anything currently being edited. A wholesale
    // assignment here is what would make a snapshot arriving mid-drag snap the
    // fader back to the daemon's older value.
    DaemonSnapshot merged = snapshot;
    for (StripRow& incoming : merged.strips) {
        const StripRow* local = findStrip(incoming.id);
        if (!local) {
            continue;
        }
        if (guard_.isHeld(EditKey{incoming.id, Field::Gain, -1})) {
            incoming.gainDb = local->gainDb;
        }
        if (guard_.isHeld(EditKey{incoming.id, Field::Mute, -1})) {
            incoming.muted = local->muted;
        }
        if (guard_.isHeld(EditKey{incoming.id, Field::Position, -1})) {
            incoming.position = local->position;
        }
        if (guard_.isHeld(EditKey{incoming.id, Field::AutoConnect, -1})) {
            incoming.autoConnect = local->autoConnect;
        }
    }

    snapshot_ = merged;

    // Anything whose hold has just expired had daemon values withheld from it
    // while it was held, so ask for the truth now. Without this a remote change
    // that arrived mid-drag stayed invisible until some later refresh happened
    // to occur - the header documented a "deferred value" mechanism that did
    // not exist, and this is the honest version of it.
    if (!guard_.takeExpiredKeys().isEmpty()) {
        refresh();
    }

    if (topologyMoved) {
        emit topologyChanged();
    } else {
        emit stripsUpdated();
    }
}

// ------------------------------------------------------------------- writes --

void AppState::beginEdit(const EditKey& key) {
    guard_.beginEdit(key);
}

void AppState::endEdit(const EditKey& key) {
    guard_.endEdit(key);
    // Flush on release so the final value of a drag lands immediately rather
    // than waiting out the coalescer's interval.
    coalescer_.flushNow();
}

void AppState::enqueue(const WriteOp& op, const EditKey& key, bool flushNow) {
    WriteOp stamped = op;
    stamped.seq = nextSeq_++;
    inFlight_[stamped.seq].push_back(key);
    guard_.noteWriteSent(key);

    coalescer_.enqueue(stamped);
    if (flushNow) {
        coalescer_.flushNow();
    }
}

void AppState::onWritesReady(const QVector<WriteOp>& ops) {
    quint64 highestSeq = 0;
    QVector<ChannelTarget> targets;
    for (const WriteOp& op : ops) {
        highestSeq = std::max(highestSeq, op.seq);
        const ChannelTarget target{op.outputId, op.channelIndex};
        if (!targets.contains(target)) {
            targets.push_back(target);
        }
    }
    if (highestSeq != 0) {
        inFlightTargets_.insert(highestSeq, targets);
    }

    QMetaObject::invokeMethod(worker_, "applyWrites", Qt::QueuedConnection,
                               Q_ARG(QVector<WriteOp>, ops));
}

void AppState::onWritesCompleted(quint64 seq, bool ok, const QString& error) {
    // Every batch with a sequence at or below this one has been applied.
    for (auto it = inFlight_.begin(); it != inFlight_.end();) {
        if (it.key() > seq) {
            ++it;
            continue;
        }
        for (const EditKey& key : *it) {
            guard_.noteWriteCompleted(key);
        }
        it = inFlight_.erase(it);
    }

    // Re-read whatever the batch touched. The optimistic local update keeps the
    // UI responsive during a drag, but it is a guess: only the backend knows
    // whether the value was clamped, refused, or applied to a whole link group.
    // Converging here is what stops a stale cache surviving indefinitely.
    for (auto it = inFlightTargets_.begin(); it != inFlightTargets_.end();) {
        if (it.key() > seq) {
            ++it;
            continue;
        }
        for (const ChannelTarget& target : *it) {
            if (target.outputId.isEmpty()) {
                continue;
            }
            requestChannelDetail(target.outputId, target.channelIndex);
            requestOutputSends(target.outputId);
        }
        it = inFlightTargets_.erase(it);
    }

    if (!ok) {
        // The optimistic local value is now wrong, so pull the truth rather
        // than leaving a control showing something the daemon rejected.
        emit errorReported(error);
        refresh();
    }
}

void AppState::setChannelGain(const QString& outputId, uint32_t channelIndex, double gainDb) {
    const QString stripId = outputId + "#" + QString::number(channelIndex);
    // Optimistic local update, so the UI tracks the pointer rather than the
    // round trip. The guard is what keeps it from being overwritten.
    for (StripRow& strip : snapshot_.strips) {
        if (strip.outputId == outputId &&
            (strip.channelIndex == channelIndex || !strip.groupId.isEmpty())) {
            // Linked channels move together daemon-side; mirror that locally so
            // a partner's fader doesn't lag a round trip behind.
            const StripRow* self = findStrip(stripId);
            if (self && !self->groupId.isEmpty() && strip.groupId == self->groupId) {
                strip.gainDb = gainDb;
            } else if (strip.channelIndex == channelIndex) {
                strip.gainDb = gainDb;
            }
        }
    }

    WriteOp op;
    op.kind = WriteOp::Kind::ChannelGain;
    op.outputId = outputId;
    op.channelIndex = channelIndex;
    op.doubleArg = gainDb;
    enqueue(op, EditKey{stripId, Field::Gain, -1}, /*flushNow=*/false);

    // Deliberately NOT emitting stripsUpdated here. This runs once per
    // mouse-move event, and that signal makes the window refresh every strip in
    // the rack plus the EQ preview - which re-evaluates the filter response per
    // pixel column. The strip being dragged already paints its own local value,
    // and the daemon's change notification converges everything else.
    emit channelValueChanged(outputId, channelIndex);
}

void AppState::setSend(const QString& outputId, uint32_t channelIndex, const QString& inputId,
                        double gainDb) {
    WriteOp op;
    op.kind = WriteOp::Kind::Send;
    op.outputId = outputId;
    op.channelIndex = channelIndex;
    op.stringArg = inputId;
    op.doubleArg = gainDb;

    const QString key = outputId + "#" + QString::number(channelIndex) + ":" + inputId;
    enqueue(op, EditKey{key, Field::Send, -1}, /*flushNow=*/false);

    // Optimistic: reflect the level locally so the fader tracks the pointer.
    for (SendEntry& entry : sends_[outputId]) {
        if (entry.channelIndex == channelIndex && entry.inputId == inputId) {
            entry.gainDb = gainDb;
        }
    }
}

void AppState::setChannelEqBand(const QString& outputId, uint32_t channelIndex, uint32_t bandIndex,
                                 const eqcore::EqBand& band) {
    ChannelDetail& detail = details_[detailKey(outputId, channelIndex)];
    if (static_cast<int>(bandIndex) < detail.bands.size()) {
        detail.bands[static_cast<int>(bandIndex)] = band;
    }

    WriteOp op;
    op.kind = WriteOp::Kind::ChannelEqBand;
    op.outputId = outputId;
    op.channelIndex = channelIndex;
    op.uintArg = bandIndex;
    op.stringArg = filterTypeName(band.type);
    op.doubleArg = band.freqHz;
    op.doubleArg2 = band.gainDb;
    op.doubleArg3 = band.q;

    const QString stripId = outputId + "#" + QString::number(channelIndex);
    enqueue(op, EditKey{stripId, Field::EqBand, static_cast<int>(bandIndex)}, /*flushNow=*/false);
}

void AppState::setChannelMuted(const QString& outputId, uint32_t channelIndex, bool muted) {
    WriteOp op;
    op.kind = WriteOp::Kind::ChannelMute;
    op.outputId = outputId;
    op.channelIndex = channelIndex;
    op.boolArg = muted;
    const QString stripId = outputId + "#" + QString::number(channelIndex);
    enqueue(op, EditKey{stripId, Field::Mute, -1}, /*flushNow=*/true);
}

void AppState::setChannelPosition(const QString& outputId, uint32_t channelIndex,
                                   const QString& position) {
    WriteOp op;
    op.kind = WriteOp::Kind::ChannelPosition;
    op.outputId = outputId;
    op.channelIndex = channelIndex;
    op.stringArg = position;
    const QString stripId = outputId + "#" + QString::number(channelIndex);
    enqueue(op, EditKey{stripId, Field::Position, -1}, /*flushNow=*/true);
}

void AppState::setOutputAutoConnect(const QString& outputId, bool autoConnect) {
    WriteOp op;
    op.kind = WriteOp::Kind::OutputAutoConnect;
    op.outputId = outputId;
    op.boolArg = autoConnect;
    enqueue(op, EditKey{outputId, Field::AutoConnect, -1}, /*flushNow=*/true);
}

void AppState::setChannelEqBandCount(const QString& outputId, uint32_t channelIndex,
                                      uint32_t count) {
    WriteOp op;
    op.kind = WriteOp::Kind::ChannelEqBandCount;
    op.outputId = outputId;
    op.channelIndex = channelIndex;
    op.uintArg = count;
    const QString stripId = outputId + "#" + QString::number(channelIndex);
    enqueue(op, EditKey{stripId, Field::EqBandCount, -1}, /*flushNow=*/true);
    requestChannelDetail(outputId, channelIndex);
}

void AppState::removeSend(const QString& outputId, uint32_t channelIndex, const QString& inputId) {
    WriteOp op;
    op.kind = WriteOp::Kind::RemoveSend;
    op.outputId = outputId;
    op.channelIndex = channelIndex;
    op.stringArg = inputId;
    const QString key = outputId + "#" + QString::number(channelIndex) + ":" + inputId;
    enqueue(op, EditKey{key, Field::Send, -1}, /*flushNow=*/true);
    requestOutputSends(outputId);
}

// ----------------------------------------------------------------- topology --

void AppState::addOutput(const QString& deviceName, const QString& displayName) {
    QMetaObject::invokeMethod(worker_, "addOutput", Qt::QueuedConnection,
                               Q_ARG(QString, deviceName), Q_ARG(QString, displayName));
}

void AppState::removeOutput(const QString& outputId) {
    QMetaObject::invokeMethod(worker_, "removeOutput", Qt::QueuedConnection,
                               Q_ARG(QString, outputId));
}

void AppState::addInput(const QString& displayName) {
    QMetaObject::invokeMethod(worker_, "addInput", Qt::QueuedConnection,
                               Q_ARG(QString, displayName));
}

void AppState::removeInput(const QString& inputId) {
    QMetaObject::invokeMethod(worker_, "removeInput", Qt::QueuedConnection,
                               Q_ARG(QString, inputId));
}

void AppState::renameOutput(const QString& outputId, const QString& displayName) {
    QMetaObject::invokeMethod(worker_, "setOutputDisplayName", Qt::QueuedConnection,
                               Q_ARG(QString, outputId), Q_ARG(QString, displayName));
}

void AppState::renameChannel(const QString& outputId, uint32_t channelIndex,
                              const QString& displayName) {
    QMetaObject::invokeMethod(worker_, "setChannelDisplayName", Qt::QueuedConnection,
                               Q_ARG(QString, outputId), Q_ARG(uint32_t, channelIndex),
                               Q_ARG(QString, displayName));
}

void AppState::renameInput(const QString& inputId, const QString& displayName) {
    QMetaObject::invokeMethod(worker_, "setInputDisplayName", Qt::QueuedConnection,
                               Q_ARG(QString, inputId), Q_ARG(QString, displayName));
}

void AppState::linkChannels(const QString& outputId, const QVector<uint32_t>& channelIndices) {
    QMetaObject::invokeMethod(worker_, "createLinkGroup", Qt::QueuedConnection,
                               Q_ARG(QString, outputId), Q_ARG(QVector<uint32_t>, channelIndices));
}

void AppState::unlinkGroup(const QString& outputId, const QString& groupId) {
    QMetaObject::invokeMethod(worker_, "removeLinkGroup", Qt::QueuedConnection,
                               Q_ARG(QString, outputId), Q_ARG(QString, groupId));
}

void AppState::setGroupChannels(const QString& outputId, const QString& groupId,
                                 const QVector<uint32_t>& channelIndices) {
    QMetaObject::invokeMethod(worker_, "setLinkGroupChannels", Qt::QueuedConnection,
                               Q_ARG(QString, outputId), Q_ARG(QString, groupId),
                               Q_ARG(QVector<uint32_t>, channelIndices));
}

QVector<uint32_t> AppState::groupChannels(const QString& outputId, const QString& groupId) const {
    QVector<uint32_t> indices;
    if (groupId.isEmpty()) {
        return indices;
    }
    for (const StripRow& strip : snapshot_.strips) {
        if (strip.outputId == outputId && strip.groupId == groupId) {
            indices.push_back(strip.channelIndex);
        }
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

void AppState::setMeteringEnabled(bool enabled) {
    meteringWanted_ = enabled;
    meters_.setActive(enabled);
    QMetaObject::invokeMethod(worker_, "setMeteringEnabled", Qt::QueuedConnection,
                               Q_ARG(bool, enabled));
    if (enabled) {
        meterRearmTimer_.start();
    } else {
        meterRearmTimer_.stop();
    }
}

} // namespace pipeeq
