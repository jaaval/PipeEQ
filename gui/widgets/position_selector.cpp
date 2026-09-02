#include "position_selector.h"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "theme/theme.h"

namespace pipeeq {

namespace {

// Where each position sits in the grid, so the layout reads as a room seen from
// above rather than as an arbitrary list. Row 0 is the front, the last row the
// rears; LFE sits under the centre.
struct Slot {
    const char* position;
    int row;
    int column;
};

constexpr Slot kSlots[] = {
    {"FL", 0, 0},  {"FC", 0, 1},  {"FR", 0, 2},
    {"FLC", 1, 0}, {"LFE", 1, 1}, {"FRC", 1, 2},
    {"SL", 2, 0},  {"MONO", 2, 1}, {"SR", 2, 2},
    {"RL", 3, 0},  {"RC", 3, 1},  {"RR", 3, 2},
    {"TFL", 4, 0}, {"TFC", 4, 1}, {"TFR", 4, 2},
    {"TRL", 5, 0}, {"TRC", 5, 1}, {"TRR", 5, 2},
};

bool slotFor(const QString& position, int& row, int& column) {
    for (const Slot& slot : kSlots) {
        if (position == slot.position) {
            row = slot.row;
            column = slot.column;
            return true;
        }
    }
    return false;
}

} // namespace

PositionSelector::PositionSelector(const QVector<Entry>& entries, const QString& current,
                                   QWidget* parent)
    : QDialog(parent) {
    const theme::Tokens tokens = theme::tokens();
    setWindowTitle("Channel position");

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel("Which position does this hardware channel drive?", this));

    auto* grid = new QGridLayout;
    grid->setSpacing(4);

    // Anything without a spatial slot - AUX channels above all - goes into rows
    // below the grid, in order.
    int extraRow = 6;
    int extraColumn = 0;

    for (const Entry& entry : entries) {
        auto* button = new QPushButton(entry.position, this);
        button->setCheckable(true);
        button->setChecked(entry.position == current);
        button->setMinimumWidth(64);
        button->setEnabled(entry.availableOnDevice || entry.position == current);

        QString tooltip;
        if (!entry.availableOnDevice) {
            tooltip = "The device's current profile doesn't offer this position.";
        } else if (!entry.takenBy.isEmpty()) {
            tooltip = QString("Currently driven by %1. Choosing it swaps the two channels.")
                          .arg(entry.takenBy);
            button->setStyleSheet(QString("QPushButton { color: %1; }").arg(tokens.warning.name()));
        }
        if (entry.position == current) {
            tooltip = "The position this channel drives now.";
            button->setStyleSheet(
                QString("QPushButton:checked { background: %1; color: %2; font-weight: bold; }")
                    .arg(tokens.accent.name(), tokens.accentText.name()));
        }
        button->setToolTip(tooltip);

        connect(button, &QPushButton::clicked, this, [this, position = entry.position] {
            chosen_ = position;
            accept();
        });

        int row = 0;
        int column = 0;
        if (slotFor(entry.position, row, column)) {
            grid->addWidget(button, row, column);
        } else {
            grid->addWidget(button, extraRow, extraColumn);
            if (++extraColumn == 3) {
                extraColumn = 0;
                ++extraRow;
            }
        }
    }

    root->addLayout(grid);

    auto* cancel = new QPushButton("Cancel", this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    root->addWidget(cancel);
}

QString PositionSelector::choose(QWidget* parent, const QVector<Entry>& entries,
                                  const QString& current) {
    PositionSelector dialog(entries, current, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    return dialog.chosen();
}

} // namespace pipeeq
