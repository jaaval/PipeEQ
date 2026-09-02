#include "fake_backend.h"

#include <algorithm>
#include <cmath>

#include <QSet>

#include <nlohmann/json.hpp>

#include "app_config.h"

namespace pipeeq {

namespace {

QString stripId(const QString& outputId, uint32_t channelIndex) {
    return outputId + "#" + QString::number(channelIndex);
}

eqcore::FilterType filterTypeFromString(const QString& type) {
    try {
        return nlohmann::json(type.toStdString()).get<eqcore::FilterType>();
    } catch (const std::exception&) {
        return eqcore::FilterType::Peaking;
    }
}

} // namespace

FakeBackend::FakeBackend(QObject* parent) : Backend(parent) {
    devices_ = {
        DeviceRow{41, "alsa_output.pci-0000_00_1f.3.analog-surround-51", "Desktop 5.1 (SoundBlaster)",
                   {"FL", "FR", "FC", "LFE", "RL", "RR"}, true},
        DeviceRow{57, "alsa_output.usb-Focusrite_Scarlett_4i4.analog-surround-40",
                   "Scarlett 4i4 (Analog Surround 4.0)", {"FL", "FR", "RL", "RR"}, true},
        DeviceRow{63, "alsa_output.pci-0000_01_00.1.hdmi-stereo", "GB203 HDMI Digital Stereo",
                   {"FL", "FR"}, false},
    };

    inputs_ = {
        InputRow{"input-1", "Music", {"FL", "FR", "FC", "LFE", "RL", "RR"}},
        InputRow{"input-2", "Voice Chat", {"FL", "FR"}},
        InputRow{"input-3", "Game", {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"}},
        InputRow{"input-4", "System", {"FL", "FR"}},
    };
    nextInputIndex_ = 5;

    // The 5.1 card: mains linked, centre and LFE on their own, rears linked -
    // exactly the shape the per-channel EQ work exists to make possible.
    Output desktop;
    desktop.id = "output-1";
    desktop.deviceName = devices_[0].nodeName;
    desktop.displayName = "Desktop 5.1";
    for (const auto& [position, name, gainDb, groupId] :
         std::vector<std::tuple<QString, QString, double, QString>>{
             {"FL", "Mains L", 0.0, "group-1"},
             {"FR", "Mains R", 0.0, "group-1"},
             {"FC", "Centre", -1.5, ""},
             {"LFE", "Subwoofer", 3.0, ""},
             {"RL", "Surround L", -2.0, "group-2"},
             {"RR", "Surround R", -2.0, "group-2"},
         }) {
        Channel channel;
        channel.position = position;
        channel.displayName = name;
        channel.gainDb = gainDb;
        channel.groupId = groupId;
        channel.sendsDb = {{"input-1", 0.0}, {"input-2", -6.0}, {"input-3", -12.0}};
        desktop.channels.push_back(std::move(channel));
    }
    // Distinct curves per channel group, so the UI has something real to draw.
    desktop.channels[0].bands = desktop.channels[1].bands = {
        {eqcore::FilterType::LowShelf, 90.0, 2.5, 0.707},
        {eqcore::FilterType::Peaking, 3150.0, -3.5, 1.8},
    };
    desktop.channels[2].bands = {{eqcore::FilterType::HighPass, 90.0, 0.0, 0.707}};
    desktop.channels[3].bands = {{eqcore::FilterType::LowPass, 80.0, 0.0, 0.707}};
    desktop.channels[4].bands = desktop.channels[5].bands = {
        {eqcore::FilterType::Peaking, 220.0, -2.0, 1.2},
    };
    outputs_.push_back(std::move(desktop));

    Output scarlett;
    scarlett.id = "output-2";
    scarlett.deviceName = devices_[1].nodeName;
    scarlett.displayName = "Scarlett 4i4";
    for (const QString& position : {"FL", "FR", "RL", "RR"}) {
        Channel channel;
        channel.position = position;
        channel.sendsDb = {{"input-2", 0.0}};
        scarlett.channels.push_back(std::move(channel));
    }
    scarlett.channels[0].groupId = scarlett.channels[1].groupId = "group-1";
    // One channel the device's current profile no longer offers, so the
    // connected-but-not-driven state is visible somewhere in the demo.
    scarlett.channels[3].retired = true;
    outputs_.push_back(std::move(scarlett));

    // An absent device, so the "waiting" presentation is always on screen
    // somewhere - it's the state most likely to be left broken otherwise.
    Output headphones;
    headphones.id = "output-3";
    headphones.deviceName = "alsa_output.usb-Sennheiser_HD650_Amp.analog-stereo";
    headphones.displayName = "HD650 Amp";
    headphones.connected = false;
    for (const QString& position : {"FL", "FR"}) {
        Channel channel;
        channel.position = position;
        channel.gainDb = -4.0;
        channel.sendsDb = {{"input-1", 0.0}};
        headphones.channels.push_back(std::move(channel));
    }
    headphones.channels[0].groupId = headphones.channels[1].groupId = "group-1";
    outputs_.push_back(std::move(headphones));
    nextOutputIndex_ = 4;

    meterTimer_.setInterval(33);
    connect(&meterTimer_, &QTimer::timeout, this, &FakeBackend::emitMeters);
}

// ------------------------------------------------------------------ lookups --

FakeBackend::Output* FakeBackend::findOutput(const QString& outputId) {
    auto it = std::find_if(outputs_.begin(), outputs_.end(),
                            [&](const Output& o) { return o.id == outputId; });
    return it == outputs_.end() ? nullptr : &*it;
}

FakeBackend::Channel* FakeBackend::findChannel(const QString& outputId, uint32_t channelIndex) {
    Output* output = findOutput(outputId);
    if (!output || channelIndex >= output->channels.size()) {
        return nullptr;
    }
    return &output->channels[channelIndex];
}

std::vector<FakeBackend::Channel*> FakeBackend::linkedChannels(const QString& outputId,
                                                                uint32_t channelIndex) {
    Output* output = findOutput(outputId);
    if (!output || channelIndex >= output->channels.size()) {
        return {};
    }
    const QString groupId = output->channels[channelIndex].groupId;
    if (groupId.isEmpty()) {
        return {&output->channels[channelIndex]};
    }
    std::vector<Channel*> members;
    for (Channel& channel : output->channels) {
        if (channel.groupId == groupId) {
            members.push_back(&channel);
        }
    }
    return members;
}

// ------------------------------------------------------------------ readers --

std::vector<DeviceRow> FakeBackend::listDevices() {
    return devices_;
}

std::vector<StripRow> FakeBackend::listStrips() {
    std::vector<StripRow> result;
    for (const Output& output : outputs_) {
        for (std::size_t i = 0; i < output.channels.size(); ++i) {
            const Channel& channel = output.channels[i];
            StripRow strip;
            strip.outputId = output.id;
            strip.channelIndex = static_cast<uint32_t>(i);
            strip.id = stripId(output.id, strip.channelIndex);
            strip.deviceName = output.deviceName;
            strip.outputName = output.displayName;
            strip.position = channel.position;
            strip.channelName = channel.displayName;
            strip.gainDb = channel.gainDb;
            strip.muted = channel.muted;
            strip.bandCount = static_cast<uint32_t>(channel.bands.size());
            strip.connected = output.connected;
            // Per channel, as the daemon reports it: a channel the device's
            // current profile doesn't offer is connected-but-not-driven. The
            // demo topology marks the retired channels so the "CH N/A"
            // presentation is actually reachable here.
            strip.driven = output.connected && !channel.retired;
            strip.autoConnect = output.autoConnect;
            strip.groupId = channel.groupId;
            result.push_back(std::move(strip));
        }
    }
    return result;
}

std::vector<InputRow> FakeBackend::listInputs() {
    return inputs_;
}

std::vector<eqcore::EqBand> FakeBackend::getChannelEqBands(const QString& outputId,
                                                            uint32_t channelIndex) {
    const Channel* channel = findChannel(outputId, channelIndex);
    return channel ? channel->bands : std::vector<eqcore::EqBand>{};
}

QVector<SendEntry> FakeBackend::getOutputSends(const QString& outputId) {
    QVector<SendEntry> result;
    const Output* output = findOutput(outputId);
    if (!output) {
        return result;
    }
    for (std::size_t i = 0; i < output->channels.size(); ++i) {
        for (const auto& [inputId, gainDb] : output->channels[i].sendsDb) {
            result.push_back(SendEntry{static_cast<uint32_t>(i), inputId, gainDb});
        }
    }
    return result;
}

// ----------------------------------------------------------------- mutations --

QString FakeBackend::addOutput(const QString& deviceName, const QString& displayName) {
    auto device = std::find_if(devices_.begin(), devices_.end(),
                                [&](const DeviceRow& d) { return d.nodeName == deviceName; });

    Output output;
    output.id = QString("output-%1").arg(nextOutputIndex_++);
    output.deviceName = deviceName;
    output.displayName = displayName.isEmpty() ? deviceName : displayName;
    output.connected = device != devices_.end();
    if (device != devices_.end()) {
        for (const QString& position : device->positions) {
            Channel channel;
            channel.position = position;
            for (const InputRow& input : inputs_) {
                channel.sendsDb[input.id] = 0.0;
            }
            output.channels.push_back(std::move(channel));
        }
    }
    outputs_.push_back(std::move(output));
    emit outputsChanged();
    return outputs_.back().id;
}

void FakeBackend::removeOutput(const QString& outputId) {
    outputs_.erase(std::remove_if(outputs_.begin(), outputs_.end(),
                                   [&](const Output& o) { return o.id == outputId; }),
                    outputs_.end());
    emit outputsChanged();
}

bool FakeBackend::setOutputAutoConnect(const QString& outputId, bool autoConnect) {
    Output* output = findOutput(outputId);
    if (!output) {
        return false;
    }
    output->autoConnect = autoConnect;
    emit outputChanged(outputId);
    return true;
}

bool FakeBackend::setChannelGain(const QString& outputId, uint32_t channelIndex, double gainDb) {
    const std::vector<Channel*> members = linkedChannels(outputId, channelIndex);
    if (members.empty()) {
        return false;
    }
    for (Channel* channel : members) {
        channel->gainDb = gainDb;
    }
    emit outputChanged(outputId);
    return true;
}

bool FakeBackend::setChannelMuted(const QString& outputId, uint32_t channelIndex, bool muted) {
    const std::vector<Channel*> members = linkedChannels(outputId, channelIndex);
    if (members.empty()) {
        return false;
    }
    for (Channel* channel : members) {
        channel->muted = muted;
    }
    emit outputChanged(outputId);
    return true;
}

bool FakeBackend::setChannelPosition(const QString& outputId, uint32_t channelIndex,
                                      const QString& position) {
    Channel* channel = findChannel(outputId, channelIndex);
    if (!channel) {
        return false;
    }
    channel->position = position;
    emit outputChanged(outputId);
    return true;
}

// EQ follows the LINK GROUP, exactly as the daemon does: linked channels share
// one curve. Editing only the addressed channel meant that in --demo, linking a
// pair and editing the curve left the partner untouched - so the editor's
// "shared with ... (linked)" note was simply false against the fake, and UI
// developed against it would have been wrong against the daemon.
bool FakeBackend::setChannelEqBandCount(const QString& outputId, uint32_t channelIndex,
                                         uint32_t count) {
    const std::vector<Channel*> members = linkedChannels(outputId, channelIndex);
    if (members.empty()) {
        return false;
    }
    for (Channel* channel : members) {
        channel->bands.resize(std::min<std::size_t>(count, eqcore::kMaxBands));
    }
    emit outputChanged(outputId);
    return true;
}

bool FakeBackend::setChannelEqBand(const QString& outputId, uint32_t channelIndex, uint32_t index,
                                    const QString& type, double freqHz, double gainDb, double q) {
    const std::vector<Channel*> members = linkedChannels(outputId, channelIndex);
    if (members.empty() || index >= members.front()->bands.size()) {
        return false;
    }
    const eqcore::EqBand band{filterTypeFromString(type), freqHz, gainDb, q};
    for (Channel* channel : members) {
        if (index < channel->bands.size()) {
            channel->bands[index] = band;
        }
    }
    emit outputChanged(outputId);
    return true;
}

QString FakeBackend::addInput(const QString& displayName) {
    InputRow input;
    input.id = QString("input-%1").arg(nextInputIndex_++);
    input.displayName = displayName;
    input.positions = {"FL", "FR"};
    inputs_.push_back(std::move(input));
    emit inputsChanged();
    return inputs_.back().id;
}

void FakeBackend::removeInput(const QString& inputId) {
    inputs_.erase(std::remove_if(inputs_.begin(), inputs_.end(),
                                  [&](const InputRow& i) { return i.id == inputId; }),
                   inputs_.end());
    for (Output& output : outputs_) {
        for (Channel& channel : output.channels) {
            channel.sendsDb.erase(inputId);
        }
    }
    emit inputsChanged();
}

bool FakeBackend::setSend(const QString& outputId, uint32_t channelIndex, const QString& inputId,
                           double gainDb) {
    const std::vector<Channel*> members = linkedChannels(outputId, channelIndex);
    if (members.empty()) {
        return false;
    }

    // The daemon bounds the DISTINCT inputs one output can send from, and
    // refuses past it. The fake has to refuse too, or the UI's "N/8 used" and
    // its disabled switch cannot be exercised in demo mode.
    Output* output = findOutput(outputId);
    if (output) {
        QSet<QString> routed;
        for (const Channel& channel : output->channels) {
            for (const auto& [id, level] : channel.sendsDb) {
                routed.insert(id);
            }
        }
        if (!routed.contains(inputId) && routed.size() >= maxSendsPerOutput()) {
            return false;
        }
    }
    for (Channel* channel : members) {
        channel->sendsDb[inputId] = gainDb;
    }
    emit outputChanged(outputId);
    return true;
}

bool FakeBackend::removeSend(const QString& outputId, uint32_t channelIndex,
                              const QString& inputId) {
    const std::vector<Channel*> members = linkedChannels(outputId, channelIndex);
    if (members.empty()) {
        return false;
    }
    for (Channel* channel : members) {
        channel->sendsDb.erase(inputId);
    }
    emit outputChanged(outputId);
    return true;
}

// ------------------------------------------------------------------ metering --

void FakeBackend::adoptLeader(Output& output, const std::vector<uint32_t>& members) {
    if (members.empty()) {
        return;
    }
    const std::size_t leader = *std::min_element(members.begin(), members.end());
    if (leader >= output.channels.size()) {
        return;
    }
    const Channel source = output.channels[leader];
    for (uint32_t index : members) {
        if (index >= output.channels.size() || index == leader) {
            continue;
        }
        Channel& target = output.channels[index];
        target.gainDb = source.gainDb;
        target.muted = source.muted;
        target.sendsDb = source.sendsDb;
        // Sharing the curve is what linking means; a copy here stands in for the
        // daemon's shared instance.
        target.bands = source.bands;
    }
}

void FakeBackend::splitEq(Output& output, const std::vector<uint32_t>& members) {
    // The fake stores bands per channel, so they are already separate copies -
    // nothing to do beyond making the intent explicit. The real daemon has to
    // clone a shared instance here.
    Q_UNUSED(output);
    Q_UNUSED(members);
}

bool FakeBackend::setOutputDisplayName(const QString& outputId, const QString& displayName) {
    Output* output = findOutput(outputId);
    if (!output) {
        return false;
    }
    output->displayName = displayName.isEmpty() ? output->deviceName : displayName;
    emit outputChanged(outputId);
    return true;
}

bool FakeBackend::setChannelDisplayName(const QString& outputId, uint32_t channelIndex,
                                         const QString& displayName) {
    Channel* channel = findChannel(outputId, channelIndex);
    if (!channel) {
        return false;
    }
    channel->displayName = displayName;
    emit outputChanged(outputId);
    return true;
}

bool FakeBackend::setInputDisplayName(const QString& inputId, const QString& displayName) {
    for (InputRow& input : inputs_) {
        if (input.id == inputId) {
            input.displayName = displayName;
            emit inputsChanged();
            return true;
        }
    }
    return false;
}

QString FakeBackend::createLinkGroup(const QString& outputId, const QVector<uint32_t>& channels,
                                      const QString& displayName) {
    Output* output = findOutput(outputId);
    if (!output) {
        return {};
    }

    std::vector<uint32_t> members(channels.begin(), channels.end());
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    if (members.size() < 2) {
        return {};
    }
    for (uint32_t index : members) {
        if (index >= output->channels.size() || !output->channels[index].groupId.isEmpty()) {
            return {}; // out of range, or already in another group
        }
    }

    const QString groupId = QString("group-%1").arg(nextGroupIndex_++);
    for (uint32_t index : members) {
        output->channels[index].groupId = groupId;
    }
    adoptLeader(*output, members);
    Q_UNUSED(displayName);
    emit outputChanged(outputId);
    return groupId;
}

bool FakeBackend::removeLinkGroup(const QString& outputId, const QString& groupId) {
    Output* output = findOutput(outputId);
    if (!output) {
        return false;
    }
    std::vector<uint32_t> members;
    for (std::size_t i = 0; i < output->channels.size(); ++i) {
        if (output->channels[i].groupId == groupId) {
            members.push_back(static_cast<uint32_t>(i));
        }
    }
    if (members.empty()) {
        return false;
    }
    for (uint32_t index : members) {
        output->channels[index].groupId.clear();
    }
    splitEq(*output, members);
    emit outputChanged(outputId);
    return true;
}

bool FakeBackend::setLinkGroupChannels(const QString& outputId, const QString& groupId,
                                        const QVector<uint32_t>& channels) {
    Output* output = findOutput(outputId);
    if (!output) {
        return false;
    }
    std::vector<uint32_t> members(channels.begin(), channels.end());
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    if (members.size() < 2) {
        return false;
    }
    for (uint32_t index : members) {
        if (index >= output->channels.size()) {
            return false;
        }
        const QString owner = output->channels[index].groupId;
        if (!owner.isEmpty() && owner != groupId) {
            return false;
        }
    }
    for (Channel& channel : output->channels) {
        if (channel.groupId == groupId) {
            channel.groupId.clear();
        }
    }
    for (uint32_t index : members) {
        output->channels[index].groupId = groupId;
    }
    adoptLeader(*output, members);
    emit outputChanged(outputId);
    return true;
}

void FakeBackend::setMeteringEnabled(bool enabled) {
    if (enabled) {
        meterTimer_.start();
    } else {
        meterTimer_.stop();
    }
}

void FakeBackend::emitMeters() {
    meterPhase_ += 0.033;

    QVector<MeterRow> outputMeters;
    for (const Output& output : outputs_) {
        if (!output.connected) {
            continue; // an absent device shows no signal, hatched rather than empty
        }
        MeterRow meter;
        meter.id = output.id;
        for (std::size_t i = 0; i < output.channels.size(); ++i) {
            const Channel& channel = output.channels[i];
            if (channel.muted) {
                meter.peaksDb.push_back(-144.0);
                continue;
            }
            // Two detuned sinusoids per channel, offset by index: enough to
            // look like independent programme material rather than a row of
            // bars moving in lockstep.
            const double slow = std::sin(meterPhase_ * 1.7 + static_cast<double>(i) * 0.9);
            const double fast = std::sin(meterPhase_ * 6.1 + static_cast<double>(i) * 2.3);
            const double normalized = 0.55 + 0.30 * slow + 0.12 * fast;
            // Peaks at about -3 dBFS rather than above 0: the demo should look
            // like healthy programme material, not like something permanently
            // clipping, or the clip indicator conveys nothing.
            const double peakDb = -40.0 + 37.0 * std::clamp(normalized, 0.0, 1.0);
            meter.peaksDb.push_back(peakDb + channel.gainDb * 0.5);
        }
        outputMeters.push_back(std::move(meter));
    }

    QVector<MeterRow> inputMeters;
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        MeterRow meter;
        meter.id = inputs_[i].id;
        const double slow = std::sin(meterPhase_ * 2.3 + static_cast<double>(i) * 1.6);
        for (int channel = 0; channel < inputs_[i].positions.size(); ++channel) {
            const double jitter = std::sin(meterPhase_ * 7.7 + static_cast<double>(channel));
            meter.peaksDb.push_back(-30.0 + 24.0 * std::clamp(0.5 + 0.4 * slow + 0.1 * jitter, 0.0, 1.0));
        }
        inputMeters.push_back(std::move(meter));
    }

    emit metersReceived(outputMeters, inputMeters);
}

} // namespace pipeeq
