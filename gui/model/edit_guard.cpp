#include "edit_guard.h"

namespace pipeeq {

EditGuard::EditGuard(QObject* parent) : QObject(parent) {
    clock_.start();
}

void EditGuard::beginEdit(const EditKey& key) {
    Hold& hold = holds_[key.toString()];
    hold.active = true;
    hold.releasedAtMs = -1;
}

void EditGuard::endEdit(const EditKey& key) {
    auto it = holds_.find(key.toString());
    if (it == holds_.end()) {
        return;
    }
    it->active = false;
    it->releasedAtMs = clock_.elapsed();
}

bool EditGuard::isHeld(const EditKey& key) const {
    const auto it = holds_.constFind(key.toString());
    if (it == holds_.constEnd()) {
        return false;
    }
    if (it->active) {
        return true;
    }
    // Still held while a write we sent hasn't come back, even if the grace
    // period has elapsed: the value on screen is the truth until the daemon
    // confirms otherwise.
    if (it->pendingWrites > 0) {
        return true;
    }
    return it->releasedAtMs >= 0 && (clock_.elapsed() - it->releasedAtMs) < kGraceMs;
}

void EditGuard::noteWriteSent(const EditKey& key) {
    ++holds_[key.toString()].pendingWrites;
}

void EditGuard::noteWriteCompleted(const EditKey& key) {
    auto it = holds_.find(key.toString());
    if (it == holds_.end()) {
        return;
    }
    if (it->pendingWrites > 0) {
        --it->pendingWrites;
    }
}

QVector<QString> EditGuard::takeExpiredKeys() {
    QVector<QString> expired;
    const qint64 now = clock_.elapsed();

    for (auto it = holds_.begin(); it != holds_.end();) {
        const bool stillHeld =
            it->active || it->pendingWrites > 0 ||
            (it->releasedAtMs >= 0 && (now - it->releasedAtMs) < kGraceMs);
        if (stillHeld) {
            ++it;
            continue;
        }
        expired.push_back(it.key());
        it = holds_.erase(it);
    }
    return expired;
}

} // namespace pipeeq
