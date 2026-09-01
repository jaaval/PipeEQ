// Config schema: v1 -> v2 migration, v2 parsing and round-tripping, the
// sanitizer's repairs, and the load/save path's refusal to overwrite a config
// it couldn't read.
//
// The migration case is driven by tests/data/real_v1_config.json - an actual
// config written by the shipped v1 daemon - so this asserts against a document
// that really exists rather than an idealized one.

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app_config.h"
#include "config_io.h"
#include "config_migrate.h"

#include "check.h"

namespace {

using namespace eqcore;

// ------------------------------------------------------------------ helpers --

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << contents;
}

// A scratch $XDG_CONFIG_HOME, so the load/save tests never touch the real one.
class ScopedConfigHome {
public:
    ScopedConfigHome() {
        dir_ = std::filesystem::temp_directory_path() /
               ("pipeeq-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
        ::setenv("XDG_CONFIG_HOME", dir_.c_str(), 1);
    }
    ~ScopedConfigHome() {
        ::unsetenv("XDG_CONFIG_HOME");
        std::error_code ignored;
        std::filesystem::remove_all(dir_, ignored);
    }

    std::filesystem::path configPath() const { return dir_ / "pipeeq" / "config.json"; }

private:
    static int counter_;
    std::filesystem::path dir_;
};
int ScopedConfigHome::counter_ = 0;

nlohmann::json realV1Document() {
    // PIPEEQ_TEST_DATA_DIR is set by CMake.
    const std::filesystem::path path =
        std::filesystem::path(PIPEEQ_TEST_DATA_DIR) / "real_v1_config.json";
    return nlohmann::json::parse(readFile(path));
}

// ------------------------------------------------------------- v1 migration --

// The real v1 config has two routes: a stereo HDMI sink with two peaking bands
// and one send, and a 4.0 Scarlett sink with no bands, no "channels" key and
// two sends. Both must come out behaving exactly as they did under v1.
void testMigrateRealV1Config() {
    std::vector<std::string> warnings;
    auto migrated = migrateV1(realV1Document(), warnings);
    CHECK(migrated.has_value());
    if (!migrated) {
        return;
    }
    const AppConfig& config = *migrated;

    CHECK_EQ(config.version, kConfigVersion);

    // Inputs keep their ids - the sends are keyed by them - and become stereo.
    CHECK_EQ(config.inputs.size(), std::size_t{2});
    CHECK_EQ(config.inputs[0].id, std::string("input-1"));
    CHECK_EQ(config.inputs[0].displayName, std::string("Default"));
    CHECK_EQ(config.inputs[0].positions, std::vector<std::string>({"FL", "FR"}));
    CHECK_EQ(config.inputs[1].id, std::string("input-2"));
    CHECK_EQ(config.inputs[1].displayName, std::string("Testi"));

    CHECK_EQ(config.outputs.size(), std::size_t{2});

    // ---- route-1 -> output-1: bands become one shared EQ instance ----
    const OutputConfig& hdmi = config.outputs[0];
    CHECK_EQ(hdmi.id, std::string("output-1"));
    CHECK_EQ(hdmi.deviceName, std::string("alsa_output.pci-0000_01_00.1.hdmi-stereo"));
    CHECK(hdmi.autoConnect);

    CHECK_EQ(hdmi.eqInstances.size(), std::size_t{1});
    CHECK_EQ(hdmi.eqInstances[0].id, std::string("eq-1"));
    CHECK_EQ(hdmi.eqInstances[0].bands.size(), std::size_t{2});
    // Bit-for-bit: a migration that rounded a curve would be a silent
    // regression in the thing the user actually tuned.
    const EqBand expectedFirst{FilterType::Peaking, 1000.0, 0.0, 0.86};
    const EqBand expectedSecond{FilterType::Peaking, 288.4834510482133, -5.414141414141412, 0.707};
    CHECK(hdmi.eqInstances[0].bands[0] == expectedFirst);
    CHECK(hdmi.eqInstances[0].bands[1] == expectedSecond);

    // Two channels, both referencing that instance, both carrying the route's
    // gain/mute/sends, and linked - so it behaves as one strip, as before.
    CHECK_EQ(hdmi.channels.size(), std::size_t{2});
    CHECK_EQ(hdmi.channels[0].position, std::string("FL"));
    CHECK_EQ(hdmi.channels[1].position, std::string("FR"));
    for (const OutputChannelConfig& channel : hdmi.channels) {
        CHECK_EQ(channel.eqInstanceId, std::string("eq-1"));
        CHECK_NEAR(channel.gainDb, 0.0, 1e-12);
        CHECK(!channel.muted);
        CHECK_EQ(channel.sendsDb.size(), std::size_t{1});
        CHECK(channel.sendsDb.count("input-1") == 1);
    }
    CHECK_EQ(hdmi.linkGroups.size(), std::size_t{1});
    CHECK_EQ(hdmi.linkGroups[0].channelIndices, std::vector<uint32_t>({0, 1}));

    // ---- route-2 -> output-2: no bands means no EQ instance at all ----
    const OutputConfig& scarlett = config.outputs[1];
    CHECK_EQ(scarlett.id, std::string("output-2"));
    CHECK_EQ(scarlett.eqInstances.size(), std::size_t{0});
    // No "channels" key in v1 meant "device default", which in practice was
    // the front pair. The real device is 4.0; adopting its extra channels is
    // the engine's job on connect, not the migration's.
    CHECK_EQ(scarlett.channels.size(), std::size_t{2});
    CHECK_EQ(scarlett.channels[0].position, std::string("FL"));
    CHECK_EQ(scarlett.channels[1].position, std::string("FR"));
    for (const OutputChannelConfig& channel : scarlett.channels) {
        CHECK_EQ(channel.eqInstanceId, std::string(""));
        CHECK_EQ(channel.sendsDb.size(), std::size_t{2});
    }
}

// v1's own "channels" field, when present, has to be honoured.
void testMigrateHonoursExplicitChannelPair() {
    const auto j = nlohmann::json::parse(R"({
      "inputs": [{"id": "input-1", "display_name": "Default"}],
      "routes": [{
        "id": "route-9", "device_name": "dev", "display_name": "Rear",
        "gain_db": -4.5, "muted": true, "bands": [],
        "channels": ["RL", "RR"], "auto_connect": false,
        "input_gains_db": {"input-1": -6.0}
      }]
    })");
    std::vector<std::string> warnings;
    auto migrated = migrateV1(j, warnings);
    CHECK(migrated.has_value());
    if (!migrated) {
        return;
    }

    const OutputConfig& output = migrated->outputs.at(0);
    CHECK_EQ(output.id, std::string("output-9"));
    CHECK(!output.autoConnect);
    CHECK_EQ(output.channels[0].position, std::string("RL"));
    CHECK_EQ(output.channels[1].position, std::string("RR"));
    CHECK_EQ(output.linkGroups.at(0).displayName, std::string("RL/RR"));
    for (const OutputChannelConfig& channel : output.channels) {
        CHECK_NEAR(channel.gainDb, -4.5, 1e-12);
        CHECK(channel.muted);
        CHECK_NEAR(channel.sendsDb.at("input-1"), -6.0, 1e-12);
    }
}

// A config predating the mixer feature has no "inputs" at all. It must gain the
// default input AND a send for it on every route, or every output goes silent
// on upgrade.
void testMigrateSynthesizesDefaultInput() {
    const auto j = nlohmann::json::parse(R"({
      "routes": [{"id": "route-1", "device_name": "dev", "display_name": "Main",
                  "gain_db": 0.0, "muted": false, "bands": []}]
    })");
    std::vector<std::string> warnings;
    auto migrated = migrateV1(j, warnings);
    CHECK(migrated.has_value());
    if (!migrated) {
        return;
    }

    CHECK_EQ(migrated->inputs.size(), std::size_t{1});
    CHECK_EQ(migrated->inputs[0].id, std::string("input-1"));
    CHECK_EQ(migrated->inputs[0].displayName, std::string("Default"));
    for (const OutputChannelConfig& channel : migrated->outputs.at(0).channels) {
        CHECK(channel.sendsDb.count("input-1") == 1);
    }
}

// v1's parser threw on a missing "routes", and the caller turned the throw into
// "zero routes" and then saved it. Now it's a warning and an empty config.
void testMigrateMissingRoutesIsNotFatal() {
    std::vector<std::string> warnings;
    auto migrated = migrateV1(nlohmann::json::parse(R"({"inputs": []})"), warnings);
    CHECK(migrated.has_value());
    if (migrated) {
        CHECK_EQ(migrated->outputs.size(), std::size_t{0});
    }
    CHECK(!warnings.empty());
}

void testMigrateRejectsNonObject() {
    std::vector<std::string> warnings;
    CHECK(!migrateV1(nlohmann::json::parse("[1, 2, 3]"), warnings).has_value());
}

// -------------------------------------------------------- v2 parse and save --

AppConfig sampleV2Config() {
    AppConfig config;

    InputConfig music;
    music.id = "input-1";
    music.displayName = "Music";
    music.positions = {"FL", "FR", "FC", "LFE", "RL", "RR"};
    config.inputs.push_back(music);

    InputConfig voice;
    voice.id = "input-2";
    voice.displayName = "Voice";
    voice.positions = {"FL", "FR"};
    config.inputs.push_back(voice);

    OutputConfig output;
    output.id = "output-1";
    output.deviceName = "alsa_output.usb-thing";
    output.displayName = "Interface";
    output.autoConnect = true;

    EqInstanceConfig mains;
    mains.id = "eq-1";
    mains.displayName = "Mains";
    mains.bands = {{FilterType::LowShelf, 90.0, 2.5, 0.707},
                   {FilterType::Peaking, 3150.0, -3.25, 1.8}};
    output.eqInstances.push_back(mains);

    EqInstanceConfig sub;
    sub.id = "eq-2";
    sub.displayName = "Sub HPF";
    sub.bypassed = true;
    sub.bands = {{FilterType::LowPass, 80.0, 0.0, 0.707}};
    output.eqInstances.push_back(sub);

    for (const auto& [position, eqId] : std::vector<std::pair<std::string, std::string>>{
             {"FL", "eq-1"}, {"FR", "eq-1"}, {"FC", ""}, {"LFE", "eq-2"}}) {
        OutputChannelConfig channel;
        channel.position = position;
        channel.displayName = position == "LFE" ? "Subwoofer" : "";
        channel.gainDb = position == "LFE" ? 3.0 : -1.5;
        channel.muted = position == "FC";
        channel.eqInstanceId = eqId;
        channel.sendsDb = {{"input-1", 0.0}, {"input-2", -6.5}};
        output.channels.push_back(channel);
    }

    LinkGroupConfig group;
    group.id = "group-1";
    group.displayName = "Mains";
    group.channelIndices = {0, 1};
    output.linkGroups.push_back(group);

    config.outputs.push_back(output);
    return config;
}

void testV2JsonRoundTrip() {
    const AppConfig original = sampleV2Config();
    std::vector<std::string> warnings;
    auto reparsed = parseV2(nlohmann::json(original), warnings);

    CHECK(reparsed.has_value());
    CHECK(warnings.empty());
    if (reparsed) {
        CHECK(*reparsed == original);
    }
}

void testV2SaveLoadRoundTrip() {
    ScopedConfigHome home;
    const AppConfig original = sampleV2Config();

    CHECK(saveConfig(original));
    const LoadResult loaded = loadConfig();

    CHECK(loaded.status == LoadStatus::Loaded);
    CHECK(loaded.persistable());
    CHECK(loaded.config == original);
}

void testParseRejectsWrongVersion() {
    std::vector<std::string> warnings;
    // A v1 document must not be accepted by the v2 parser: it goes through
    // migrateV1 instead.
    CHECK(!parseV2(nlohmann::json::parse(R"({"routes": []})"), warnings).has_value());
    CHECK(!parseV2(nlohmann::json::parse(R"({"version": 99, "outputs": []})"), warnings).has_value());
    CHECK(!parseV2(nlohmann::json::parse("42"), warnings).has_value());
}

// One bad output must not cost the good ones - that was the v1 failure mode.
void testParseSkipsMalformedOutputAndKeepsRest() {
    const auto j = nlohmann::json::parse(R"({
      "version": 2,
      "inputs": [{"id": "input-1", "display_name": "A", "positions": ["FL","FR"]}],
      "outputs": [
        {"id": "output-1", "device_name": "good-1", "channels": [{"position": "FL"}]},
        {"device_name": "no-id-so-skipped"},
        "not even an object",
        {"id": "output-3", "device_name": "good-2", "channels": [{"position": "FR"}]}
      ]
    })");
    std::vector<std::string> warnings;
    auto parsed = parseV2(j, warnings);

    CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    CHECK_EQ(parsed->outputs.size(), std::size_t{2});
    CHECK_EQ(parsed->outputs[0].deviceName, std::string("good-1"));
    CHECK_EQ(parsed->outputs[1].deviceName, std::string("good-2"));
    CHECK_EQ(warnings.size(), std::size_t{2});
}

// A present-but-wrong-typed field degrades to its default rather than aborting
// the load, which is the realistic hand-edited-file case.
void testParseToleratesWrongTypedFields() {
    const auto j = nlohmann::json::parse(R"({
      "version": 2,
      "outputs": [{
        "id": "output-1", "device_name": "dev",
        "auto_connect": "yes please",
        "channels": [{"position": "FL", "gain_db": "loud", "muted": 3}]
      }]
    })");
    std::vector<std::string> warnings;
    auto parsed = parseV2(j, warnings);

    CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    const OutputConfig& output = parsed->outputs.at(0);
    CHECK(output.autoConnect);
    CHECK_NEAR(output.channels.at(0).gainDb, 0.0, 1e-12);
    CHECK(!output.channels.at(0).muted);
}

// ---------------------------------------------------------------- sanitizer --

void testSanitizeClampsBandCount() {
    AppConfig config;
    OutputConfig output;
    output.id = "output-1";
    output.deviceName = "dev";
    EqInstanceConfig instance;
    instance.id = "eq-1";
    instance.bands.assign(kMaxBands + 5, EqBand{});
    output.eqInstances.push_back(instance);
    config.outputs.push_back(output);

    std::vector<std::string> warnings;
    sanitize(config, warnings);

    CHECK_EQ(config.outputs.at(0).eqInstances.at(0).bands.size(), kMaxBands);
    CHECK(!warnings.empty());
}

void testSanitizeClearsDanglingEqReference() {
    AppConfig config;
    OutputConfig output;
    output.id = "output-1";
    output.deviceName = "dev";
    OutputChannelConfig channel;
    channel.position = "FL";
    channel.eqInstanceId = "eq-does-not-exist";
    output.channels.push_back(channel);
    config.outputs.push_back(output);

    std::vector<std::string> warnings;
    sanitize(config, warnings);

    CHECK_EQ(config.outputs.at(0).channels.at(0).eqInstanceId, std::string(""));
    CHECK(!warnings.empty());
}

void testSanitizeDropsSendsForUnknownInputs() {
    AppConfig config;
    InputConfig input;
    input.id = "input-1";
    config.inputs.push_back(input);

    OutputConfig output;
    output.id = "output-1";
    output.deviceName = "dev";
    OutputChannelConfig channel;
    channel.position = "FL";
    channel.sendsDb = {{"input-1", 0.0}, {"input-ghost", -3.0}};
    output.channels.push_back(channel);
    config.outputs.push_back(output);

    std::vector<std::string> warnings;
    sanitize(config, warnings);

    const auto& sends = config.outputs.at(0).channels.at(0).sendsDb;
    CHECK_EQ(sends.size(), std::size_t{1});
    CHECK(sends.count("input-1") == 1);
}

void testSanitizeDropsDuplicateIds() {
    AppConfig config;
    config.inputs.push_back(InputConfig{"input-1", "A", {"FL", "FR"}});
    config.inputs.push_back(InputConfig{"input-1", "B", {"FL", "FR"}});

    OutputConfig first;
    first.id = "output-1";
    first.deviceName = "dev-a";
    OutputConfig second;
    second.id = "output-1";
    second.deviceName = "dev-b";
    config.outputs.push_back(first);
    config.outputs.push_back(second);

    std::vector<std::string> warnings;
    sanitize(config, warnings);

    CHECK_EQ(config.inputs.size(), std::size_t{1});
    CHECK_EQ(config.inputs.at(0).displayName, std::string("A"));
    CHECK_EQ(config.outputs.size(), std::size_t{1});
    CHECK_EQ(config.outputs.at(0).deviceName, std::string("dev-a"));
}

// A channel in two groups has no coherent meaning: a set on it would have to
// write two different member sets.
void testSanitizeRejectsOverlappingLinkGroups() {
    AppConfig config;
    OutputConfig output;
    output.id = "output-1";
    output.deviceName = "dev";
    output.channels.resize(4);
    for (std::size_t i = 0; i < 4; ++i) {
        output.channels[i].position = "AUX" + std::to_string(i);
    }
    output.linkGroups.push_back(LinkGroupConfig{"group-1", "A", {0, 1}});
    output.linkGroups.push_back(LinkGroupConfig{"group-2", "B", {1, 2, 3}});
    config.outputs.push_back(output);

    std::vector<std::string> warnings;
    sanitize(config, warnings);

    const OutputConfig& out = config.outputs.at(0);
    CHECK_EQ(out.linkGroups.size(), std::size_t{2});
    CHECK_EQ(out.linkGroups.at(0).channelIndices, std::vector<uint32_t>({0, 1}));
    CHECK_EQ(out.linkGroups.at(1).channelIndices, std::vector<uint32_t>({2, 3}));
    CHECK(!warnings.empty());
}

// A group referencing a channel index past the end is expected after a profile
// switch shrinks a device, and must survive so a switch back restores it.
void testSanitizeToleratesRetiredChannelIndices() {
    AppConfig config;
    OutputConfig output;
    output.id = "output-1";
    output.deviceName = "dev";
    output.channels.resize(2);
    output.channels[0].position = "FL";
    output.channels[1].position = "FR";
    output.linkGroups.push_back(LinkGroupConfig{"group-2", "Rear", {2, 3}});
    config.outputs.push_back(output);

    std::vector<std::string> warnings;
    sanitize(config, warnings);

    CHECK_EQ(config.outputs.at(0).linkGroups.size(), std::size_t{1});
    CHECK_EQ(config.outputs.at(0).linkGroups.at(0).channelIndices, std::vector<uint32_t>({2, 3}));
}

void testLinkedChannelsAccessor() {
    OutputConfig output;
    output.channels.resize(4);
    output.linkGroups.push_back(LinkGroupConfig{"group-1", "Mains", {0, 1}});

    CHECK_EQ(output.linkedChannels(0), std::vector<std::size_t>({0, 1}));
    CHECK_EQ(output.linkedChannels(1), std::vector<std::size_t>({0, 1}));
    CHECK_EQ(output.linkedChannels(2), std::vector<std::size_t>({2}));
    CHECK(output.groupOfChannel(0) != nullptr);
    CHECK(output.groupOfChannel(3) == nullptr);
}

void testNextIdsAvoidCollisions() {
    OutputConfig output;
    CHECK_EQ(output.nextEqInstanceId(), std::string("eq-1"));
    output.eqInstances.push_back(EqInstanceConfig{"eq-1", "a", false, {}});
    output.eqInstances.push_back(EqInstanceConfig{"eq-7", "b", false, {}});
    CHECK_EQ(output.nextEqInstanceId(), std::string("eq-8"));

    CHECK_EQ(output.nextLinkGroupId(), std::string("group-1"));
    output.linkGroups.push_back(LinkGroupConfig{"group-3", "g", {0, 1}});
    CHECK_EQ(output.nextLinkGroupId(), std::string("group-4"));
}

// --------------------------------------------------------- load path safety --

void testLoadMissingConfig() {
    ScopedConfigHome home;
    const LoadResult loaded = loadConfig();

    CHECK(loaded.status == LoadStatus::Missing);
    CHECK(loaded.persistable()); // a first run is safe to write
    CHECK_EQ(loaded.config.outputs.size(), std::size_t{0});
}

// The whole point of LoadStatus::Failed: an unreadable config must be reported
// as not-persistable, so the daemon never writes over something recoverable.
void testLoadMalformedConfigFailsAndIsNotPersistable() {
    ScopedConfigHome home;
    const std::string corrupt = "{ \"version\": 2, \"outputs\": [ { \"id\": ";
    writeFile(home.configPath(), corrupt);

    const LoadResult loaded = loadConfig();

    CHECK(loaded.status == LoadStatus::Failed);
    CHECK(!loaded.persistable());
    CHECK(!loaded.message.empty());
    // And the file itself is untouched by the failed load.
    CHECK_EQ(readFile(home.configPath()), corrupt);
}

// A config from a newer PipeEQ is not something to helpfully reduce to nothing.
void testLoadNewerVersionFails() {
    ScopedConfigHome home;
    writeFile(home.configPath(), R"({"version": 99, "inputs": [], "outputs": []})");

    const LoadResult loaded = loadConfig();

    CHECK(loaded.status == LoadStatus::Failed);
    CHECK(!loaded.persistable());
}

void testLoadV1MigratesAndBacksUp() {
    ScopedConfigHome home;
    const std::string v1 = readFile(std::filesystem::path(PIPEEQ_TEST_DATA_DIR) / "real_v1_config.json");
    writeFile(home.configPath(), v1);

    const LoadResult loaded = loadConfig();

    CHECK(loaded.status == LoadStatus::Migrated);
    CHECK(loaded.persistable());
    CHECK_EQ(loaded.config.outputs.size(), std::size_t{2});
    CHECK_EQ(loaded.config.version, kConfigVersion);

    // The original must be recoverable by hand, byte-for-byte.
    const std::filesystem::path backup = home.configPath().string() + ".v1.bak";
    CHECK(std::filesystem::exists(backup));
    CHECK_EQ(readFile(backup), v1);

    // The v1 file itself is still in place until something saves over it.
    CHECK_EQ(readFile(home.configPath()), v1);

    // Saving the migrated config and reloading must be stable.
    CHECK(saveConfig(loaded.config));
    const LoadResult reloaded = loadConfig();
    CHECK(reloaded.status == LoadStatus::Loaded);
    CHECK(reloaded.config == loaded.config);
}

void testSaveIsAtomicAndLeavesNoTempFile() {
    ScopedConfigHome home;
    CHECK(saveConfig(sampleV2Config()));

    CHECK(std::filesystem::exists(home.configPath()));
    CHECK(!std::filesystem::exists(home.configPath().string() + ".tmp"));
}

} // namespace

int main() {
    RUN(testMigrateRealV1Config);
    RUN(testMigrateHonoursExplicitChannelPair);
    RUN(testMigrateSynthesizesDefaultInput);
    RUN(testMigrateMissingRoutesIsNotFatal);
    RUN(testMigrateRejectsNonObject);

    RUN(testV2JsonRoundTrip);
    RUN(testV2SaveLoadRoundTrip);
    RUN(testParseRejectsWrongVersion);
    RUN(testParseSkipsMalformedOutputAndKeepsRest);
    RUN(testParseToleratesWrongTypedFields);

    RUN(testSanitizeClampsBandCount);
    RUN(testSanitizeClearsDanglingEqReference);
    RUN(testSanitizeDropsSendsForUnknownInputs);
    RUN(testSanitizeDropsDuplicateIds);
    RUN(testSanitizeRejectsOverlappingLinkGroups);
    RUN(testSanitizeToleratesRetiredChannelIndices);
    RUN(testLinkedChannelsAccessor);
    RUN(testNextIdsAvoidCollisions);

    RUN(testLoadMissingConfig);
    RUN(testLoadMalformedConfigFailsAndIsNotPersistable);
    RUN(testLoadNewerVersionFails);
    RUN(testLoadV1MigratesAndBacksUp);
    RUN(testSaveIsAtomicAndLeavesNoTempFile);

    return pipeeq::test::summary("config");
}
