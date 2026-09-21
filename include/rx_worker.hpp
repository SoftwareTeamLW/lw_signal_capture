#pragma once

#include "rx_config.hpp"
#include "signal_processor.hpp"

#include <QObject>
#include <atomic>
#include <mutex>
#include <thread>

struct lw39x0_context;

class RxWorker final : public QObject
{
    Q_OBJECT
public:
    explicit RxWorker(RxConfig config, QObject* parent = nullptr);
    ~RxWorker() override;

    // Called directly from the GUI thread. It records the stop request and
    // launches a small interrupter so a blocked lw39x0_recv() can be released
    // without blocking the GUI thread.
    void requestStop() noexcept;

public slots:
    void run();

signals:
    void started();
    void deviceReleased();
    void logMessage(const QString& text);
    // File-only high-detail diagnostics. MainWindow persists these lines without
    // flooding the customer-facing log pane.
    void diagnosticMessage(const QString& text);
    void displayFrameReady(const DisplayFrame& frame);
    void statisticsUpdated(quint64 totalBytes, double mibPerSecond, qint64 elapsedMs);
    void errorOccurred(const QString& message);
    void stopped();

private:
    void configureDevice(lw39x0_context* ctx);
    void cleanup(lw39x0_context*& ctx, bool& receiveStarted) noexcept;
    void registerContext(lw39x0_context* ctx, bool receiveStarted) noexcept;
    void interruptReceive() noexcept;

    RxConfig config_;
    std::atomic_bool stopRequested_{false};
    std::atomic_bool stopIssued_{false};
    std::mutex contextMutex_;
    lw39x0_context* activeContext_ = nullptr;
    bool activeReceiveStarted_ = false;
    std::mutex stopThreadMutex_;
    std::thread stopThread_;
};
