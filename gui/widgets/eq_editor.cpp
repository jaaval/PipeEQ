#include "eq_editor.h"

#include <algorithm>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSet>
#include <QPushButton>
#include <QVBoxLayout>

#include "app_config.h"
#include "eq_curve_widget.h"
#include "model/app_state.h"
#include "theme/theme.h"

namespace pipeeq {

namespace {

// In enum order: a button's index is used directly as the FilterType value.
const struct {
    eqcore::FilterType type;
    const char* label;
} kFilterTypes[] = {
    {eqcore::FilterType::Peaking, "PK"},
    {eqcore::FilterType::LowShelf, "LSH"},
    {eqcore::FilterType::HighShelf, "HSH"},
    {eqcore::FilterType::LowPass, "LP"},
    {eqcore::FilterType::HighPass, "HP"},
};

QString shortType(eqcore::FilterType type) {
    for (const auto& entry : kFilterTypes) {
        if (entry.type == type) {
            return QString::fromLatin1(entry.label);
        }
    }
    return QStringLiteral("?");
}

QString formatFreq(double freqHz) {
    if (freqHz >= 1000.0) {
        return QString::number(freqHz / 1000.0, 'f', freqHz >= 10000.0 ? 0 : 1) + "k";
    }
    return QString::number(freqHz, 'f', 0);
}

// The shared limit, not a second copy of the number.
constexpr int kMaxBands = static_cast<int>(eqcore::kMaxBands);

} // namespace

EqEditor::EqEditor(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
    const theme::Tokens tokens = theme::tokens();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    // ---- header ----
    auto* header = new QHBoxLayout;
    auto* backButton = new QPushButton("< Back to mixer", this);
    connect(backButton, &QPushButton::clicked, this, &EqEditor::backRequested);
    header->addWidget(backButton);

    title_ = new QLabel(this);
    QFont titleFont = tokens.uiFont;
    titleFont.setBold(true);
    title_->setFont(titleFont);
    header->addWidget(title_);

    sharedNote_ = new QLabel(this);
    sharedNote_->setStyleSheet(QString("color: %1;").arg(tokens.linkActive.name()));
    header->addWidget(sharedNote_);

    header->addStretch(1);

    bandCountLabel_ = new QLabel(this);
    bandCountLabel_->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    header->addWidget(bandCountLabel_);

    copyButton_ = new QPushButton("Copy to channel...", this);
    connect(copyButton_, &QPushButton::clicked, this, &EqEditor::showCopyDialog);
    header->addWidget(copyButton_);
    root->addLayout(header);

    // ---- the curve ----
    curve_ = new EqCurveWidget(this);
    connect(curve_, &EqCurveWidget::bandEdited, this, [this](int index, eqcore::EqBand band) {
        if (index < 0 || index >= bands_.size()) {
            return;
        }
        bands_[index] = band;
        const StripRow* strip = state_->findStrip(stripId_);
        if (!strip) {
            return;
        }
        // One write per drag event is fine: the store coalesces to at most 25 a
        // second and flushes the final value on release.
        state_->setChannelEqBand(strip->outputId, strip->channelIndex,
                                  static_cast<uint32_t>(index), band);
        updateBandControls();
        rebuildRibbon();
    });
    connect(curve_, &EqCurveWidget::bandEditBegan, this, [this](int index) {
        if (const StripRow* strip = state_->findStrip(stripId_)) {
            state_->beginEdit(EditKey{strip->id, Field::EqBand, index});
        }
    });
    connect(curve_, &EqCurveWidget::bandEditFinished, this, [this](int index) {
        if (const StripRow* strip = state_->findStrip(stripId_)) {
            state_->endEdit(EditKey{strip->id, Field::EqBand, index});
        }
    });
    connect(curve_, &EqCurveWidget::selectedBandChanged, this, [this](int index) {
        selected_ = index;
        updateBandControls();
        rebuildRibbon();
    });
    connect(curve_, &EqCurveWidget::bandAddRequested, this, &EqEditor::addBand);
    connect(curve_, &EqCurveWidget::bandRemoveRequested, this, &EqEditor::removeBand);
    connect(curve_, &EqCurveWidget::bandTypeChangeRequested, this,
            [this](int index, eqcore::FilterType type) {
                if (index < 0 || index >= bands_.size()) {
                    return;
                }
                bands_[index].type = type;
                selected_ = index;
                pushSelectedBand();
            });
    root->addWidget(curve_, 1);

    // ---- band ribbon: one chip per band, plus add ----
    auto* ribbonRow = new QWidget(this);
    ribbonLayout_ = new QHBoxLayout(ribbonRow);
    ribbonLayout_->setContentsMargins(0, 0, 0, 0);
    ribbonLayout_->setSpacing(4);
    addBandButton_ = new QPushButton("+ Add band", ribbonRow);
    connect(addBandButton_, &QPushButton::clicked, this, [this] {
        // A new band from the button lands at 1 kHz and flat, which is a
        // deliberate no-op until it's moved: adding a band should never change
        // the sound by itself.
        addBand(1000.0, 0.0);
    });
    ribbonLayout_->addWidget(addBandButton_);
    ribbonLayout_->addStretch(1);
    root->addWidget(ribbonRow);

    // ---- the selected band's controls ----
    auto* bandRow = new QHBoxLayout;
    bandTitle_ = new QLabel(this);
    bandTitle_->setMinimumWidth(70);
    bandRow->addWidget(bandTitle_);

    // A checked QPushButton under Fusion is barely distinguishable from an
    // unchecked one, which for a five-way exclusive choice is useless - so the
    // checked state is given the accent colour explicitly.
    const QString typeButtonStyle =
        QString("QPushButton:checked { background: %1; color: %2; font-weight: bold; }")
            .arg(tokens.accent.name(), tokens.accentText.name());
    for (const auto& entry : kFilterTypes) {
        auto* button = new QPushButton(entry.label, this);
        button->setCheckable(true);
        button->setMaximumWidth(46);
        button->setStyleSheet(typeButtonStyle);
        connect(button, &QPushButton::clicked, this, [this, type = entry.type] {
            if (selected_ < 0 || selected_ >= bands_.size()) {
                return;
            }
            bands_[selected_].type = type;
            pushSelectedBand();
        });
        typeButtons_.push_back(button);
        bandRow->addWidget(button);
    }

    bandRow->addSpacing(10);
    bandRow->addWidget(new QLabel("Freq", this));
    freqSpin_ = new QDoubleSpinBox(this);
    freqSpin_->setRange(20.0, 20000.0);
    freqSpin_->setDecimals(1);
    freqSpin_->setSuffix(" Hz");
    freqSpin_->setMinimumWidth(110);
    connect(freqSpin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (suppressSignals_ || selected_ < 0 || selected_ >= bands_.size()) {
            return;
        }
        bands_[selected_].freqHz = value;
        pushSelectedBand();
    });
    bandRow->addWidget(freqSpin_);

    bandRow->addWidget(new QLabel("Gain", this));
    gainSpin_ = new QDoubleSpinBox(this);
    gainSpin_->setRange(-24.0, 24.0);
    gainSpin_->setDecimals(2);
    gainSpin_->setSuffix(" dB");
    connect(gainSpin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (suppressSignals_ || selected_ < 0 || selected_ >= bands_.size()) {
            return;
        }
        bands_[selected_].gainDb = value;
        pushSelectedBand();
    });
    bandRow->addWidget(gainSpin_);

    bandRow->addWidget(new QLabel("Q", this));
    qSpin_ = new QDoubleSpinBox(this);
    qSpin_->setRange(0.1, 10.0);
    qSpin_->setDecimals(3);
    qSpin_->setSingleStep(0.05);
    connect(qSpin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (suppressSignals_ || selected_ < 0 || selected_ >= bands_.size()) {
            return;
        }
        bands_[selected_].q = value;
        pushSelectedBand();
    });
    bandRow->addWidget(qSpin_);

    bandRow->addStretch(1);
    removeBandButton_ = new QPushButton("Remove band", this);
    connect(removeBandButton_, &QPushButton::clicked, this, [this] { removeBand(selected_); });
    bandRow->addWidget(removeBandButton_);
    root->addLayout(bandRow);
}

void EqEditor::setSelection(const QString& stripId) {
    stripId_ = stripId;
    selected_ = -1;
    if (const StripRow* strip = state_->findStrip(stripId)) {
        state_->requestChannelDetail(strip->outputId, strip->channelIndex);
    }
    refresh();
}

void EqEditor::refresh() {
    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip) {
        title_->setText("No channel selected");
        sharedNote_->clear();
        bandCountLabel_->clear();
        bands_.clear();
        curve_->setBands({});
        rebuildRibbon();
        updateBandControls();
        return;
    }

    bands_ = state_->channelBands(strip->outputId, strip->channelIndex);
    if (selected_ >= bands_.size()) {
        selected_ = bands_.isEmpty() ? -1 : bands_.size() - 1;
    }
    if (selected_ < 0 && !bands_.isEmpty()) {
        selected_ = 0;
    }

    const QString name = strip->channelName.isEmpty() ? strip->position : strip->channelName;
    title_->setText(QString("%1 · %2").arg(strip->outputName, name));

    // Say plainly that an edit here lands on every linked channel. Without it,
    // the fact that FL and FR share one curve is invisible until someone
    // notices both moved.
    if (!strip->groupId.isEmpty()) {
        QStringList siblings;
        for (const StripRow& other : state_->strips()) {
            if (other.outputId == strip->outputId && other.groupId == strip->groupId &&
                other.id != strip->id) {
                siblings << (other.channelName.isEmpty() ? other.position : other.channelName);
            }
        }
        sharedNote_->setText(siblings.isEmpty()
                                  ? QString()
                                  : QString("shared with %1 (linked)").arg(siblings.join(", ")));
    } else {
        sharedNote_->clear();
    }

    bandCountLabel_->setText(QString("%1/%2 bands").arg(bands_.size()).arg(kMaxBands));
    curve_->setSampleRateHz(48000.0);
    curve_->setBands(std::vector<eqcore::EqBand>(bands_.begin(), bands_.end()));
    curve_->setSelectedBand(selected_);

    rebuildRibbon();
    updateBandControls();
}

void EqEditor::rebuildRibbon() {
    for (QPushButton* chip : ribbonChips_) {
        chip->deleteLater();
    }
    ribbonChips_.clear();

    for (int i = 0; i < bands_.size(); ++i) {
        const eqcore::EqBand& band = bands_.at(i);
        auto* chip = new QPushButton(QString("%1 %2 %3")
                                          .arg(i + 1)
                                          .arg(shortType(band.type), formatFreq(band.freqHz)),
                                      this);
        chip->setCheckable(true);
        chip->setChecked(i == selected_);
        chip->setMaximumWidth(110);
        chip->setStyleSheet(
            QString("QPushButton:checked { background: %1; color: %2; font-weight: bold; }")
                .arg(theme::tokens().accent.name(), theme::tokens().accentText.name()));
        connect(chip, &QPushButton::clicked, this, [this, i] {
            selected_ = i;
            curve_->setSelectedBand(i);
            updateBandControls();
            rebuildRibbon();
        });
        // Before the add button and the trailing stretch.
        ribbonLayout_->insertWidget(ribbonLayout_->count() - 2, chip);
        ribbonChips_.push_back(chip);
    }

    addBandButton_->setEnabled(!stripId_.isEmpty() && bands_.size() < kMaxBands);
    addBandButton_->setToolTip(bands_.size() >= kMaxBands
                                    ? QString("An EQ can hold at most %1 bands.").arg(kMaxBands)
                                    : QString());
}

void EqEditor::updateBandControls() {
    const bool have = selected_ >= 0 && selected_ < bands_.size();

    for (QPushButton* button : typeButtons_) {
        button->setEnabled(have);
    }
    freqSpin_->setEnabled(have);
    gainSpin_->setEnabled(have);
    qSpin_->setEnabled(have);
    removeBandButton_->setEnabled(have);
    copyButton_->setEnabled(!stripId_.isEmpty());

    if (!have) {
        bandTitle_->setText("No band");
        return;
    }

    const eqcore::EqBand& band = bands_.at(selected_);
    bandTitle_->setText(QString("Band %1").arg(selected_ + 1));

    suppressSignals_ = true;
    for (int i = 0; i < typeButtons_.size(); ++i) {
        typeButtons_[i]->setChecked(static_cast<int>(band.type) == i);
    }
    freqSpin_->setValue(band.freqHz);
    gainSpin_->setValue(band.gainDb);
    qSpin_->setValue(band.q);
    suppressSignals_ = false;
}

void EqEditor::pushSelectedBand() {
    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip || selected_ < 0 || selected_ >= bands_.size()) {
        return;
    }
    state_->setChannelEqBand(strip->outputId, strip->channelIndex,
                              static_cast<uint32_t>(selected_), bands_.at(selected_));
    curve_->setBands(std::vector<eqcore::EqBand>(bands_.begin(), bands_.end()));
    curve_->setSelectedBand(selected_);
    updateBandControls();
    rebuildRibbon();
}

void EqEditor::addBand(double freqHz, double gainDb) {
    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip || bands_.size() >= kMaxBands) {
        return;
    }

    eqcore::EqBand band;
    band.type = eqcore::FilterType::Peaking;
    band.freqHz = freqHz;
    band.gainDb = gainDb;
    band.q = 1.0;

    const int index = bands_.size();
    bands_.push_back(band);
    selected_ = index;

    // Count first, then the value: the daemon grows the band list to the new
    // count before the value has anywhere to land. The store's coalescer keeps
    // structural writes ordered ahead of coalesced ones for exactly this.
    state_->setChannelEqBandCount(strip->outputId, strip->channelIndex,
                                   static_cast<uint32_t>(bands_.size()));
    state_->setChannelEqBand(strip->outputId, strip->channelIndex, static_cast<uint32_t>(index),
                              band);

    curve_->setBands(std::vector<eqcore::EqBand>(bands_.begin(), bands_.end()));
    curve_->setSelectedBand(selected_);
    rebuildRibbon();
    updateBandControls();
}

void EqEditor::removeBand(int index) {
    const StripRow* strip = state_->findStrip(stripId_);
    if (!strip || index < 0 || index >= bands_.size()) {
        return;
    }

    // The daemon's band list is positional and only its LENGTH can be set, so
    // removing band i means rewriting every band after it and then shrinking.
    bands_.remove(index);
    for (int i = index; i < bands_.size(); ++i) {
        state_->setChannelEqBand(strip->outputId, strip->channelIndex, static_cast<uint32_t>(i),
                                  bands_.at(i));
    }
    state_->setChannelEqBandCount(strip->outputId, strip->channelIndex,
                                   static_cast<uint32_t>(bands_.size()));

    selected_ = bands_.isEmpty() ? -1 : std::min(index, static_cast<int>(bands_.size()) - 1);
    curve_->setBands(std::vector<eqcore::EqBand>(bands_.begin(), bands_.end()));
    curve_->setSelectedBand(selected_);
    bandCountLabel_->setText(QString("%1/%2 bands").arg(bands_.size()).arg(kMaxBands));
    rebuildRibbon();
    updateBandControls();
}

void EqEditor::showCopyDialog() {
    const StripRow* source = state_->findStrip(stripId_);
    if (!source) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Copy EQ to channel");
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QString("Copy this %1-band curve from \"%2\" to:")
                                      .arg(bands_.size())
                                      .arg(source->label()),
                                  &dialog));

    QVector<QPair<QCheckBox*, StripRow>> targets;
    // Tracks EVERY group already offered, not just the previous strip's.
    // Comparing against only the preceding entry worked for contiguous members
    // and listed a group once per member as soon as its channel indices were
    // non-contiguous, which createLinkGroup permits.
    QSet<QString> offeredGroups;
    for (const StripRow& strip : state_->strips()) {
        if (strip.id == source->id) {
            continue;
        }
        // A linked channel shares its group's curve, so copying onto one member
        // changes the whole group. Offer only one entry per group and say so,
        // rather than listing members that would silently do the same thing.
        const QString groupKey =
            strip.groupId.isEmpty() ? strip.id : (strip.outputId + ":" + strip.groupId);
        if (offeredGroups.contains(groupKey)) {
            continue;
        }
        offeredGroups.insert(groupKey);

        QString label = strip.label();
        if (!strip.groupId.isEmpty()) {
            label += "  (and its linked channels)";
        }
        auto* check = new QCheckBox(label, &dialog);
        layout->addWidget(check);
        targets.push_back({check, strip});
    }

    if (targets.isEmpty()) {
        QMessageBox::information(this, "PipeEQ", "There is no other channel to copy this EQ to.");
        return;
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    for (const auto& [check, target] : targets) {
        if (!check->isChecked()) {
            continue;
        }
        state_->setChannelEqBandCount(target.outputId, target.channelIndex,
                                       static_cast<uint32_t>(bands_.size()));
        for (int i = 0; i < bands_.size(); ++i) {
            state_->setChannelEqBand(target.outputId, target.channelIndex,
                                      static_cast<uint32_t>(i), bands_.at(i));
        }
    }
}

} // namespace pipeeq
