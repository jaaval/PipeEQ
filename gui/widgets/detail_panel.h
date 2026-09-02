#pragma once

#include <QHash>
#include <QWidget>

#include "backend.h"

class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;

namespace pipeeq {

class AppState;
class ElidedLabel;
class EqPreview;
class SendStrip;

// The area above the mixer row: what the current selection is, its channel
// controls, one send fader per input sink, and a small EQ curve that opens the
// full editor.
//
// Sends are shown for the selected CHANNEL, but the send-slot limit is a
// per-output bound on distinct inputs - so the "Add sink" tile reports N/M for
// the whole output rather than for the channel, and the on/off switch of an
// unrouted input is disabled once the output is full. Previously the daemon's
// refusal was discarded and the user got a switch that looked on and did
// nothing.
class DetailPanel : public QWidget {
    Q_OBJECT

public:
    explicit DetailPanel(AppState* state, QWidget* parent = nullptr);

    void setSelection(const QString& stripId);
    QString selection() const { return stripId_; }

    // Values changed but the input set didn't.
    void refreshValues();
    // The input set changed: rebuild the send strips.
    void rebuildSends();
    void refreshMeters();

protected:
    void resizeEvent(QResizeEvent* event) override;

signals:
    void eqEditRequested(const QString& stripId);
    void addInputRequested();

private:
    void connectSendStrip(SendStrip* strip);
    void updateSendsWidth();
    void updateHeader();
    void updateEqPreview();
    void choosePosition();
    void renameChannel();
    void renameOutput();

    AppState* state_;
    QString stripId_;

    QLabel* title_ = nullptr;
    ElidedLabel* subtitle_ = nullptr;
    QPushButton* positionButton_ = nullptr;
    QPushButton* renameButton_ = nullptr;
    QPushButton* autoConnectButton_ = nullptr;

    QWidget* sendsColumn_ = nullptr;
    QWidget* sendHeader_ = nullptr;
    QScrollArea* sendArea_ = nullptr;
    QHBoxLayout* sendLayout_ = nullptr;
    QHash<QString, SendStrip*> sendStrips_;
    QPushButton* addSinkButton_ = nullptr;
    QLabel* sendCountLabel_ = nullptr;

    EqPreview* eqPreview_ = nullptr;
    bool suppressSignals_ = false;
};

} // namespace pipeeq
