#pragma once

#include <cstdint>

#include <QMetaType>
#include <QString>
#include <QVector>

#include "backend.h"

namespace pipeeq {

// Identifies one editable control for the purposes of edit guarding and write
// coalescing: an object plus which of its fields.
//
// The field matters. Without it, a snapshot arriving while the user drags a
// fader would have to be dropped wholesale - including a genuine remote change
// to that channel's mute or EQ, which has nothing to do with the drag.
enum class Field : uint8_t {
    Gain,
    Mute,
    Position,
    AutoConnect,
    EqBand,
    EqBandCount,
    Send,
};

struct EditKey {
    QString objectId; // strip id, or "<stripId>:<inputId>" for a send
    Field field = Field::Gain;
    int index = -1; // band index for EqBand; -1 otherwise

    bool operator==(const EditKey&) const = default;

    QString toString() const {
        return objectId + "/" + QString::number(static_cast<int>(field)) + "/" +
               QString::number(index);
    }
};

inline std::size_t qHash(const EditKey& key, std::size_t seed = 0) {
    return qHash(key.toString(), seed);
}

// One pending write. Continuous params are coalesced latest-value-wins; discrete
// ones keep their order, because "band count 3" then "band 2 = ..." is not the
// same as the reverse.
struct WriteOp {
    enum class Kind : uint8_t {
        ChannelGain,
        ChannelMute,
        ChannelPosition,
        OutputAutoConnect,
        ChannelEqBandCount,
        ChannelEqBand,
        Send,
        RemoveSend,
    };

    Kind kind = Kind::ChannelGain;
    QString outputId;
    uint32_t channelIndex = 0;
    // Kind-specific payload. Kept as a flat struct rather than a variant so the
    // whole thing stays trivially copyable across the thread boundary.
    QString stringArg;  // position, inputId, or filter type
    uint32_t uintArg = 0; // band index or band count
    double doubleArg = 0.0; // gain / level / freq
    double doubleArg2 = 0.0; // band gain
    double doubleArg3 = 0.0; // band Q
    bool boolArg = false;
    quint64 seq = 0;

    // True for writes that are safe to collapse to the newest value.
    bool coalescable() const {
        return kind == Kind::ChannelGain || kind == Kind::Send || kind == Kind::ChannelEqBand;
    }

    // The coalescing identity: two ops with the same key supersede each other.
    QString coalesceKey() const {
        return QString::number(static_cast<int>(kind)) + "/" + outputId + "/" +
               QString::number(channelIndex) + "/" + stringArg + "/" + QString::number(uintArg);
    }
};

// Everything one read round trip returns. Fetched in one go rather than
// per-selection, because the EQ editor's ghost curves need every channel's
// bands resident, and discovering that later would mean unpicking a lazy
// per-selection fetch.
struct DaemonSnapshot {
    QVector<DeviceRow> devices;
    QVector<StripRow> strips;
    QVector<InputRow> inputs;
    bool available = false;
};

} // namespace pipeeq

Q_DECLARE_METATYPE(pipeeq::DaemonSnapshot)
Q_DECLARE_METATYPE(pipeeq::WriteOp)
Q_DECLARE_METATYPE(QVector<pipeeq::WriteOp>)
