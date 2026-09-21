#pragma once

#include "rx_config.hpp"
#include "app_translator.hpp"
#include "signal_processor.hpp"

#include <QFile>
#include <QMainWindow>

class QCloseEvent;
class QEvent;
class QResizeEvent;
class QScrollArea;
class QLabel;
class QThread;
class RxWorker;
class SpectrumWidget;
class WaterfallWidget;
class ConstellationPlaceholderWidget;
class WaveformWidget;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class State { Idle, Starting, Running, Stopping };

    void setupAdaptiveContainers();
    void buildPlots();
    void polishUi();
    void applyLanguage(AppLanguage language);
    void retranslateDynamicUi();
    void updateResponsiveLayout();
    void updateCompanyLogo();
    void setupStatusBar();
    void wireUi();
    void updateChannelModes();
    void updateDisplayChannels();
    void syncMarkerControls();
    void updateConnectionUi();
    void refreshIqBufferOptions();
    void setState(State state);
    void appendLog(const QString& text);
    void appendDeveloperLog(const QString& text);
    void initializeSessionLog(const QString& iqPath);
    void relocateSessionLogForIqPath(const QString& iqPath);
    void writeLogLine(const QString& text, bool showInUi);
    void updateWaterfallLevels();
    void saveSpectrumScreenshot();
    RxConfig currentConfig() const;
    bool validateConfig(const RxConfig& cfg, QString& reason) const;
    void connectDevice();
    void disconnectDevice();
    void startCapture();
    void stopCapture();
    void onWorkerStopped();

    Ui::MainWindow* ui = nullptr;
    SpectrumWidget* spectrum_ = nullptr;
    WaterfallWidget* waterfall_ = nullptr;
    ConstellationPlaceholderWidget* constellation_ = nullptr;
    WaveformWidget* waveform_ = nullptr;

    QLabel* footerDeviceLabel_ = nullptr;
    QLabel* footerStreamLabel_ = nullptr;
    QLabel* footerRateLabel_ = nullptr;
    QScrollArea* leftScrollArea_ = nullptr;
    QScrollArea* spectrumToolScrollArea_ = nullptr;

    AppTranslator translator_;
    AppLanguage language_ = AppLanguage::Chinese;

    QThread* rxThread_ = nullptr;
    RxWorker* rxWorker_ = nullptr;
    State state_ = State::Idle;
    bool deviceConnected_ = false;
    bool deviceReleaseConfirmed_ = true;
    LwModel connectedModel_ = LwModel::LW3940;
    bool closeAfterStop_ = false;
    quint64 captureRunSequence_ = 0;

    QFile sessionLogFile_;
    QString sessionLogPath_;
    bool shutdownLogged_ = false;
    bool responsiveLayoutUpdatePending_ = false;
};
