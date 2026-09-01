#include "eq_preview.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include "eq_curve_widget.h"
#include "theme/theme.h"

namespace pipeeq {

EqPreview::EqPreview(QWidget* parent) : QWidget(parent) {
    const theme::Tokens tokens = theme::tokens();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);

    auto* header = new QHBoxLayout;
    caption_ = new QLabel(this);
    caption_->setStyleSheet(QString("color: %1;").arg(tokens.textDim.name()));
    header->addWidget(caption_, 1);
    auto* editButton = new QPushButton("Edit EQ", this);
    connect(editButton, &QPushButton::clicked, this, &EqPreview::activated);
    header->addWidget(editButton);
    layout->addLayout(header);

    curve_ = new EqCurveWidget(this);
    curve_->setPreviewMode(true);
    // A PREVIEW: tall enough to read the shape of a curve, small enough that it
    // doesn't crowd out the mixer row it sits above. The full editor is one
    // click away and gets the whole detail area.
    curve_->setMaximumHeight(190);
    layout->addWidget(curve_, 1);

    setCursor(Qt::PointingHandCursor);
    setToolTip("Click to open the full EQ editor");
}

void EqPreview::setBands(const std::vector<eqcore::EqBand>& bands) {
    curve_->setBands(bands);
}

void EqPreview::setCaption(const QString& caption) {
    caption_->setText(caption);
}

void EqPreview::setSampleRateHz(double sampleRateHz) {
    curve_->setSampleRateHz(sampleRateHz);
}

void EqPreview::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit activated();
    }
}

} // namespace pipeeq
