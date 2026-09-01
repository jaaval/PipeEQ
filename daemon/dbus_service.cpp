#include "dbus_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <poll.h>

#include "app_config.h"
#include "config_io.h"
#include "dbus_interface.h"

namespace pipeeq {

namespace {

eqcore::FilterType filterTypeFromString(const std::string& s) {
    return nlohmann::json(s).get<eqcore::FilterType>();
}

std::string filterTypeToString(eqcore::FilterType t) {
    return nlohmann::json(t).get<std::string>();
}

// Peaks travel as dB so the GUI doesn't have to know the linear convention, and
// so a silent channel is an unmistakable sentinel rather than a very small
// number that might be a rounding artefact.
constexpr double kSilenceDb = -144.0;

double linearToDb(float linear) {
    if (!(linear > 0.0f)) {
        return kSilenceDb;
    }
    return std::max(kSilenceDb, 20.0 * std::log10(static_cast<double>(linear)));
}

std::chrono::steady_clock::duration nowDuration() {
    return std::chrono::steady_clock::now().time_since_epoch();
}

} // namespace

DbusService::DbusService(AudioEngine& engine) : engine_(engine) {}

DbusService::~DbusService() {
    stop();
}

bool DbusService::start() {
    // Migration from the v1 schema (and the synthesized default input for
    // configs predating the mixer feature) lives in eqcore::migrateV1, which
    // loadConfig() applies - and which backs the original file up before
    // anything can overwrite it.
    const eqcore::LoadResult loaded = eqcore::loadConfig();

    for (const std::string& warning : loaded.warnings) {
        std::fprintf(stderr, "pipeeq: config: %s\n", warning.c_str());
    }

    switch (loaded.status) {
    case eqcore::LoadStatus::Missing:
        std::fprintf(stderr, "pipeeq: no config at %s yet; starting fresh\n", loaded.path.c_str());
        break;
    case eqcore::LoadStatus::Loaded:
        std::fprintf(stderr, "pipeeq: loaded %zu output(s) and %zu input(s) from %s\n",
                     loaded.config.outputs.size(), loaded.config.inputs.size(), loaded.path.c_str());
        break;
    case eqcore::LoadStatus::Migrated:
        std::fprintf(stderr, "pipeeq: %s\n", loaded.message.c_str());
        std::fprintf(stderr, "pipeeq: migrated %zu output(s) and %zu input(s)\n",
                     loaded.config.outputs.size(), loaded.config.inputs.size());
        break;
    case eqcore::LoadStatus::Failed:
        // Deliberately keep serving rather than exiting - this is a
        // bus-activated service, and a daemon that refuses to start is worse UX
        // than one running with no outputs. But it must never write, or the
        // first slider move would overwrite a recoverable config with nothing.
        std::fprintf(stderr,
                     "pipeeq: REFUSING to use or overwrite the config at %s: %s\n"
                     "pipeeq: running with an empty configuration; nothing will be saved this "
                     "session, so the file stays exactly as it is. Fix or move it and restart.\n",
                     loaded.path.c_str(), loaded.message.c_str());
        break;
    }

    if (!loaded.persistable()) {
        std::lock_guard<std::mutex> lock(persistMutex_);
        persistDisabled_ = true;
    }

    // The engine restores inputs and outputs verbatim, keeping the saved ids
    // (so the mix levels keyed by input id stay valid) and keeping outputs
    // whose device isn't plugged in right now as pending rather than dropping
    // them.
    engine_.applyConfig(loaded.config);

    try {
#if SDBUSCPP_MAJOR_VERSION >= 2
        connection_ = sdbus::createSessionBusConnection(sdbus::ServiceName{eqcore::dbus::kServiceName});
        object_ = sdbus::createObject(*connection_, sdbus::ObjectPath{eqcore::dbus::kObjectPath});
#else
        connection_ = sdbus::createSessionBusConnection(eqcore::dbus::kServiceName);
        object_ = sdbus::createObject(*connection_, eqcore::dbus::kObjectPath);
#endif
        registerInterface();
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "pipeeq: cannot export %s: %s\n", eqcore::dbus::kServiceName, e.what());
        return false;
    }

    // A migration is only really complete once it's on disk in the new shape.
    if (loaded.status == eqcore::LoadStatus::Migrated) {
        markConfigDirty();
    }

    running_.store(true, std::memory_order_release);
    serviceThread_ = std::thread([this] { serviceLoop(); });

    std::printf("pipeeq-daemon: D-Bus service running as %s\n", eqcore::dbus::kServiceName);
    return true;
}

void DbusService::stop() {
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        if (serviceThread_.joinable()) {
            serviceThread_.join();
        }
    }

    // Anything the debounce window was still holding must reach disk before we
    // go away, or the last edit before a shutdown is silently lost.
    flushConfigIfDue(/*force=*/true);

    object_.reset();
    connection_.reset();
}

// This loop is the whole reason DbusService owns a thread: one thread both
// dispatches incoming calls and drives everything periodic, so reconciliation
// can emit signals. sdbus's own enterEventLoopAsync() would own the connection
// on a thread we can't schedule work onto.
void DbusService::serviceLoop() {
    auto lastReconcile = std::chrono::steady_clock::now();
    auto lastMeters = lastReconcile;

    while (running_.load(std::memory_order_acquire)) {
        // Drain anything already pending before sleeping.
        try {
            while (connection_->processPendingEvent()) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
            }
        } catch (const sdbus::Error& e) {
            std::fprintf(stderr, "pipeeq: D-Bus dispatch error: %s\n", e.what());
        }

        const auto pollData = connection_->getEventLoopPollData();
        int timeoutMs = pollData.getPollTimeout();
        // Never sleep longer than the shortest periodic job needs, and never
        // longer than the reconcile interval even when nothing is armed - that
        // interval is the worst-case delay before hardware that just appeared
        // starts playing.
        const int periodicMs =
            static_cast<int>((meterLeaseUntil_.load(std::memory_order_relaxed) > nowDuration()
                                   ? kMeterInterval
                                   : kReconcileInterval)
                                  .count());
        timeoutMs = timeoutMs < 0 ? periodicMs : std::min(timeoutMs, periodicMs);

        pollfd fds[2];
        fds[0] = {pollData.fd, pollData.events, 0};
        fds[1] = {pollData.eventFd, POLLIN, 0};
        ::poll(fds, 2, timeoutMs);

        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastReconcile >= kReconcileInterval) {
            lastReconcile = now;
            engine_.reconcile();
            flushConfigIfDue(/*force=*/false);

            // Now emittable at last: this used to be impossible because
            // reconciliation ran on a different thread from the one dispatching
            // the connection, so DevicesChanged was registered but never sent
            // and the GUI had to poll instead.
            if (engine_.consumeDevicesChanged()) {
                emitDevicesChanged();
            }
        }

        // Drain queued change notifications, coalesced per output.
        std::vector<std::string> outputChanges;
        bool inputsChanged = false;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            outputChanges.swap(pendingOutputChanges_);
            inputsChanged = pendingInputsChanged_;
            pendingInputsChanged_ = false;
        }
        std::sort(outputChanges.begin(), outputChanges.end());
        outputChanges.erase(std::unique(outputChanges.begin(), outputChanges.end()),
                             outputChanges.end());
        for (const std::string& outputId : outputChanges) {
            emitOutputChanged(outputId);
        }
        if (inputsChanged) {
            emitInputsChanged();
        }

        if (meterLeaseUntil_.load(std::memory_order_relaxed) > nowDuration() &&
            now - lastMeters >= kMeterInterval) {
            lastMeters = now;
            emitMeters();
        }
    }
}


// Two registration shapes because sdbus-c++ 2.0 was a breaking API rewrite;
// distro-shipped versions still vary (Arch has 2.x, Ubuntu 24.04 ships 1.x).
void DbusService::registerInterface() {
    using namespace eqcore::dbus;

#if SDBUSCPP_MAJOR_VERSION >= 2
    object_->addVTable(
        // ---- readers ----
        sdbus::registerMethod(kMethodListDevices).implementedAs([this] { return listDevices(); }),
        sdbus::registerMethod(kMethodListOutputs).implementedAs([this] { return listOutputs(); }),
        sdbus::registerMethod(kMethodGetOutputChannels)
            .implementedAs([this](std::string outputId) { return getOutputChannels(outputId); }),
        sdbus::registerMethod(kMethodListEqInstances)
            .implementedAs([this](std::string outputId) { return listEqInstances(outputId); }),
        sdbus::registerMethod(kMethodGetEqBands)
            .implementedAs([this](std::string outputId, std::string eqId) {
                return getEqBands(outputId, eqId);
            }),
        sdbus::registerMethod(kMethodGetChannelEqBands)
            .implementedAs([this](std::string outputId, uint32_t channelIndex) {
                return getChannelEqBands(outputId, channelIndex);
            }),
        sdbus::registerMethod(kMethodListLinkGroups)
            .implementedAs([this](std::string outputId) { return listLinkGroups(outputId); }),
        sdbus::registerMethod(kMethodGetSends)
            .implementedAs([this](std::string outputId) { return getSends(outputId); }),
        sdbus::registerMethod(kMethodListInputs).implementedAs([this] { return listInputs(); }),
        sdbus::registerMethod(kMethodGetState).implementedAs([this] { return getState(); }),

        // ---- outputs ----
        sdbus::registerMethod(kMethodAddOutput)
            .implementedAs([this](std::string deviceName, std::string displayName) {
                return addOutput(deviceName, displayName);
            }),
        sdbus::registerMethod(kMethodRemoveOutput)
            .implementedAs([this](std::string outputId) { removeOutput(outputId); }),
        sdbus::registerMethod(kMethodSetOutputDisplayName)
            .implementedAs([this](std::string outputId, std::string displayName) {
                return setOutputDisplayName(outputId, displayName);
            }),
        sdbus::registerMethod(kMethodSetOutputAutoConnect)
            .implementedAs([this](std::string outputId, bool autoConnect) {
                return setOutputAutoConnect(outputId, autoConnect);
            }),

        // ---- channels ----
        sdbus::registerMethod(kMethodSetChannelGain)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, double gainDb) {
                return setChannelGain(outputId, channelIndex, gainDb);
            }),
        sdbus::registerMethod(kMethodSetChannelMuted)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, bool muted) {
                return setChannelMuted(outputId, channelIndex, muted);
            }),
        sdbus::registerMethod(kMethodSetChannelPosition)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string position) {
                return setChannelPosition(outputId, channelIndex, position);
            }),
        sdbus::registerMethod(kMethodSetChannelDisplayName)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string displayName) {
                return setChannelDisplayName(outputId, channelIndex, displayName);
            }),
        sdbus::registerMethod(kMethodSetChannelEqInstance)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string eqId) {
                return setChannelEqInstance(outputId, channelIndex, eqId);
            }),

        // ---- EQ ----
        sdbus::registerMethod(kMethodAddEqInstance)
            .implementedAs([this](std::string outputId, std::string displayName) {
                return addEqInstance(outputId, displayName);
            }),
        sdbus::registerMethod(kMethodRemoveEqInstance)
            .implementedAs([this](std::string outputId, std::string eqId) {
                return removeEqInstance(outputId, eqId);
            }),
        sdbus::registerMethod(kMethodSetEqInstanceName)
            .implementedAs([this](std::string outputId, std::string eqId, std::string displayName) {
                return setEqInstanceName(outputId, eqId, displayName);
            }),
        sdbus::registerMethod(kMethodSetEqBypassed)
            .implementedAs([this](std::string outputId, std::string eqId, bool bypassed) {
                return setEqBypassed(outputId, eqId, bypassed);
            }),
        sdbus::registerMethod(kMethodSetEqBandCount)
            .implementedAs([this](std::string outputId, std::string eqId, uint32_t count) {
                return setEqBandCount(outputId, eqId, count);
            }),
        sdbus::registerMethod(kMethodSetEqBand)
            .implementedAs([this](std::string outputId, std::string eqId, uint32_t index,
                                   std::string type, double freqHz, double gainDb, double q) {
                return setEqBand(outputId, eqId, index, type, freqHz, gainDb, q);
            }),
        sdbus::registerMethod(kMethodCopyEqInstance)
            .implementedAs([this](std::string sourceOutputId, std::string eqId,
                                   std::string targetOutputId) {
                return copyEqInstance(sourceOutputId, eqId, targetOutputId);
            }),
        sdbus::registerMethod(kMethodSetChannelEqBandCount)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, uint32_t count) {
                return setChannelEqBandCount(outputId, channelIndex, count);
            }),
        sdbus::registerMethod(kMethodSetChannelEqBand)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, uint32_t index,
                                   std::string type, double freqHz, double gainDb, double q) {
                return setChannelEqBand(outputId, channelIndex, index, type, freqHz, gainDb, q);
            }),

        // ---- link groups ----
        sdbus::registerMethod(kMethodCreateLinkGroup)
            .implementedAs([this](std::string outputId, std::vector<uint32_t> channelIndices,
                                   std::string displayName) {
                return createLinkGroup(outputId, channelIndices, displayName);
            }),
        sdbus::registerMethod(kMethodRemoveLinkGroup)
            .implementedAs([this](std::string outputId, std::string groupId) {
                return removeLinkGroup(outputId, groupId);
            }),
        sdbus::registerMethod(kMethodSetLinkGroupChannels)
            .implementedAs([this](std::string outputId, std::string groupId,
                                   std::vector<uint32_t> channelIndices) {
                return setLinkGroupChannels(outputId, groupId, channelIndices);
            }),

        // ---- sends ----
        sdbus::registerMethod(kMethodSetSend)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string inputId,
                                   double gainDb) {
                return setSend(outputId, channelIndex, inputId, gainDb);
            }),
        sdbus::registerMethod(kMethodRemoveSend)
            .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string inputId) {
                return removeSend(outputId, channelIndex, inputId);
            }),

        // ---- inputs ----
        sdbus::registerMethod(kMethodAddInput)
            .implementedAs([this](std::string displayName, std::vector<std::string> positions) {
                return addInput(displayName, positions);
            }),
        sdbus::registerMethod(kMethodRemoveInput)
            .implementedAs([this](std::string inputId) { removeInput(inputId); }),
        sdbus::registerMethod(kMethodSetInputDisplayName)
            .implementedAs([this](std::string inputId, std::string displayName) {
                return setInputDisplayName(inputId, displayName);
            }),

        // ---- metering ----
        sdbus::registerMethod(kMethodSetMeteringEnabled)
            .implementedAs([this](bool enabled) { setMeteringEnabled(enabled); }),

        // ---- signals ----
        sdbus::registerSignal(kSignalDevicesChanged),
        sdbus::registerSignal(kSignalOutputsChanged),
        sdbus::registerSignal(kSignalOutputChanged).withParameters<std::string>(),
        sdbus::registerSignal(kSignalInputsChanged),
        sdbus::registerSignal(kSignalMeters)
            .withParameters<std::vector<MeterRow>, std::vector<MeterRow>>())
        .forInterface(kInterfaceName);
#else
    auto on = [&](const char* name) { return object_->registerMethod(name).onInterface(kInterfaceName); };

    on(kMethodListDevices).implementedAs([this] { return listDevices(); });
    on(kMethodListOutputs).implementedAs([this] { return listOutputs(); });
    on(kMethodGetOutputChannels)
        .implementedAs([this](std::string outputId) { return getOutputChannels(outputId); });
    on(kMethodListEqInstances)
        .implementedAs([this](std::string outputId) { return listEqInstances(outputId); });
    on(kMethodGetEqBands).implementedAs([this](std::string outputId, std::string eqId) {
        return getEqBands(outputId, eqId);
    });
    on(kMethodGetChannelEqBands).implementedAs([this](std::string outputId, uint32_t channelIndex) {
        return getChannelEqBands(outputId, channelIndex);
    });
    on(kMethodListLinkGroups)
        .implementedAs([this](std::string outputId) { return listLinkGroups(outputId); });
    on(kMethodGetSends).implementedAs([this](std::string outputId) { return getSends(outputId); });
    on(kMethodListInputs).implementedAs([this] { return listInputs(); });
    on(kMethodGetState).implementedAs([this] { return getState(); });

    on(kMethodAddOutput).implementedAs([this](std::string deviceName, std::string displayName) {
        return addOutput(deviceName, displayName);
    });
    on(kMethodRemoveOutput).implementedAs([this](std::string outputId) { removeOutput(outputId); });
    on(kMethodSetOutputDisplayName)
        .implementedAs([this](std::string outputId, std::string displayName) {
            return setOutputDisplayName(outputId, displayName);
        });
    on(kMethodSetOutputAutoConnect).implementedAs([this](std::string outputId, bool autoConnect) {
        return setOutputAutoConnect(outputId, autoConnect);
    });

    on(kMethodSetChannelGain)
        .implementedAs([this](std::string outputId, uint32_t channelIndex, double gainDb) {
            return setChannelGain(outputId, channelIndex, gainDb);
        });
    on(kMethodSetChannelMuted)
        .implementedAs([this](std::string outputId, uint32_t channelIndex, bool muted) {
            return setChannelMuted(outputId, channelIndex, muted);
        });
    on(kMethodSetChannelPosition)
        .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string position) {
            return setChannelPosition(outputId, channelIndex, position);
        });
    on(kMethodSetChannelDisplayName)
        .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string displayName) {
            return setChannelDisplayName(outputId, channelIndex, displayName);
        });
    on(kMethodSetChannelEqInstance)
        .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string eqId) {
            return setChannelEqInstance(outputId, channelIndex, eqId);
        });

    on(kMethodAddEqInstance).implementedAs([this](std::string outputId, std::string displayName) {
        return addEqInstance(outputId, displayName);
    });
    on(kMethodRemoveEqInstance).implementedAs([this](std::string outputId, std::string eqId) {
        return removeEqInstance(outputId, eqId);
    });
    on(kMethodSetEqInstanceName)
        .implementedAs([this](std::string outputId, std::string eqId, std::string displayName) {
            return setEqInstanceName(outputId, eqId, displayName);
        });
    on(kMethodSetEqBypassed)
        .implementedAs([this](std::string outputId, std::string eqId, bool bypassed) {
            return setEqBypassed(outputId, eqId, bypassed);
        });
    on(kMethodSetEqBandCount)
        .implementedAs([this](std::string outputId, std::string eqId, uint32_t count) {
            return setEqBandCount(outputId, eqId, count);
        });
    on(kMethodSetEqBand)
        .implementedAs([this](std::string outputId, std::string eqId, uint32_t index, std::string type,
                               double freqHz, double gainDb, double q) {
            return setEqBand(outputId, eqId, index, type, freqHz, gainDb, q);
        });
    on(kMethodCopyEqInstance)
        .implementedAs([this](std::string sourceOutputId, std::string eqId, std::string targetOutputId) {
            return copyEqInstance(sourceOutputId, eqId, targetOutputId);
        });
    on(kMethodSetChannelEqBandCount)
        .implementedAs([this](std::string outputId, uint32_t channelIndex, uint32_t count) {
            return setChannelEqBandCount(outputId, channelIndex, count);
        });
    on(kMethodSetChannelEqBand)
        .implementedAs([this](std::string outputId, uint32_t channelIndex, uint32_t index,
                               std::string type, double freqHz, double gainDb, double q) {
            return setChannelEqBand(outputId, channelIndex, index, type, freqHz, gainDb, q);
        });

    on(kMethodCreateLinkGroup)
        .implementedAs([this](std::string outputId, std::vector<uint32_t> channelIndices,
                               std::string displayName) {
            return createLinkGroup(outputId, channelIndices, displayName);
        });
    on(kMethodRemoveLinkGroup).implementedAs([this](std::string outputId, std::string groupId) {
        return removeLinkGroup(outputId, groupId);
    });
    on(kMethodSetLinkGroupChannels)
        .implementedAs([this](std::string outputId, std::string groupId,
                               std::vector<uint32_t> channelIndices) {
            return setLinkGroupChannels(outputId, groupId, channelIndices);
        });

    on(kMethodSetSend).implementedAs(
        [this](std::string outputId, uint32_t channelIndex, std::string inputId, double gainDb) {
            return setSend(outputId, channelIndex, inputId, gainDb);
        });
    on(kMethodRemoveSend)
        .implementedAs([this](std::string outputId, uint32_t channelIndex, std::string inputId) {
            return removeSend(outputId, channelIndex, inputId);
        });

    on(kMethodAddInput).implementedAs([this](std::string displayName, std::vector<std::string> positions) {
        return addInput(displayName, positions);
    });
    on(kMethodRemoveInput).implementedAs([this](std::string inputId) { removeInput(inputId); });
    on(kMethodSetInputDisplayName).implementedAs([this](std::string inputId, std::string displayName) {
        return setInputDisplayName(inputId, displayName);
    });

    on(kMethodSetMeteringEnabled).implementedAs([this](bool enabled) { setMeteringEnabled(enabled); });

    object_->registerSignal(kSignalDevicesChanged).onInterface(kInterfaceName);
    object_->registerSignal(kSignalOutputsChanged).onInterface(kInterfaceName);
    object_->registerSignal(kSignalOutputChanged)
        .onInterface(kInterfaceName)
        .withParameters<std::string>();
    object_->registerSignal(kSignalInputsChanged).onInterface(kInterfaceName);
    object_->registerSignal(kSignalMeters)
        .onInterface(kInterfaceName)
        .withParameters<std::vector<MeterRow>, std::vector<MeterRow>>();

    object_->finishRegistration();
#endif
}

// ---------------------------------------------------------------- readers --

std::vector<DbusService::DeviceRow> DbusService::listDevices() {
    std::vector<DeviceRow> rows;
    const std::vector<OutputInfo> outputs = engine_.listOutputs();
    for (const DeviceInfo& device : engine_.listDevices()) {
        const std::vector<std::string> positions = device.streamPositions();
        std::vector<DeviceChannelRow> channels;
        channels.reserve(positions.size());
        for (uint32_t i = 0; i < positions.size(); ++i) {
            channels.emplace_back(i, positions[i]);
        }
        const bool inUse =
            std::any_of(outputs.begin(), outputs.end(),
                         [&](const OutputInfo& o) { return o.deviceName == device.nodeName; });
        rows.emplace_back(device.id, device.nodeName, device.description, std::move(channels), inUse);
    }
    return rows;
}

std::vector<DbusService::OutputRow> DbusService::listOutputs() {
    std::vector<OutputRow> rows;
    for (const OutputInfo& output : engine_.listOutputs()) {
        rows.emplace_back(output.id, output.deviceName, output.displayName, output.connected,
                           output.autoConnect, static_cast<uint32_t>(output.channelCount),
                           static_cast<uint32_t>(output.liveChannelCount), output.sampleRateHz);
    }
    return rows;
}

std::vector<DbusService::ChannelRow> DbusService::getOutputChannels(const std::string& outputId) {
    std::vector<ChannelRow> rows;
    for (const ChannelInfo& channel : engine_.getOutputChannels(outputId)) {
        rows.emplace_back(static_cast<uint32_t>(channel.index), channel.position, channel.displayName,
                           channel.gainDb, channel.muted, channel.eqInstanceId, channel.groupId,
                           channel.driven);
    }
    return rows;
}

std::vector<DbusService::EqInstanceRow> DbusService::listEqInstances(const std::string& outputId) {
    std::vector<EqInstanceRow> rows;
    for (const EqInstanceInfo& instance : engine_.listEqInstances(outputId)) {
        rows.emplace_back(instance.id, instance.displayName,
                           static_cast<uint32_t>(instance.bandCount), instance.bypassed,
                           static_cast<uint32_t>(instance.channelCount));
    }
    return rows;
}

std::vector<DbusService::BandRow> DbusService::getEqBands(const std::string& outputId,
                                                           const std::string& eqInstanceId) {
    std::vector<BandRow> rows;
    for (const eqcore::EqBand& band : engine_.getEqBands(outputId, eqInstanceId)) {
        rows.emplace_back(filterTypeToString(band.type), band.freqHz, band.gainDb, band.q);
    }
    return rows;
}

std::vector<DbusService::BandRow> DbusService::getChannelEqBands(const std::string& outputId,
                                                                  uint32_t channelIndex) {
    std::vector<BandRow> rows;
    for (const eqcore::EqBand& band : engine_.getChannelEqBands(outputId, channelIndex)) {
        rows.emplace_back(filterTypeToString(band.type), band.freqHz, band.gainDb, band.q);
    }
    return rows;
}

std::vector<DbusService::LinkGroupRow> DbusService::listLinkGroups(const std::string& outputId) {
    std::vector<LinkGroupRow> rows;
    for (const LinkGroupInfo& group : engine_.listLinkGroups(outputId)) {
        rows.emplace_back(group.id, group.displayName, group.channelIndices);
    }
    return rows;
}

std::vector<DbusService::SendRow> DbusService::getSends(const std::string& outputId) {
    std::vector<SendRow> rows;
    for (const SendInfo& send : engine_.getSends(outputId)) {
        rows.emplace_back(static_cast<uint32_t>(send.channelIndex), send.inputId, send.gainDb);
    }
    return rows;
}

std::vector<DbusService::InputRow> DbusService::listInputs() {
    std::vector<InputRow> rows;
    for (const InputInfo& input : engine_.listInputs()) {
        rows.emplace_back(input.id, input.displayName, input.positions);
    }
    return rows;
}

DbusService::StateRow DbusService::getState() {
    return StateRow{listDevices(), listOutputs(), listInputs()};
}

// --------------------------------------------------------------- mutators --

std::string DbusService::addOutput(const std::string& deviceName, const std::string& displayName) {
    const std::string outputId = engine_.addOutput(deviceName, displayName);
    if (!outputId.empty()) {
        markConfigDirty();
        queueOutputChanged(outputId);
        emitOutputsChanged();
    }
    return outputId;
}

void DbusService::removeOutput(const std::string& outputId) {
    engine_.removeOutput(outputId);
    markConfigDirty();
    emitOutputsChanged();
}

bool DbusService::setOutputDisplayName(const std::string& outputId, const std::string& displayName) {
    const bool ok = engine_.setOutputDisplayName(outputId, displayName);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setOutputAutoConnect(const std::string& outputId, bool autoConnect) {
    const bool ok = engine_.setOutputAutoConnect(outputId, autoConnect);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setChannelGain(const std::string& outputId, uint32_t channelIndex, double gainDb) {
    const bool ok = engine_.setChannelGain(outputId, channelIndex, gainDb);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setChannelMuted(const std::string& outputId, uint32_t channelIndex, bool muted) {
    const bool ok = engine_.setChannelMuted(outputId, channelIndex, muted);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setChannelPosition(const std::string& outputId, uint32_t channelIndex,
                                      const std::string& position) {
    const bool ok = engine_.setChannelPosition(outputId, channelIndex, position);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setChannelDisplayName(const std::string& outputId, uint32_t channelIndex,
                                         const std::string& displayName) {
    const bool ok = engine_.setChannelDisplayName(outputId, channelIndex, displayName);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setChannelEqInstance(const std::string& outputId, uint32_t channelIndex,
                                        const std::string& eqInstanceId) {
    const bool ok = engine_.setChannelEqInstance(outputId, channelIndex, eqInstanceId);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

std::string DbusService::addEqInstance(const std::string& outputId, const std::string& displayName) {
    const std::string eqId = engine_.addEqInstance(outputId, displayName);
    if (!eqId.empty()) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return eqId;
}

bool DbusService::removeEqInstance(const std::string& outputId, const std::string& eqInstanceId) {
    const bool ok = engine_.removeEqInstance(outputId, eqInstanceId);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setEqInstanceName(const std::string& outputId, const std::string& eqInstanceId,
                                     const std::string& displayName) {
    const bool ok = engine_.setEqInstanceName(outputId, eqInstanceId, displayName);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setEqBypassed(const std::string& outputId, const std::string& eqInstanceId,
                                 bool bypassed) {
    const bool ok = engine_.setEqBypassed(outputId, eqInstanceId, bypassed);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setEqBandCount(const std::string& outputId, const std::string& eqInstanceId,
                                  uint32_t count) {
    const bool ok = engine_.setEqBandCount(outputId, eqInstanceId, count);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setEqBand(const std::string& outputId, const std::string& eqInstanceId,
                             uint32_t index, const std::string& type, double freqHz, double gainDb,
                             double q) {
    eqcore::EqBand band;
    try {
        band.type = filterTypeFromString(type);
    } catch (const std::exception&) {
        return false;
    }
    band.freqHz = freqHz;
    band.gainDb = gainDb;
    band.q = q;

    const bool ok = engine_.setEqBand(outputId, eqInstanceId, index, band);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

std::string DbusService::copyEqInstance(const std::string& sourceOutputId,
                                         const std::string& eqInstanceId,
                                         const std::string& targetOutputId) {
    const std::string newId = engine_.copyEqInstance(sourceOutputId, eqInstanceId, targetOutputId);
    if (!newId.empty()) {
        markConfigDirty();
        queueOutputChanged(targetOutputId);
    }
    return newId;
}

bool DbusService::setChannelEqBandCount(const std::string& outputId, uint32_t channelIndex,
                                         uint32_t count) {
    const bool ok = engine_.setChannelEqBandCount(outputId, channelIndex, count);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setChannelEqBand(const std::string& outputId, uint32_t channelIndex, uint32_t index,
                                    const std::string& type, double freqHz, double gainDb, double q) {
    eqcore::EqBand band;
    try {
        band.type = filterTypeFromString(type);
    } catch (const std::exception&) {
        return false;
    }
    band.freqHz = freqHz;
    band.gainDb = gainDb;
    band.q = q;

    const bool ok = engine_.setChannelEqBand(outputId, channelIndex, index, band);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

std::string DbusService::createLinkGroup(const std::string& outputId,
                                          const std::vector<uint32_t>& channelIndices,
                                          const std::string& displayName) {
    const std::string groupId = engine_.createLinkGroup(outputId, channelIndices, displayName);
    if (!groupId.empty()) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return groupId;
}

bool DbusService::removeLinkGroup(const std::string& outputId, const std::string& groupId) {
    const bool ok = engine_.removeLinkGroup(outputId, groupId);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setLinkGroupChannels(const std::string& outputId, const std::string& groupId,
                                        const std::vector<uint32_t>& channelIndices) {
    const bool ok = engine_.setLinkGroupChannels(outputId, groupId, channelIndices);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::setSend(const std::string& outputId, uint32_t channelIndex,
                           const std::string& inputId, double gainDb) {
    const bool ok = engine_.setSend(outputId, channelIndex, inputId, gainDb);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

bool DbusService::removeSend(const std::string& outputId, uint32_t channelIndex,
                              const std::string& inputId) {
    const bool ok = engine_.removeSend(outputId, channelIndex, inputId);
    if (ok) {
        markConfigDirty();
        queueOutputChanged(outputId);
    }
    return ok;
}

std::string DbusService::addInput(const std::string& displayName,
                                  const std::vector<std::string>& positions) {
    const std::string inputId = engine_.addInput(displayName, positions);
    markConfigDirty();
    queueInputsChanged();
    return inputId;
}

void DbusService::removeInput(const std::string& inputId) {
    engine_.removeInput(inputId);
    markConfigDirty();
    queueInputsChanged();
}

bool DbusService::setInputDisplayName(const std::string& inputId, const std::string& displayName) {
    const bool ok = engine_.setInputDisplayName(inputId, displayName);
    if (ok) {
        markConfigDirty();
        queueInputsChanged();
    }
    return ok;
}

void DbusService::setMeteringEnabled(bool enabled) {
    if (enabled) {
        meterLeaseUntil_.store(nowDuration() + kMeterLease, std::memory_order_relaxed);
    } else {
        meterLeaseUntil_.store(std::chrono::steady_clock::duration::zero(),
                                std::memory_order_relaxed);
    }
}

// ------------------------------------------------------- signals and saving --

void DbusService::queueOutputChanged(const std::string& outputId) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingOutputChanges_.push_back(outputId);
}

void DbusService::queueInputsChanged() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingInputsChanged_ = true;
}

void DbusService::emitOutputChanged(const std::string& outputId) {
    try {
        object_->emitSignal(eqcore::dbus::kSignalOutputChanged)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputId);
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "pipeeq: failed to emit OutputChanged: %s\n", e.what());
    }
}

void DbusService::emitOutputsChanged() {
    try {
        object_->emitSignal(eqcore::dbus::kSignalOutputsChanged)
            .onInterface(eqcore::dbus::kInterfaceName);
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "pipeeq: failed to emit OutputsChanged: %s\n", e.what());
    }
}

void DbusService::emitInputsChanged() {
    try {
        object_->emitSignal(eqcore::dbus::kSignalInputsChanged)
            .onInterface(eqcore::dbus::kInterfaceName);
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "pipeeq: failed to emit InputsChanged: %s\n", e.what());
    }
}

void DbusService::emitDevicesChanged() {
    try {
        object_->emitSignal(eqcore::dbus::kSignalDevicesChanged)
            .onInterface(eqcore::dbus::kInterfaceName);
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "pipeeq: failed to emit DevicesChanged: %s\n", e.what());
    }
}

void DbusService::emitMeters() {
    std::vector<MeterRow> outputMeters;
    for (const OutputInfo& output : engine_.listOutputs()) {
        if (!output.connected) {
            continue;
        }
        const std::vector<float> peaks = engine_.takeOutputPeaks(output.id);
        if (peaks.empty()) {
            continue;
        }
        std::vector<double> peaksDb;
        peaksDb.reserve(peaks.size());
        for (float peak : peaks) {
            peaksDb.push_back(linearToDb(peak));
        }
        outputMeters.emplace_back(output.id, std::move(peaksDb));
    }

    // Input metering is a later phase; the signal carries the array now so its
    // signature doesn't have to change when it arrives.
    const std::vector<MeterRow> inputMeters;

    try {
        object_->emitSignal(eqcore::dbus::kSignalMeters)
            .onInterface(eqcore::dbus::kInterfaceName)
            .withArguments(outputMeters, inputMeters);
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "pipeeq: failed to emit Meters: %s\n", e.what());
    }
}

void DbusService::markConfigDirty() {
    std::lock_guard<std::mutex> lock(persistMutex_);
    if (persistDisabled_) {
        return;
    }
    if (!configDirty_) {
        configDirty_ = true;
        dirtySince_ = std::chrono::steady_clock::now();
    }
}

void DbusService::flushConfigIfDue(bool force) {
    {
        std::lock_guard<std::mutex> lock(persistMutex_);
        if (persistDisabled_ || !configDirty_) {
            return;
        }
        if (!force && std::chrono::steady_clock::now() - dirtySince_ < kSaveDebounce) {
            return;
        }
        configDirty_ = false;
    }

    // snapshotConfig() reports every configured output, including ones whose
    // device isn't currently plugged in. Rebuilding this from only the live
    // outputs instead is what used to erase a powered-off device's EQ from the
    // config on the next slider move.
    //
    // Debounced rather than written per mutation: with a per-channel fader and a
    // per-channel x per-input send matrix, a drag would otherwise be tens of
    // full-document rewrites per second.
    std::lock_guard<std::mutex> lock(persistMutex_);
    eqcore::saveConfig(engine_.snapshotConfig());
}

} // namespace pipeeq
