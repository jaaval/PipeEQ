#pragma once

#include <vector>

#include <QMainWindow>

#include "model/app_state.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace pipeeq {

class DetailPanel;
class EqEditor;
class StripRack;

// The application window: a top bar for output management, a detail area for
// the selected channel (its sends and EQ, or the full EQ editor), and the mixer
// strip rack along the bottom.
//
// Holds no state of its own beyond the current selection - everything it draws
// comes from AppState, and every change it makes goes back through it.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Reads from the store rather than calling a backend directly, so nothing
    // it does can block on the daemon and every write goes through the
    // coalescer. Ownership of the store stays with the caller.
    explicit MainWindow(AppState* state, QWidget* parent = nullptr);

    // Opens the EQ editor for the current selection. Exists so a screenshot or
    // a scripted check can reach that page without simulating input.
    void showEqEditor();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void onAddOutputClicked();
    void onRemoveOutputClicked();
    void onRackSelectionChanged(const QString& stripId);
    void onAddInputClicked();
    void onTopologyChanged();
    void onStripsUpdated();
    void onChannelDetailUpdated(const QString& outputId, uint32_t channelIndex);
    void onErrorReported(const QString& message);
    void refreshStrips();
    void refreshDevices();
    void refreshInputs();

private:
    void applyDetailSizing(int pageIndex);
    void restoreSession();
    void saveSession() const;
    void selectStrip(const QString& stripId);
    void loadStripDetail(const StripRow& strip);
    // Refreshes only what can change without the strip set changing: labels,
    // connected state, and the values shown on existing strips. Deliberately
    // does not rebuild anything, since rebuilding destroys widgets and would
    // drop a drag in progress.
    void updateStripStatus();
    const StripRow* findStrip(const QString& stripId) const;
    // The device row for a strip's target, or null when it isn't present.
    const DeviceRow* findDevice(const QString& nodeName) const;

    AppState* state_;

    StripRack* stripRack_ = nullptr;
    DetailPanel* detailPanel_ = nullptr;
    QVBoxLayout* rootLayout_ = nullptr;
    QStackedWidget* detailStack_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;

    EqEditor* eqEditor_ = nullptr;

    QLabel* statusLabel_ = nullptr;

    QString currentStripId_;
    // A selection restored from QSettings, applied once that channel exists.
    QString pendingSelection_;
    // Still needed for programmatic widget updates - a QSignalBlocker in all
    // but name. It no longer has anything to do with reconciling daemon values;
    // that is EditGuard's job in the store now.
    bool suppressSignals_ = false;
};

} // namespace pipeeq
