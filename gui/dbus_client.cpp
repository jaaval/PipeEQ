#include "dbus_client.h"

#include <QDebug>

#include "app_config.h"
#include "dbus_interface.h"

namespace pipeeq {

namespace {

using DeviceChannelStructRow = sdbus::Struct<uint32_t, std::string>;
using DeviceStructRow =
    sdbus::Struct<uint32_t, std::string, std::string, std::vector<DeviceChannelStructRow>, bool>;
using OutputStructRow =
    sdbus::Struct<std::string, std::string, std::string, bool, bool, uint32_t, uint32_t, uint32_t>;
using ChannelStructRow =
    sdbus::Struct<uint32_t, std::string, std::string, double, bool, std::string, std::string, bool>;
using BandStructRow = sdbus::Struct<std::string, double, double, double>;
using InputStructRow = sdbus::Struct<std::string, std::string, std::vector<std::string>>;
using SendStructRow = sdbus::Struct<uint32_t, std::string, double>;

QString stripId(const QString& outputId, uint32_t channelIndex) {
    return outputId + "#" + QString::number(channelIndex);
}

} // namespace

DbusClient::DbusClient(QObject* parent) : QObject(parent) {
    // Explicit session-bus connection rather than sdbus-c++'s ambiguous
    // "default bus" resolution, which (like plain `busctl` with no --user flag)
    // can resolve to the system bus in some environments - the daemon only ever
    // registers on the session bus.
#if SDBUSCPP_MAJOR_VERSION >= 2
    proxy_ = sdbus::createProxy(sdbus::createSessionBusConnection(),
                                 sdbus::ServiceName{eqcore::dbus::kServiceName},
                                 sdbus::ObjectPath{eqcore::dbus::kObjectPath});
#else
    proxy_ = sdbus::createProxy(sdbus::createSessionBusConnection(),
                                 std::string(eqcore::dbus::kServiceName),
                                 std::string(eqcore::dbus::kObjectPath));
#endif

    proxy_->uponSignal(eqcore::dbus::kSignalOutputChanged)
        .onInterface(eqcore::dbus::kInterfaceName)
        .call([this](const std::string& outputId) {
            emit outputChanged(QString::fromStdString(outputId));
        });

    proxy_->uponSignal(eqcore::dbus::kSignalOutputsChanged)
        .onInterface(eqcore::dbus::kInterfaceName)
        .call([this] { emit outputsChanged(); });

    proxy_->uponSignal(eqcore::dbus::kSignalInputsChanged)
        .onInterface(eqcore::dbus::kInterfaceName)
        .call([this] { emit inputsChanged(); });

    // The daemon actually emits this now: reconciliation and D-Bus dispatch
    // share one thread, so the reconciler can write the connection. Previously
    // the signal existed but was never sent, and the window had to poll.
    proxy_->uponSignal(eqcore::dbus::kSignalDevicesChanged)
        .onInterface(eqcore::dbus::kInterfaceName)
        .call([this] { emit devicesChanged(); });
}

DbusClient::~DbusClient() = default;

std::vector<DeviceRow> DbusClient::listDevices() {
    std::vector<DeviceRow> result;
    try {
        std::vector<DeviceStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodListDevices)
            .onInterface(eqcore::dbus::kInterfaceName)
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            DeviceRow device;
            device.id = std::get<0>(r);
            device.nodeName = QString::fromStdString(std::get<1>(r));
            device.description = QString::fromStdString(std::get<2>(r));
            for (const auto& channel : std::get<3>(r)) {
                device.positions.push_back(QString::fromStdString(std::get<1>(channel)));
            }
            device.inUse = std::get<4>(r);
            result.push_back(std::move(device));
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: ListDevices failed: %s", e.what());
    }
    return result;
}

std::vector<StripRow> DbusClient::listStrips() {
    std::vector<StripRow> result;
    try {
        std::vector<OutputStructRow> outputs;
        proxy_->callMethod(eqcore::dbus::kMethodListOutputs)
            .onInterface(eqcore::dbus::kInterfaceName)
            .storeResultsTo(outputs);

        for (const auto& output : outputs) {
            const QString outputId = QString::fromStdString(std::get<0>(output));

            std::vector<ChannelStructRow> channels;
            proxy_->callMethod(eqcore::dbus::kMethodGetOutputChannels)
                .onInterface(eqcore::dbus::kInterfaceName)
                .withArguments(std::get<0>(output))
                .storeResultsTo(channels);

            for (const auto& channel : channels) {
                StripRow strip;
                strip.outputId = outputId;
                strip.channelIndex = std::get<0>(channel);
                strip.id = stripId(outputId, strip.channelIndex);
                strip.deviceName = QString::fromStdString(std::get<1>(output));
                strip.outputName = QString::fromStdString(std::get<2>(output));
                strip.connected = std::get<3>(output);
                strip.autoConnect = std::get<4>(output);
                strip.position = QString::fromStdString(std::get<1>(channel));
                strip.channelName = QString::fromStdString(std::get<2>(channel));
                strip.gainDb = std::get<3>(channel);
                strip.muted = std::get<4>(channel);
                strip.groupId = QString::fromStdString(std::get<6>(channel));
                strip.driven = std::get<7>(channel);

                std::vector<BandStructRow> bands;
                proxy_->callMethod(eqcore::dbus::kMethodGetChannelEqBands)
                    .onInterface(eqcore::dbus::kInterfaceName)
                    .withArguments(std::get<0>(output), strip.channelIndex)
                    .storeResultsTo(bands);
                strip.bandCount = static_cast<uint32_t>(bands.size());

                result.push_back(std::move(strip));
            }
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: listing outputs failed: %s", e.what());
    }
    return result;
}

QString DbusClient::addOutput(const QString& deviceName, const QString& displayName) {
    try {
        std::string outputId;
        proxy_->callMethod(eqcore::dbus::kMethodAddOutput)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(deviceName.toStdString(), displayName.toStdString())
            .storeResultsTo(outputId);
        return QString::fromStdString(outputId);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: AddOutput failed: %s", e.what());
        return {};
    }
}

void DbusClient::removeOutput(const QString& outputId) {
    try {
        proxy_->callMethod(eqcore::dbus::kMethodRemoveOutput)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString());
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: RemoveOutput failed: %s", e.what());
    }
}

bool DbusClient::setOutputAutoConnect(const QString& outputId, bool autoConnect) {
    try {
        bool ok = false;
        proxy_->callMethod(eqcore::dbus::kMethodSetOutputAutoConnect)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), autoConnect)
            .storeResultsTo(ok);
        return ok;
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetOutputAutoConnect failed: %s", e.what());
        return false;
    }
}

bool DbusClient::setChannelGain(const QString& outputId, uint32_t channelIndex, double gainDb) {
    try {
        bool ok = false;
        proxy_->callMethod(eqcore::dbus::kMethodSetChannelGain)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), channelIndex, gainDb)
            .storeResultsTo(ok);
        return ok;
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetChannelGain failed: %s", e.what());
        return false;
    }
}

bool DbusClient::setChannelMuted(const QString& outputId, uint32_t channelIndex, bool muted) {
    try {
        bool ok = false;
        proxy_->callMethod(eqcore::dbus::kMethodSetChannelMuted)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), channelIndex, muted)
            .storeResultsTo(ok);
        return ok;
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetChannelMuted failed: %s", e.what());
        return false;
    }
}

bool DbusClient::setChannelPosition(const QString& outputId, uint32_t channelIndex,
                                     const QString& position) {
    try {
        bool ok = false;
        proxy_->callMethod(eqcore::dbus::kMethodSetChannelPosition)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), channelIndex, position.toStdString())
            .storeResultsTo(ok);
        return ok;
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetChannelPosition failed: %s", e.what());
        return false;
    }
}

std::vector<eqcore::EqBand> DbusClient::getChannelEqBands(const QString& outputId,
                                                           uint32_t channelIndex) {
    std::vector<eqcore::EqBand> result;
    try {
        std::vector<BandStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodGetChannelEqBands)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), channelIndex)
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            eqcore::EqBand band;
            band.type = nlohmann::json(std::get<0>(r)).get<eqcore::FilterType>();
            band.freqHz = std::get<1>(r);
            band.gainDb = std::get<2>(r);
            band.q = std::get<3>(r);
            result.push_back(band);
        }
    } catch (const std::exception& e) {
        qWarning("pipeeq-gui: GetChannelEqBands failed: %s", e.what());
    }
    return result;
}

bool DbusClient::setChannelEqBandCount(const QString& outputId, uint32_t channelIndex,
                                        uint32_t count) {
    try {
        bool ok = false;
        proxy_->callMethod(eqcore::dbus::kMethodSetChannelEqBandCount)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), channelIndex, count)
            .storeResultsTo(ok);
        return ok;
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetChannelEqBandCount failed: %s", e.what());
        return false;
    }
}

bool DbusClient::setChannelEqBand(const QString& outputId, uint32_t channelIndex, uint32_t index,
                                   const QString& type, double freqHz, double gainDb, double q) {
    try {
        bool ok = false;
        proxy_->callMethod(eqcore::dbus::kMethodSetChannelEqBand)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), channelIndex, index, type.toStdString(), freqHz,
                            gainDb, q)
            .storeResultsTo(ok);
        return ok;
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetChannelEqBand failed: %s", e.what());
        return false;
    }
}

std::vector<InputRow> DbusClient::listInputs() {
    std::vector<InputRow> result;
    try {
        std::vector<InputStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodListInputs)
            .onInterface(eqcore::dbus::kInterfaceName)
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            InputRow input;
            input.id = QString::fromStdString(std::get<0>(r));
            input.displayName = QString::fromStdString(std::get<1>(r));
            for (const std::string& position : std::get<2>(r)) {
                input.positions.push_back(QString::fromStdString(position));
            }
            result.push_back(std::move(input));
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: ListInputs failed: %s", e.what());
    }
    return result;
}

QString DbusClient::addInput(const QString& displayName) {
    try {
        std::string inputId;
        // An empty layout means stereo, which is what the old GUI always made.
        proxy_->callMethod(eqcore::dbus::kMethodAddInput)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(displayName.toStdString(), std::vector<std::string>{})
            .storeResultsTo(inputId);
        return QString::fromStdString(inputId);
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: AddInput failed: %s", e.what());
        return {};
    }
}

void DbusClient::removeInput(const QString& inputId) {
    try {
        proxy_->callMethod(eqcore::dbus::kMethodRemoveInput)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(inputId.toStdString());
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: RemoveInput failed: %s", e.what());
    }
}

bool DbusClient::setSend(const QString& outputId, uint32_t channelIndex, const QString& inputId,
                          double gainDb) {
    try {
        bool ok = false;
        proxy_->callMethod(eqcore::dbus::kMethodSetSend)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), channelIndex, inputId.toStdString(), gainDb)
            .storeResultsTo(ok);
        return ok;
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: SetSend failed: %s", e.what());
        return false;
    }
}

bool DbusClient::removeSend(const QString& outputId, uint32_t channelIndex, const QString& inputId) {
    try {
        bool ok = false;
        proxy_->callMethod(eqcore::dbus::kMethodRemoveSend)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString(), channelIndex, inputId.toStdString())
            .storeResultsTo(ok);
        return ok;
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: RemoveSend failed: %s", e.what());
        return false;
    }
}

std::vector<std::pair<QString, double>> DbusClient::getChannelSends(const QString& outputId,
                                                                     uint32_t channelIndex) {
    std::vector<std::pair<QString, double>> result;
    try {
        std::vector<SendStructRow> rows;
        proxy_->callMethod(eqcore::dbus::kMethodGetSends)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId.toStdString())
            .storeResultsTo(rows);
        for (const auto& r : rows) {
            if (std::get<0>(r) != channelIndex) {
                continue;
            }
            result.emplace_back(QString::fromStdString(std::get<1>(r)), std::get<2>(r));
        }
    } catch (const sdbus::Error& e) {
        qWarning("pipeeq-gui: GetSends failed: %s", e.what());
    }
    return result;
}

} // namespace pipeeq
