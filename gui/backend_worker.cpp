#include "backend_worker.h"

#include <algorithm>

#include <QThread>

#include "dbus_client.h"
#include "fake_backend.h"

namespace pipeeq {

BackendWorker::BackendWorker(bool demo, QObject* parent) : QObject(parent), demo_(demo) {}

BackendWorker::~BackendWorker() = default;

void BackendWorker::initialize() {
    if (demo_) {
        backend_ = std::make_unique<FakeBackend>();
    } else {
        backend_ = std::make_unique<DbusClient>();
    }

    // Direct connections: both objects live on this thread, and the store
    // receives these across the thread boundary via its own queued connections.
    connect(backend_.get(), &Backend::outputChanged, this, &BackendWorker::daemonOutputChanged);
    connect(backend_.get(), &Backend::outputsChanged, this, &BackendWorker::daemonOutputsChanged);
    connect(backend_.get(), &Backend::inputsChanged, this, &BackendWorker::daemonInputsChanged);
    connect(backend_.get(), &Backend::devicesChanged, this, &BackendWorker::daemonDevicesChanged);
    connect(backend_.get(), &Backend::metersReceived, this, &BackendWorker::metersReceived);

    emit availabilityChanged(backend_->isAvailable());
}

void BackendWorker::requestSnapshot() {
    if (!backend_) {
        return;
    }

    DaemonSnapshot snapshot;
    snapshot.available = backend_->isAvailable();
    for (const DeviceRow& device : backend_->listDevices()) {
        snapshot.devices.push_back(device);
    }
    for (const StripRow& strip : backend_->listStrips()) {
        snapshot.strips.push_back(strip);
    }
    snapshot.maxSendsPerOutput = backend_->maxSendsPerOutput();
    for (const InputRow& input : backend_->listInputs()) {
        snapshot.inputs.push_back(input);
    }
    emit snapshotReady(snapshot);
}

void BackendWorker::requestChannelDetail(const QString& outputId, uint32_t channelIndex) {
    if (!backend_) {
        return;
    }
    QVector<eqcore::EqBand> bands;
    for (const eqcore::EqBand& band : backend_->getChannelEqBands(outputId, channelIndex)) {
        bands.push_back(band);
    }
    emit channelDetailReady(outputId, channelIndex, bands);
}

void BackendWorker::requestOutputSends(const QString& outputId) {
    if (!backend_) {
        return;
    }
    emit outputSendsReady(outputId, backend_->getOutputSends(outputId));
}

void BackendWorker::applyWrites(const QVector<WriteOp>& ops) {
    if (!backend_) {
        return;
    }

    bool allOk = true;
    QString error;
    quint64 seq = 0;

    for (const WriteOp& op : ops) {
        seq = std::max(seq, op.seq);
        bool ok = true;
        switch (op.kind) {
        case WriteOp::Kind::ChannelGain:
            ok = backend_->setChannelGain(op.outputId, op.channelIndex, op.doubleArg);
            break;
        case WriteOp::Kind::ChannelMute:
            ok = backend_->setChannelMuted(op.outputId, op.channelIndex, op.boolArg);
            break;
        case WriteOp::Kind::ChannelPosition:
            ok = backend_->setChannelPosition(op.outputId, op.channelIndex, op.stringArg);
            break;
        case WriteOp::Kind::OutputAutoConnect:
            ok = backend_->setOutputAutoConnect(op.outputId, op.boolArg);
            break;
        case WriteOp::Kind::ChannelEqBandCount:
            ok = backend_->setChannelEqBandCount(op.outputId, op.channelIndex, op.uintArg);
            break;
        case WriteOp::Kind::ChannelEqBand:
            ok = backend_->setChannelEqBand(op.outputId, op.channelIndex, op.uintArg, op.stringArg,
                                             op.doubleArg, op.doubleArg2, op.doubleArg3);
            break;
        case WriteOp::Kind::Send:
            ok = backend_->setSend(op.outputId, op.channelIndex, op.stringArg, op.doubleArg);
            break;
        case WriteOp::Kind::RemoveSend:
            ok = backend_->removeSend(op.outputId, op.channelIndex, op.stringArg);
            break;
        }
        if (!ok) {
            allOk = false;
            if (error.isEmpty()) {
                error = QString("the daemon refused a change to %1").arg(op.outputId);
            }
        }
    }

    emit writesCompleted(seq, allOk, error);
}

void BackendWorker::setMeteringEnabled(bool enabled) {
    if (backend_) {
        backend_->setMeteringEnabled(enabled);
    }
}

void BackendWorker::addOutput(const QString& deviceName, const QString& displayName) {
    if (!backend_) {
        return;
    }
    backend_->addOutput(deviceName, displayName);
    requestSnapshot();
}

void BackendWorker::removeOutput(const QString& outputId) {
    if (!backend_) {
        return;
    }
    backend_->removeOutput(outputId);
    requestSnapshot();
}

void BackendWorker::addInput(const QString& displayName) {
    if (!backend_) {
        return;
    }
    backend_->addInput(displayName);
    requestSnapshot();
}

void BackendWorker::removeInput(const QString& inputId) {
    if (!backend_) {
        return;
    }
    backend_->removeInput(inputId);
    requestSnapshot();
}

void BackendWorker::createLinkGroup(const QString& outputId, const QVector<uint32_t>& channels) {
    if (!backend_) {
        return;
    }
    backend_->createLinkGroup(outputId, channels, QString());
    requestSnapshot();
    requestOutputSends(outputId);
}

void BackendWorker::removeLinkGroup(const QString& outputId, const QString& groupId) {
    if (!backend_) {
        return;
    }
    backend_->removeLinkGroup(outputId, groupId);
    requestSnapshot();
    requestOutputSends(outputId);
}

void BackendWorker::setLinkGroupChannels(const QString& outputId, const QString& groupId,
                                          const QVector<uint32_t>& channels) {
    if (!backend_) {
        return;
    }
    backend_->setLinkGroupChannels(outputId, groupId, channels);
    requestSnapshot();
    requestOutputSends(outputId);
}

} // namespace pipeeq
