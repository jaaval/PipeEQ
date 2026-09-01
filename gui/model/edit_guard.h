#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>

#include "model_types.h"

namespace pipeeq {

// Decides whether a value arriving from the daemon may overwrite what is on
// screen - the "don't clobber the fader the user is dragging" problem.
//
// This replaces a single global suppressSignals_ bool, which was already
// subtly inconsistent across its use sites and could not express "this one
// field of this one object is being edited". Three cooperating mechanisms, per
// (object, field):
//
//   (a) A HOLD, opened on mouse-press and closed on release, then kept for a
//       grace period afterwards - long enough to cover the coalescer flush,
//       the daemon applying it, the config write, and the signal coming back.
//   (b) A PENDING COUNT, incremented per write and decremented when that write
//       actually completes. The grace period is a guess; this is not, so a slow
//       daemon can't let a stale value through just because the clock ran out.
//   (c) A DEFERRED value: a remote change that arrived while held is stashed
//       and applied once, on release. Dropping it forever is the other failure
//       mode and it is worse - the UI would then lie about the daemon's actual
//       value indefinitely.
class EditGuard : public QObject {
    Q_OBJECT

public:
    explicit EditGuard(QObject* parent = nullptr);

    void beginEdit(const EditKey& key);
    void endEdit(const EditKey& key);

    // True while a daemon value for this key must not be applied.
    bool isHeld(const EditKey& key) const;

    // Call when a write is sent, and again when it completes.
    void noteWriteSent(const EditKey& key);
    void noteWriteCompleted(const EditKey& key);

    // Keys whose hold has expired since the last call, so a deferred remote
    // value can now be applied. Returns the opaque key strings (see
    // EditKey::toString) and forgets them, so the caller can look up whatever
    // it stashed against them without this class having to parse anything.
    QVector<QString> takeExpiredKeys();

    // Long enough to cover a coalescer flush plus the daemon's round trip and
    // its debounced config write; short enough that a genuine remote change
    // isn't visibly stale after letting go.
    static constexpr qint64 kGraceMs = 400;

private:
    struct Hold {
        bool active = false;      // between beginEdit and endEdit
        qint64 releasedAtMs = -1; // when the grace period started
        int pendingWrites = 0;
    };

    QHash<QString, Hold> holds_;
    QElapsedTimer clock_;
};

} // namespace pipeeq
