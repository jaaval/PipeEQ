#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

namespace pipeeq {

// Picks a channel's logical position from the positions its device actually
// advertises.
//
// A grid rather than a dropdown because positions are spatial: FL/FR above
// RL/RR reads as a room, a vertical list does not. A position already claimed by
// a sibling channel is shown and offered - picking it SWAPS the two, which is
// what someone re-mapping a mis-wired interface wants - and one the device
// doesn't advertise is shown disabled rather than hidden, so its absence is
// visible instead of mysterious.
class PositionSelector : public QDialog {
    Q_OBJECT

public:
    struct Entry {
        QString position;
        // Non-empty when a sibling channel of the same output already claims it;
        // holds that channel's label, for the swap prompt.
        QString takenBy;
        bool availableOnDevice = true;
    };

    PositionSelector(const QVector<Entry>& entries, const QString& current, QWidget* parent = nullptr);

    // Empty if cancelled.
    QString chosen() const { return chosen_; }

    // Returns the chosen position, or empty if cancelled.
    static QString choose(QWidget* parent, const QVector<Entry>& entries, const QString& current);

private:
    QString chosen_;
};

} // namespace pipeeq
