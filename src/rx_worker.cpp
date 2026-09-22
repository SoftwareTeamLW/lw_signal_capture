#include "rx_worker.hpp"

#include <lw39x0.h>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QList>
#include <QStorageInfo>
#include <QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
constexpr qint64 kPreviewIntervalMs = 50;       // target ~20 FPS
constexpr qint64 kStatsIntervalMs = 500;
constexpr qint64 kIqCheckIntervalMs = 1000;
constexpr qint64 kDeveloperStatusIntervalMs = 2000;
constexpr qint64 kStorageStartupStatusIntervalMs = 1000;
constexpr qint64 kStorageSteadyStatusIntervalMs = 2000;
constexpr qint64 kStorageStartupWindowMs = 5000;
constexpr double kStorageProtectiveStopFillPercent = 85.0;
constexpr std::size_t kAlignment = 32 * 1024;
constexpr std::size_t kProbeBytes = 256;
constexpr std::size_t kAnalysisShorts = 32768;
constexpr std::size_t kWriterQueueMinBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kWriterBatchMaxBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t kWriterDirectAlignment = 4096ULL;

struct FreeDeleter {
    void operator()(char* p) const noexcept { std::free(p); }
};

struct RawStats {
    std::size_t count = 0;
    std::size_t nonZero = 0;
    std::size_t transitions = 0;
    std::int16_t minValue = 0;
    std::int16_t maxValue = 0;
    double rms = 0.0;

    bool allZero() const noexcept { return count > 0 && nonZero == 0; }
    bool constant() const noexcept { return count > 0 && minValue == maxValue; }
    bool active() const noexcept { return count > 0 && !allZero() && !constant(); }
    double nonZeroPercent() const noexcept
    {
        return count ? 100.0 * static_cast<double>(nonZero) / static_cast<double>(count) : 0.0;
    }
};

struct WriterSnapshot
{
    std::size_t queuedSlots = 0;
    std::size_t slotCount = 0;
    std::size_t slotSize = 0;
    std::size_t peakQueuedSlots = 0;
    quint64 queuedBytes = 0;
    quint64 capacityBytes = 0;
    quint64 peakQueuedBytes = 0;
    quint64 totalEnqueued = 0;
    quint64 totalWritten = 0;
    quint64 writeCalls = 0;
    quint64 totalWriteUsec = 0;
    quint64 maxWriteUsec = 0;
    quint64 maxWriteBytes = 0;
    quint64 maxBatchSlots = 0;
    quint64 slowWriteCalls = 0;
    quint64 flushCalls = 0;
    quint64 totalFlushUsec = 0;
    quint64 maxFlushUsec = 0;
    bool failed = false;

    double fillPercent() const noexcept
    {
        return capacityBytes ? 100.0 * static_cast<double>(queuedBytes)
            / static_cast<double>(capacityBytes) : 0.0;
    }

    quint64 queuedBytesApprox() const noexcept
    {
        return queuedBytes;
    }
};

void atomicMax(std::atomic<quint64>& target, quint64 value) noexcept
{
    quint64 current = target.load(std::memory_order_relaxed);
    while (current < value
           && !target.compare_exchange_weak(current, value,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    }
}

struct ProcIoSnapshot
{
    quint64 wcharBytes = 0;
    quint64 writeSyscalls = 0;
    quint64 writeBytes = 0;
    quint64 cancelledWriteBytes = 0;
    bool valid = false;
};

quint64 parseProcCounter(const QByteArray& data, const QByteArray& key)
{
    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray& line : lines) {
        if (!line.startsWith(key)) continue;
        const int colon = line.indexOf(':');
        if (colon < 0) return 0;
        bool ok = false;
        const quint64 value = line.mid(colon + 1).trimmed().toULongLong(&ok);
        return ok ? value : 0;
    }
    return 0;
}

ProcIoSnapshot readProcIo()
{
    ProcIoSnapshot out;
    QFile file(QStringLiteral("/proc/self/io"));
    if (!file.open(QIODevice::ReadOnly)) return out;
    const QByteArray data = file.readAll();
    out.wcharBytes = parseProcCounter(data, QByteArrayLiteral("wchar"));
    out.writeSyscalls = parseProcCounter(data, QByteArrayLiteral("syscw"));
    out.writeBytes = parseProcCounter(data, QByteArrayLiteral("write_bytes"));
    out.cancelledWriteBytes = parseProcCounter(data, QByteArrayLiteral("cancelled_write_bytes"));
    out.valid = true;
    return out;
}

struct MemInfoSnapshot
{
    quint64 availableBytes = 0;
    quint64 dirtyBytes = 0;
    quint64 writebackBytes = 0;
    bool valid = false;
};

quint64 parseMemInfoKb(const QByteArray& data, const QByteArray& key)
{
    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray& line : lines) {
        if (!line.startsWith(key)) continue;
        const int colon = line.indexOf(':');
        if (colon < 0) return 0;
        const QList<QByteArray> parts = line.mid(colon + 1).simplified().split(' ');
        if (parts.isEmpty()) return 0;
        bool ok = false;
        const quint64 kb = parts[0].toULongLong(&ok);
        return ok ? kb * 1024ULL : 0;
    }
    return 0;
}

MemInfoSnapshot readMemInfo()
{
    MemInfoSnapshot out;
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly)) return out;
    const QByteArray data = file.readAll();
    out.availableBytes = parseMemInfoKb(data, QByteArrayLiteral("MemAvailable"));
    out.dirtyBytes = parseMemInfoKb(data, QByteArrayLiteral("Dirty"));
    out.writebackBytes = parseMemInfoKb(data, QByteArrayLiteral("Writeback"));
    out.valid = true;
    return out;
}

struct NvmeThermalSnapshot
{
    bool valid = false;
    double compositeC = 0.0;
    double maxSensorC = 0.0;
    QString source;
};

bool readMilliCFile(const QString& path, double& celsius)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    bool ok = false;
    const qint64 milli = file.readAll().trimmed().toLongLong(&ok);
    if (!ok) return false;
    celsius = milli / 1000.0;
    return true;
}

NvmeThermalSnapshot readNvmeThermal()
{
    NvmeThermalSnapshot best;
    QDir root(QStringLiteral("/sys/class/hwmon"));
    const QStringList hwmons = root.entryList({QStringLiteral("hwmon*")},
        QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& hw : hwmons) {
        const QString base = root.filePath(hw);
        QFile nameFile(base + QStringLiteral("/name"));
        if (!nameFile.open(QIODevice::ReadOnly)) continue;
        const QString name = QString::fromLatin1(nameFile.readAll()).trimmed().toLower();
        if (!name.contains(QStringLiteral("nvme"))) continue;

        double composite = 0.0;
        if (!readMilliCFile(base + QStringLiteral("/temp1_input"), composite)) continue;
        double maxSensor = composite;
        for (int sensor = 2; sensor <= 9; ++sensor) {
            double value = 0.0;
            if (readMilliCFile(base + QStringLiteral("/temp%1_input").arg(sensor), value))
                maxSensor = std::max(maxSensor, value);
        }
        if (!best.valid || maxSensor > best.maxSensorC) {
            best.valid = true;
            best.compositeC = composite;
            best.maxSensorC = maxSensor;
            best.source = hw;
        }
    }
    return best;
}

QString nvmeThermalText(const NvmeThermalSnapshot& t)
{
    if (!t.valid) return QStringLiteral("N/A");
    return QStringLiteral("%1/%2 C")
        .arg(t.compositeC, 0, 'f', 1)
        .arg(t.maxSensorC, 0, 'f', 1);
}

QString bytesText(quint64 bytes);

QString storageModeText(IqStorageMode mode)
{
    switch (mode) {
    case IqStorageMode::Buffered: return QStringLiteral("Buffered");
    case IqStorageMode::Direct: return QStringLiteral("Direct");
    default: return QStringLiteral("Auto");
    }
}

class AsyncIqWriter final
{
public:
    ~AsyncIqWriter() { finish(); }

    bool start(const QString& path, std::size_t blockBytes,
               std::size_t requestedCapacityBytes, IqStorageMode requestedMode,
               QString& error)
    {
        path_ = path;
        slotSize_ = std::max<std::size_t>(1, blockBytes);
        capacityBytes_ = std::max<std::size_t>(requestedCapacityBytes, kWriterQueueMinBytes);
        capacityBytes_ = (capacityBytes_ / kWriterDirectAlignment) * kWriterDirectAlignment;
        if (capacityBytes_ < kWriterQueueMinBytes) capacityBytes_ = kWriterQueueMinBytes;

        const auto allocBegin = std::chrono::steady_clock::now();
        void* raw = nullptr;
        const int allocRet = ::posix_memalign(&raw, kWriterDirectAlignment, capacityBytes_);
        if (allocRet != 0 || !raw) {
            error = QStringLiteral("IQ 写入缓存分配失败：请求 %1 | errno=%2 (%3)")
                .arg(bytesText(static_cast<quint64>(capacityBytes_)))
                .arg(allocRet)
                .arg(QString::fromLocal8Bit(std::strerror(allocRet)));
            return false;
        }
        ring_.reset(static_cast<char*>(raw));
        // Commit/prefault the selected queue before RX starts. This moves page
        // faults out of the multi-GiB/s receive hot path and also makes an
        // over-aggressive manual memory choice fail before acquisition.
        std::memset(ring_.get(), 0, capacityBytes_);
        allocationUsec_ = static_cast<quint64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - allocBegin).count());

        const QByteArray pathBytes = path.toLocal8Bit();
        requestedMode_ = requestedMode;
        directOpenErrno_ = 0;

        // V1.1 Auto intentionally prefers buffered sequential fwrite(), which
        // is closer to the vendor recv_demo. Direct I/O is opt-in so field
        // deployments can compare thermal behaviour without losing the V1.0
        // continuity checks and bounded application queue.
        if (requestedMode == IqStorageMode::Direct) {
            errno = 0;
#ifdef O_DIRECT
            directFd_ = ::open(pathBytes.constData(),
                O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT | O_CLOEXEC, 0666);
#else
            directFd_ = -1;
            errno = EOPNOTSUPP;
#endif
            if (directFd_ < 0) {
                directOpenErrno_ = errno;
                ring_.reset();
                error = QStringLiteral("Direct I/O 打开失败：%1 | errno=%2 (%3)")
                    .arg(path).arg(directOpenErrno_)
                    .arg(QString::fromLocal8Bit(std::strerror(directOpenErrno_)));
                return false;
            }
            backend_ = Backend::Direct;
        } else {
            errno = 0;
            output_ = std::fopen(pathBytes.constData(), "wb");
            if (!output_) {
                const int e = errno;
                ring_.reset();
                error = QStringLiteral("IQ 文件创建失败：%1 | errno=%2 (%3)")
                    .arg(path).arg(e)
                    .arg(QString::fromLocal8Bit(std::strerror(e)));
                return false;
            }
            backend_ = Backend::Buffered;
        }

        stopping_ = false;
        headByte_ = 0;
        tailByte_ = 0;
        usedBytes_ = 0;
        peakUsedBytes_ = 0;
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            errorText_.clear();
        }
        ioFailed_.store(false, std::memory_order_release);
        totalEnqueued_.store(0, std::memory_order_release);
        totalWritten_.store(0, std::memory_order_release);
        writeCalls_.store(0, std::memory_order_release);
        totalWriteUsec_.store(0, std::memory_order_release);
        maxWriteUsec_.store(0, std::memory_order_release);
        maxWriteBytes_.store(0, std::memory_order_release);
        maxBatchSlots_.store(0, std::memory_order_release);
        slowWriteCalls_.store(0, std::memory_order_release);
        flushCalls_.store(0, std::memory_order_release);
        totalFlushUsec_.store(0, std::memory_order_release);
        maxFlushUsec_.store(0, std::memory_order_release);

        try {
            worker_ = std::thread([this] { writerLoop(); });
        } catch (...) {
            closeOutputs();
            ring_.reset();
            error = QStringLiteral("IQ 写入线程启动失败");
            return false;
        }
        return true;
    }

    bool enqueue(const char* data, std::size_t bytes, QString& reason)
    {
        if ((!output_ && directFd_ < 0) || !data || bytes == 0) return true;
        if (bytes > capacityBytes_) {
            reason = QStringLiteral("IQ 数据块超过写入缓存容量：%1 > %2 B")
                .arg(static_cast<qulonglong>(bytes))
                .arg(static_cast<qulonglong>(capacityBytes_));
            return false;
        }
        if (ioFailed_.load(std::memory_order_acquire)) {
            reason = errorText();
            if (reason.isEmpty()) reason = QStringLiteral("IQ 文件写入失败");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                reason = QStringLiteral("IQ 写入队列已停止");
                return false;
            }
            if (capacityBytes_ - usedBytes_ < bytes) {
                reason = QStringLiteral(
                    "存储速度不足或缓存配置过小：IQ 写入缓存已满，采集已停止以避免文件跳块");
                return false;
            }

            const std::size_t first = std::min(bytes, capacityBytes_ - tailByte_);
            std::memcpy(ring_.get() + tailByte_, data, first);
            if (bytes > first)
                std::memcpy(ring_.get(), data + first, bytes - first);
            tailByte_ = (tailByte_ + bytes) % capacityBytes_;
            usedBytes_ += bytes;
            peakUsedBytes_ = std::max(peakUsedBytes_, usedBytes_);
            totalEnqueued_.fetch_add(static_cast<quint64>(bytes), std::memory_order_acq_rel);
        }
        cv_.notify_one();
        return true;
    }

    void finish()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();

        if (backend_ == Backend::Direct && directFd_ >= 0) {
            // Direct writer drains all aligned bytes. At most 4095 bytes may
            // remain. Close O_DIRECT first, then append the exact tail without
            // padding so the raw IQ file byte stream remains unchanged.
            std::vector<char> tail;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!failed() && usedBytes_ > 0) {
                    tail.resize(usedBytes_);
                    const std::size_t first = std::min(usedBytes_, capacityBytes_ - headByte_);
                    std::memcpy(tail.data(), ring_.get() + headByte_, first);
                    if (usedBytes_ > first)
                        std::memcpy(tail.data() + first, ring_.get(), usedBytes_ - first);
                }
            }

            if (!failed()) {
                const auto syncBegin = std::chrono::steady_clock::now();
                errno = 0;
                const int syncRet = ::fdatasync(directFd_);
                const quint64 syncUsec = static_cast<quint64>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - syncBegin).count());
                flushCalls_.fetch_add(1, std::memory_order_acq_rel);
                totalFlushUsec_.fetch_add(syncUsec, std::memory_order_acq_rel);
                atomicMax(maxFlushUsec_, syncUsec);
                if (syncRet != 0) {
                    const int e = errno;
                    setIoError(QStringLiteral("IQ O_DIRECT 最终 fdatasync 失败：errno=%1 (%2)")
                        .arg(e).arg(QString::fromLocal8Bit(std::strerror(e))));
                }
            }
            ::close(directFd_);
            directFd_ = -1;

            if (!failed() && !tail.empty()) {
                const QByteArray pathBytes = path_.toLocal8Bit();
                errno = 0;
                const int tailFd = ::open(pathBytes.constData(), O_WRONLY | O_APPEND | O_CLOEXEC);
                if (tailFd < 0) {
                    const int e = errno;
                    setIoError(QStringLiteral("IQ 文件尾部写入打开失败：errno=%1 (%2)")
                        .arg(e).arg(QString::fromLocal8Bit(std::strerror(e))));
                } else {
                    std::size_t done = 0;
                    while (done < tail.size()) {
                        errno = 0;
                        const ssize_t n = ::write(tailFd, tail.data() + done, tail.size() - done);
                        if (n < 0 && errno == EINTR) continue;
                        if (n <= 0) {
                            const int e = errno;
                            setIoError(QStringLiteral("IQ 文件尾部写入失败：errno=%1 (%2)")
                                .arg(e).arg(QString::fromLocal8Bit(std::strerror(e))));
                            break;
                        }
                        done += static_cast<std::size_t>(n);
                    }
                    if (done > 0) {
                        totalWritten_.fetch_add(static_cast<quint64>(done), std::memory_order_acq_rel);
                        std::lock_guard<std::mutex> lock(mutex_);
                        headByte_ = (headByte_ + done) % capacityBytes_;
                        usedBytes_ -= std::min(usedBytes_, done);
                    }
                    if (!failed()) (void)::fdatasync(tailFd);
                    ::close(tailFd);
                }
            }
        } else if (output_) {
            if (!failed()) {
                const auto flushBegin = std::chrono::steady_clock::now();
                errno = 0;
                if (std::fflush(output_) != 0) {
                    const int e = errno;
                    setIoError(QStringLiteral("IQ 文件最终刷新失败：errno=%1 (%2)")
                        .arg(e).arg(QString::fromLocal8Bit(std::strerror(e))));
                } else {
                    const int fd = ::fileno(output_);
                    errno = 0;
                    if (fd >= 0 && ::fdatasync(fd) != 0) {
                        const int e = errno;
                        setIoError(QStringLiteral("IQ 文件最终 fdatasync 失败：errno=%1 (%2)")
                            .arg(e).arg(QString::fromLocal8Bit(std::strerror(e))));
                    }
                }
                const quint64 flushUsec = static_cast<quint64>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - flushBegin).count());
                flushCalls_.fetch_add(1, std::memory_order_acq_rel);
                totalFlushUsec_.fetch_add(flushUsec, std::memory_order_acq_rel);
                atomicMax(maxFlushUsec_, flushUsec);
            }
            errno = 0;
            if (std::fclose(output_) != 0 && !failed()) {
                const int e = errno;
                setIoError(QStringLiteral("IQ 文件关闭失败：errno=%1 (%2)")
                    .arg(e).arg(QString::fromLocal8Bit(std::strerror(e))));
            }
            output_ = nullptr;
        }

        ring_.reset();
    }

    WriterSnapshot snapshot() const
    {
        WriterSnapshot out;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            out.queuedBytes = static_cast<quint64>(usedBytes_);
            out.capacityBytes = static_cast<quint64>(capacityBytes_);
            out.peakQueuedBytes = static_cast<quint64>(peakUsedBytes_);
            out.slotSize = slotSize_;
            out.slotCount = slotSize_ ? (capacityBytes_ + slotSize_ - 1) / slotSize_ : 0;
            out.queuedSlots = slotSize_ ? (usedBytes_ + slotSize_ - 1) / slotSize_ : 0;
            out.peakQueuedSlots = slotSize_ ? (peakUsedBytes_ + slotSize_ - 1) / slotSize_ : 0;
        }
        out.totalEnqueued = totalEnqueued_.load(std::memory_order_acquire);
        out.totalWritten = totalWritten_.load(std::memory_order_acquire);
        out.writeCalls = writeCalls_.load(std::memory_order_acquire);
        out.totalWriteUsec = totalWriteUsec_.load(std::memory_order_acquire);
        out.maxWriteUsec = maxWriteUsec_.load(std::memory_order_acquire);
        out.maxWriteBytes = maxWriteBytes_.load(std::memory_order_acquire);
        out.maxBatchSlots = maxBatchSlots_.load(std::memory_order_acquire);
        out.slowWriteCalls = slowWriteCalls_.load(std::memory_order_acquire);
        out.flushCalls = flushCalls_.load(std::memory_order_acquire);
        out.totalFlushUsec = totalFlushUsec_.load(std::memory_order_acquire);
        out.maxFlushUsec = maxFlushUsec_.load(std::memory_order_acquire);
        out.failed = failed();
        return out;
    }

    quint64 totalWritten() const noexcept { return totalWritten_.load(std::memory_order_acquire); }
    quint64 totalEnqueued() const noexcept { return totalEnqueued_.load(std::memory_order_acquire); }
    std::size_t capacityBytes() const noexcept { return capacityBytes_; }
    bool failed() const noexcept { return ioFailed_.load(std::memory_order_acquire); }
    bool directIo() const noexcept { return backend_ == Backend::Direct; }
    int directOpenErrno() const noexcept { return directOpenErrno_; }
    quint64 allocationUsec() const noexcept { return allocationUsec_; }
    QString backendName() const
    {
        return backend_ == Backend::Direct
            ? QStringLiteral("Direct I/O / O_DIRECT")
            : QStringLiteral("Buffered / fwrite");
    }

    IqStorageMode requestedMode() const noexcept { return requestedMode_; }

    QString errorText() const
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return errorText_;
    }

private:
    enum class Backend { None, Direct, Buffered };

    void setIoError(const QString& text)
    {
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            errorText_ = text;
        }
        ioFailed_.store(true, std::memory_order_release);
    }

    void closeOutputs() noexcept
    {
        if (directFd_ >= 0) {
            ::close(directFd_);
            directFd_ = -1;
        }
        if (output_) {
            std::fclose(output_);
            output_ = nullptr;
        }
    }

    void writerLoop()
    {
        using clock = std::chrono::steady_clock;

        for (;;) {
            std::size_t bytes = 0;
            std::size_t head = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Direct mode batches up to 8 MiB to reduce syscall overhead.
                // Buffered mode deliberately drains one RX block at a time so
                // its write pattern remains close to the vendor recv_demo.
                const std::size_t wakeBytes = backend_ == Backend::Direct
                    ? kWriterBatchMaxBytes : slotSize_;
                cv_.wait_for(lock, std::chrono::milliseconds(10), [this, wakeBytes] {
                    return stopping_ || usedBytes_ >= wakeBytes;
                });

                if (usedBytes_ == 0) {
                    if (stopping_) break;
                    continue;
                }

                if (backend_ == Backend::Direct && usedBytes_ < kWriterDirectAlignment) {
                    if (stopping_) break; // exact tail is appended in finish()
                    continue;
                }

                head = headByte_;
                const std::size_t contiguous = std::min(usedBytes_, capacityBytes_ - headByte_);
                const std::size_t batchLimit = backend_ == Backend::Direct
                    ? kWriterBatchMaxBytes : slotSize_;
                bytes = std::min(contiguous, batchLimit);
                if (backend_ == Backend::Direct)
                    bytes = (bytes / kWriterDirectAlignment) * kWriterDirectAlignment;
                if (bytes == 0) {
                    if (stopping_) break;
                    continue;
                }
            }

            if (ioFailed_.load(std::memory_order_acquire)) break;

            const char* const source = ring_.get() + head;
            const auto writeBegin = clock::now();
            std::size_t writtenTotal = 0;
            int writeErrno = 0;

            if (backend_ == Backend::Direct) {
                while (writtenTotal < bytes) {
                    errno = 0;
                    const ssize_t n = ::write(directFd_, source + writtenTotal, bytes - writtenTotal);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) {
                        writeErrno = errno;
                        break;
                    }
                    const std::size_t got = static_cast<std::size_t>(n);
                    writtenTotal += got;
                    if (writtenTotal < bytes && (got % kWriterDirectAlignment) != 0) {
                        writeErrno = EIO;
                        break;
                    }
                }
            } else {
                errno = 0;
                writtenTotal = std::fwrite(source, 1, bytes, output_);
                if (writtenTotal != bytes) writeErrno = errno;
            }

            const quint64 writeUsec = static_cast<quint64>(
                std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - writeBegin).count());
            writeCalls_.fetch_add(1, std::memory_order_acq_rel);
            totalWriteUsec_.fetch_add(writeUsec, std::memory_order_acq_rel);
            atomicMax(maxWriteUsec_, writeUsec);
            atomicMax(maxWriteBytes_, static_cast<quint64>(bytes));
            atomicMax(maxBatchSlots_, static_cast<quint64>(
                slotSize_ ? (bytes + slotSize_ - 1) / slotSize_ : 1));
            if (writeUsec >= 10'000)
                slowWriteCalls_.fetch_add(1, std::memory_order_acq_rel);

            if (writtenTotal > 0)
                totalWritten_.fetch_add(static_cast<quint64>(writtenTotal), std::memory_order_acq_rel);
            if (writtenTotal != bytes) {
                setIoError(QStringLiteral("IQ 文件写入失败：backend=%1 %2/%3 B | errno=%4 (%5)")
                    .arg(backendName())
                    .arg(static_cast<qulonglong>(writtenTotal))
                    .arg(static_cast<qulonglong>(bytes))
                    .arg(writeErrno)
                    .arg(QString::fromLocal8Bit(std::strerror(writeErrno))));
                break;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                headByte_ = (headByte_ + bytes) % capacityBytes_;
                usedBytes_ -= bytes;
            }

        }
    }

    Backend backend_ = Backend::None;
    IqStorageMode requestedMode_ = IqStorageMode::Auto;
    QString path_;
    int directFd_ = -1;
    int directOpenErrno_ = 0;
    quint64 allocationUsec_ = 0;
    std::FILE* output_ = nullptr;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<char, FreeDeleter> ring_;
    std::size_t capacityBytes_ = 0;
    std::size_t slotSize_ = 0;
    std::size_t headByte_ = 0;
    std::size_t tailByte_ = 0;
    std::size_t usedBytes_ = 0;
    std::size_t peakUsedBytes_ = 0;
    bool stopping_ = false;

    std::atomic_bool ioFailed_{false};
    std::atomic<quint64> totalEnqueued_{0};
    std::atomic<quint64> totalWritten_{0};
    std::atomic<quint64> writeCalls_{0};
    std::atomic<quint64> totalWriteUsec_{0};
    std::atomic<quint64> maxWriteUsec_{0};
    std::atomic<quint64> maxWriteBytes_{0};
    std::atomic<quint64> maxBatchSlots_{0};
    std::atomic<quint64> slowWriteCalls_{0};
    std::atomic<quint64> flushCalls_{0};
    std::atomic<quint64> totalFlushUsec_{0};
    std::atomic<quint64> maxFlushUsec_{0};
    mutable std::mutex errorMutex_;
    QString errorText_;
};

struct PreviewSnapshot
{
    quint64 submitted = 0;
    quint64 processed = 0;
    quint64 replaced = 0;
    quint64 failures = 0;
};

class LatestPreviewProcessor final
{
public:
    using Callback = std::function<void(const DisplayFrame&)>;

    LatestPreviewProcessor(RxConfig config, Callback callback)
        : config_(std::move(config)), callback_(std::move(callback))
    {
    }

    ~LatestPreviewProcessor() { finish(); }

    bool start() noexcept
    {
        stopping_ = false;
        try {
            worker_ = std::thread([this] { processingLoop(); });
        } catch (...) {
            stopping_ = true;
            return false;
        }
        return true;
    }

    void submit(const char* data, std::size_t bytes)
    {
        if (!data || bytes == 0) return;

        const std::size_t required = static_cast<std::size_t>(config_.fftSize)
            * static_cast<std::size_t>(std::max(1, enabledChannelCount(config_)))
            * 2U * sizeof(std::int16_t);
        const std::size_t copyBytes = std::min(bytes, required);

        std::vector<char> block;
        try {
            block.resize(copyBytes);
            std::memcpy(block.data(), data, copyBytes);
        } catch (...) {
            failures_.fetch_add(1, std::memory_order_acq_rel);
            // Preview is best-effort. Never let display-side allocation failure
            // interrupt the raw acquisition path.
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            if (pending_)
                replaced_.fetch_add(1, std::memory_order_acq_rel);
            latest_ = std::move(block); // replace stale preview; acquisition never waits
            pending_ = true;
            submitted_.fetch_add(1, std::memory_order_acq_rel);
        }
        cv_.notify_one();
    }

    void finish()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            pending_ = false;
            latest_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    PreviewSnapshot snapshot() const noexcept
    {
        PreviewSnapshot out;
        out.submitted = submitted_.load(std::memory_order_acquire);
        out.processed = processed_.load(std::memory_order_acquire);
        out.replaced = replaced_.load(std::memory_order_acquire);
        out.failures = failures_.load(std::memory_order_acquire);
        return out;
    }

private:
    void processingLoop()
    {
        for (;;) {
            std::vector<char> local;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || pending_; });
                if (stopping_) break;
                local.swap(latest_);
                pending_ = false;
            }

            if (local.empty()) continue;
            try {
                const DisplayFrame frame = processor_.makeFrame(local.data(), local.size(), config_);
                bool anySpectrum = false;
                for (int ch = 0; ch < 8; ++ch) {
                    if (config_.enabled[ch] && !frame.spectrumDb[ch].isEmpty()) {
                        anySpectrum = true;
                        break;
                    }
                }
                processed_.fetch_add(1, std::memory_order_acq_rel);
                if (anySpectrum && callback_) callback_(frame);
            } catch (...) {
                failures_.fetch_add(1, std::memory_order_acq_rel);
                // Display work is best-effort; a bad preview frame must not
                // terminate the native worker thread or the acquisition.
            }
        }
    }

    RxConfig config_;
    Callback callback_;
    SignalProcessor processor_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<char> latest_;
    bool pending_ = false;
    bool stopping_ = false;
    std::atomic<quint64> submitted_{0};
    std::atomic<quint64> processed_{0};
    std::atomic<quint64> replaced_{0};
    std::atomic<quint64> failures_{0};
};

lw39x0_rx_channels toSdkChannels(const RxConfig& c)
{
    lw39x0_rx_channels out{};
    out.channel_rx_A1 = c.enabled[0];
    out.channel_rx_A2 = c.enabled[1];
    out.channel_rx_B1 = c.enabled[2];
    out.channel_rx_B2 = c.enabled[3];
    out.channel_rx_C1 = c.enabled[4];
    out.channel_rx_C2 = c.enabled[5];
    out.channel_rx_D1 = c.enabled[6];
    out.channel_rx_D2 = c.enabled[7];
    return out;
}

QString channelMatrixText(const RxConfig& c)
{
    QStringList names;
    for (int ch = 0; ch < 8; ++ch) {
        if (c.enabled[ch]) names << channelName(static_cast<LwChannel>(ch));
    }
    return names.isEmpty() ? QStringLiteral("-") : names.join(QStringLiteral("+"));
}

void openAndGainSelected(lw39x0_context* ctx, const RxConfig& c)
{
#define OPEN_AND_GAIN(zone, n, idx) \
    if (c.enabled[idx]) { \
        lw39x0_set_##zone##_zone_rx##n##_rf_powerdown(ctx, false); \
        lw39x0_set_##zone##_zone_rx##n##_gain(ctx, c.gainDb); \
    }
    OPEN_AND_GAIN(A, 1, 0)
    OPEN_AND_GAIN(A, 2, 1)
    OPEN_AND_GAIN(B, 1, 2)
    OPEN_AND_GAIN(B, 2, 3)
    OPEN_AND_GAIN(C, 1, 4)
    OPEN_AND_GAIN(C, 2, 5)
    OPEN_AND_GAIN(D, 1, 6)
    OPEN_AND_GAIN(D, 2, 7)
#undef OPEN_AND_GAIN
}

RawStats analyzeRaw(const char* buffer, std::size_t bytes,
                    std::size_t maxShorts = kAnalysisShorts)
{
    RawStats out;
    if (!buffer || bytes < sizeof(std::int16_t)) return out;

    const auto* s = reinterpret_cast<const std::int16_t*>(buffer);
    const std::size_t available = bytes / sizeof(std::int16_t);
    const std::size_t count = std::min(available, maxShorts);
    if (count == 0) return out;

    out.count = count;
    out.minValue = std::numeric_limits<std::int16_t>::max();
    out.maxValue = std::numeric_limits<std::int16_t>::min();

    long double sumSquares = 0.0L;
    std::int16_t previous = s[0];
    for (std::size_t i = 0; i < count; ++i) {
        const std::int16_t v = s[i];
        if (v != 0) ++out.nonZero;
        if (i > 0 && v != previous) ++out.transitions;
        previous = v;
        out.minValue = std::min(out.minValue, v);
        out.maxValue = std::max(out.maxValue, v);
        const long double x = static_cast<long double>(v);
        sumSquares += x * x;
    }
    out.rms = std::sqrt(static_cast<double>(sumSquares / static_cast<long double>(count)));
    return out;
}

QString statsText(const RawStats& s)
{
    if (s.count == 0) return QStringLiteral("无可用采样");
    return QStringLiteral("非零率 %1% | RMS %2 LSB | 范围 %3…%4")
        .arg(s.nonZeroPercent(), 0, 'f', 2)
        .arg(s.rms, 0, 'f', 1)
        .arg(s.minValue)
        .arg(s.maxValue);
}

std::array<unsigned char, kProbeBytes> makeProbePattern()
{
    std::array<unsigned char, kProbeBytes> out{};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<unsigned char>((0x5Au + i * 37u + (i >> 1u) * 11u) & 0xFFu);
    return out;
}

bool probeWasOverwritten(const char* buffer,
                         const std::array<unsigned char, kProbeBytes>& pattern,
                         double* unchangedPercent = nullptr)
{
    std::size_t unchanged = 0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (static_cast<unsigned char>(buffer[i]) == pattern[i]) ++unchanged;
    }
    const double pct = 100.0 * static_cast<double>(unchanged)
        / static_cast<double>(pattern.size());
    if (unchangedPercent) *unchangedPercent = pct;
    return pct < 90.0;
}

QString perChannelSummary(const char* raw, std::size_t bytes, const RxConfig& cfg)
{
    const int channelCount = enabledChannelCount(cfg);
    if (!raw || channelCount <= 0) return QString();

    const auto* s = reinterpret_cast<const std::int16_t*>(raw);
    const std::size_t shortCount = bytes / sizeof(std::int16_t);
    const std::size_t stride = static_cast<std::size_t>(2 * channelCount);
    const std::size_t complexCount = std::min<std::size_t>(shortCount / stride, 4096);
    if (complexCount == 0) return QString();

    QStringList parts;
    int packed = 0;
    for (int absolute = 0; absolute < 8; ++absolute) {
        if (!cfg.enabled[absolute]) continue;

        long double sumSquares = 0.0L;
        int peak = 0;
        std::size_t nonZeroPairs = 0;
        const std::size_t offset = static_cast<std::size_t>(2 * packed);
        for (std::size_t k = 0; k < complexCount; ++k) {
            const std::size_t base = k * stride + offset;
            const int i = static_cast<int>(s[base]);
            const int q = static_cast<int>(s[base + 1]);
            if (i != 0 || q != 0) ++nonZeroPairs;
            peak = std::max(peak, std::max(std::abs(i), std::abs(q)));
            sumSquares += static_cast<long double>(i) * i
                        + static_cast<long double>(q) * q;
        }
        const double rms = std::sqrt(static_cast<double>(
            sumSquares / static_cast<long double>(2 * complexCount)));
        const double nz = 100.0 * static_cast<double>(nonZeroPairs)
                        / static_cast<double>(complexCount);
        parts << QStringLiteral("%1 %2%/%3/%4")
                     .arg(channelName(static_cast<LwChannel>(absolute)))
                     .arg(nz, 0, 'f', 1)
                     .arg(rms, 0, 'f', 1)
                     .arg(peak);
        ++packed;
    }
    return parts.join(QStringLiteral(" | "));
}

QString mibText(quint64 bytes)
{
    return QStringLiteral("%1 MiB").arg(bytes / 1048576.0, 0, 'f', 1);
}

QString runTag(const RxConfig& cfg)
{
    return QStringLiteral("Run#%1")
        .arg(cfg.captureRunId, 3, 10, QLatin1Char('0'));
}

QString bytesText(quint64 bytes)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        return QStringLiteral("%1 GiB").arg(bytes / 1073741824.0, 0, 'f', 2);
    if (bytes >= 1024ULL * 1024ULL)
        return QStringLiteral("%1 MiB").arg(bytes / 1048576.0, 0, 'f', 1);
    if (bytes >= 1024ULL)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}


bool writeCaptureMetadata(const RxConfig& cfg,
                          qint64 rxElapsedMs,
                          quint64 recvBytes,
                          quint64 recvCalls,
                          quint64 partialBlocks,
                          qint64 maxReceiveGapMs,
                          bool validIq,
                          bool storageStopped,
                          bool abnormal,
                          const QString& terminationReason,
                          const WriterSnapshot& writer,
                          quint64 fileBytes)
{
    if (!cfg.saveIq || cfg.captureMetadataPath.isEmpty()) return false;
    const int channels = std::max(1, enabledChannelCount(cfg));
    const quint64 expectedBps = static_cast<quint64>(std::max<long>(0, cfg.sampleRateHz))
        * static_cast<quint64>(channels) * 4ULL;
    const quint64 actualSamplesPerChannel = fileBytes / (static_cast<quint64>(channels) * 4ULL);
    const long double expectedSamplesLd = static_cast<long double>(std::max<long>(0, cfg.sampleRateHz))
        * static_cast<long double>(std::max<qint64>(0, rxElapsedMs)) / 1000.0L;
    const quint64 expectedSamplesPerChannel = expectedSamplesLd > 0.0L
        ? static_cast<quint64>(std::llround(expectedSamplesLd)) : 0;
    const quint64 estimatedMissingSamples = expectedSamplesPerChannel > actualSamplesPerChannel
        ? expectedSamplesPerChannel - actualSamplesPerChannel : 0;
    const double coverage = expectedSamplesPerChannel > 0
        ? 100.0 * static_cast<double>(actualSamplesPerChannel)
          / static_cast<double>(expectedSamplesPerChannel)
        : 0.0;

    QJsonArray channelsJson;
    for (int i = 0; i < 8; ++i)
        if (cfg.enabled[i]) channelsJson.append(channelName(static_cast<LwChannel>(i)));

    QJsonObject o;
    o.insert(QStringLiteral("format_version"), QStringLiteral("LW_CAPTURE_META_1"));
    o.insert(QStringLiteral("app_version"), QStringLiteral("1.1.1"));
    o.insert(QStringLiteral("created_at"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("run_id"), static_cast<double>(cfg.captureRunId));
    o.insert(QStringLiteral("model"), modelName(cfg.model));
    o.insert(QStringLiteral("uri"), QString::fromLatin1(cfg.uri));
    o.insert(QStringLiteral("center_frequency_hz"), cfg.centerFrequencyHz);
    o.insert(QStringLiteral("sample_rate_hz"), static_cast<double>(cfg.sampleRateHz));
    o.insert(QStringLiteral("enabled_channels"), channelsJson);
    o.insert(QStringLiteral("enabled_channel_count"), channels);
    o.insert(QStringLiteral("frame_bytes"), static_cast<double>(cfg.frameBytes));
    o.insert(QStringLiteral("calibration"), cfg.performCalibration);
    o.insert(QStringLiteral("developer_mode"), cfg.developerMode);
    o.insert(QStringLiteral("iq_file_path"), QFileInfo(cfg.iqFilePath).absoluteFilePath());
    o.insert(QStringLiteral("storage_mode"), storageModeText(cfg.iqStorageMode));
    o.insert(QStringLiteral("iq_buffer_bytes"), static_cast<double>(cfg.iqBufferBytes));
    o.insert(QStringLiteral("rx_elapsed_ms"), static_cast<double>(rxElapsedMs));
    o.insert(QStringLiteral("expected_bytes_per_second"), static_cast<double>(expectedBps));
    o.insert(QStringLiteral("recv_bytes"), static_cast<double>(recvBytes));
    o.insert(QStringLiteral("recv_calls"), static_cast<double>(recvCalls));
    o.insert(QStringLiteral("partial_blocks"), static_cast<double>(partialBlocks));
    o.insert(QStringLiteral("max_receive_gap_ms"), static_cast<double>(maxReceiveGapMs));
    o.insert(QStringLiteral("writer_enqueued_bytes"), static_cast<double>(writer.totalEnqueued));
    o.insert(QStringLiteral("writer_written_bytes"), static_cast<double>(writer.totalWritten));
    o.insert(QStringLiteral("file_bytes"), static_cast<double>(fileBytes));
    o.insert(QStringLiteral("expected_samples_per_channel"), static_cast<double>(expectedSamplesPerChannel));
    o.insert(QStringLiteral("file_samples_per_channel"), static_cast<double>(actualSamplesPerChannel));
    o.insert(QStringLiteral("estimated_missing_samples_per_channel"), static_cast<double>(estimatedMissingSamples));
    o.insert(QStringLiteral("estimated_time_coverage_percent"), coverage);
    o.insert(QStringLiteral("valid_iq_seen"), validIq);
    o.insert(QStringLiteral("storage_stopped_capture"), storageStopped);
    o.insert(QStringLiteral("abnormal_termination"), abnormal);
    o.insert(QStringLiteral("termination_reason"), terminationReason);

    QDir().mkpath(QFileInfo(cfg.captureMetadataPath).absolutePath());
    QSaveFile file(cfg.captureMetadataPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    file.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return file.commit();
}
} // namespace

RxWorker::RxWorker(RxConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config))
{
}

RxWorker::~RxWorker()
{
    std::lock_guard<std::mutex> lock(stopThreadMutex_);
    if (stopThread_.joinable()) stopThread_.join();
}

void RxWorker::registerContext(lw39x0_context* ctx, bool receiveStarted) noexcept
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    activeContext_ = ctx;
    activeReceiveStarted_ = receiveStarted;
}

void RxWorker::interruptReceive() noexcept
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    if (!activeContext_ || !activeReceiveStarted_) {
        emit diagnosticMessage(QStringLiteral("[%1][STOP] interrupt skipped: context/stream not active")
            .arg(runTag(config_)));
        return;
    }
    if (stopIssued_.exchange(true, std::memory_order_acq_rel)) {
        emit diagnosticMessage(QStringLiteral("[%1][STOP] interrupt skipped: stop already issued")
            .arg(runTag(config_)));
        return;
    }

    // recv() may be blocked inside the SDK. Issuing the driver's stop command
    // here makes Stop an actual interrupt instead of waiting for another DMA
    // block before cleanup can begin. Keep the context mutex held so destroy()
    // can never race this SDK call.
    const auto begin = std::chrono::steady_clock::now();
    emit diagnosticMessage(QStringLiteral("[%1][STOP] lw39x0_issue_recv_stop begin")
        .arg(runTag(config_)));
    const int ret = lw39x0_issue_recv_stop(activeContext_);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    emit diagnosticMessage(QStringLiteral("[%1][STOP] lw39x0_issue_recv_stop ret=%2 elapsed=%3 ms")
        .arg(runTag(config_)).arg(ret).arg(elapsed));
}

void RxWorker::requestStop() noexcept
{
    const bool alreadyRequested = stopRequested_.exchange(true, std::memory_order_acq_rel);
    if (alreadyRequested) return;

    emit diagnosticMessage(QStringLiteral("[%1][STOP] stopRequested=1")
        .arg(runTag(config_)));
    try {
        std::lock_guard<std::mutex> lock(stopThreadMutex_);
        if (!stopThread_.joinable()) {
            stopThread_ = std::thread([this] { interruptReceive(); });
        }
    } catch (...) {
        // Thread creation failure is exceptionally rare. Fall back to a direct
        // stop so the device is not left streaming.
        interruptReceive();
    }
}

void RxWorker::configureDevice(lw39x0_context* ctx)
{
    lw39x0_set_rx_sample_rate(ctx, config_.sampleRateHz);

    const long long f = static_cast<long long>(config_.centerFrequencyHz);
    {
        lw39x0_rf_zone_A_freq z{};
        z.freq_a = f;
        lw39x0_set_A_zone_freq(ctx, z);
    }
    if (config_.model != LwModel::LW3920) {
        lw39x0_rf_zone_B_freq z{};
        z.freq_b = f;
        lw39x0_set_B_zone_freq(ctx, z);
    }
    if (config_.model == LwModel::LW3980) {
        lw39x0_rf_zone_C_freq zc{};
        zc.freq_c = f;
        lw39x0_set_C_zone_freq(ctx, zc);
        lw39x0_rf_zone_D_freq zd{};
        zd.freq_d = f;
        lw39x0_set_D_zone_freq(ctx, zd);
    }

    const lw39x0_rx_channels channels = toSdkChannels(config_);
    lw39x0_enable_rx_channels(ctx, channels);
    openAndGainSelected(ctx, config_);

    if (!config_.performCalibration) return;

    QStringList zones;
    if (config_.enabled[0] || config_.enabled[1]) zones << QStringLiteral("A");
    if (config_.model != LwModel::LW3920 && (config_.enabled[2] || config_.enabled[3]))
        zones << QStringLiteral("B");
    if (config_.model == LwModel::LW3980 && (config_.enabled[4] || config_.enabled[5]))
        zones << QStringLiteral("C");
    if (config_.model == LwModel::LW3980 && (config_.enabled[6] || config_.enabled[7]))
        zones << QStringLiteral("D");

    emit logMessage(tr("[设备] RF 校准开始：Zone %1").arg(zones.join(QStringLiteral(" + "))));
    const auto calibrateZone = [this](const QString& zone, auto fn) {
        if (stopRequested_.load(std::memory_order_acquire)) return;
        const auto begin = std::chrono::steady_clock::now();
        fn();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();
        emit diagnosticMessage(tr("[%1][CAL] zone=%2 completed elapsed=%3 ms")
            .arg(runTag(config_)).arg(zone).arg(ms));
    };

    if (config_.enabled[0] || config_.enabled[1])
        calibrateZone(QStringLiteral("A"), [&] { lw39x0_set_zone_A_calibrate(ctx, true); });
    if (config_.model != LwModel::LW3920 && (config_.enabled[2] || config_.enabled[3]))
        calibrateZone(QStringLiteral("B"), [&] { lw39x0_set_zone_B_calibrate(ctx, true); });
    if (config_.model == LwModel::LW3980 && (config_.enabled[4] || config_.enabled[5]))
        calibrateZone(QStringLiteral("C"), [&] { lw39x0_set_zone_C_calibrate(ctx, true); });
    if (config_.model == LwModel::LW3980 && (config_.enabled[6] || config_.enabled[7]))
        calibrateZone(QStringLiteral("D"), [&] { lw39x0_set_zone_D_calibrate(ctx, true); });

    if (stopRequested_.load(std::memory_order_acquire))
        emit logMessage(tr("[设备] RF 校准调用结束；已收到停止请求"));
    else
        emit logMessage(tr("[设备] RF 校准调用完成"));
}

void RxWorker::cleanup(lw39x0_context*& ctx, bool& receiveStarted) noexcept
{
    if (!ctx) return;

    const auto cleanupBegin = std::chrono::steady_clock::now();
    emit diagnosticMessage(QStringLiteral("[%1][CLEANUP] begin receiveStarted=%2 stopIssued=%3")
        .arg(runTag(config_))
        .arg(receiveStarted ? 1 : 0)
        .arg(stopIssued_.load(std::memory_order_acquire) ? 1 : 0));

    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (activeContext_ == ctx) {
            activeReceiveStarted_ = false;
            activeContext_ = nullptr;
        }
    }

    if (receiveStarted) {
        if (!stopIssued_.exchange(true, std::memory_order_acq_rel)) {
            const auto stopBegin = std::chrono::steady_clock::now();
            const int ret = lw39x0_issue_recv_stop(ctx);
            const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stopBegin).count();
            emit diagnosticMessage(QStringLiteral("[%1][CLEANUP] fallback recv_stop ret=%2 elapsed=%3 ms")
                .arg(runTag(config_)).arg(ret).arg(stopMs));
        }
        receiveStarted = false;
    }

    lw39x0_rx_channels none{};
    lw39x0_enable_rx_channels(ctx, none);
    const auto destroyBegin = std::chrono::steady_clock::now();
    lw39x0_destroy(ctx);
    const auto destroyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - destroyBegin).count();
    ctx = nullptr;

    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cleanupBegin).count();
    emit diagnosticMessage(QStringLiteral("[%1][CLEANUP] destroy=%2 ms total=%3 ms")
        .arg(runTag(config_)).arg(destroyMs).arg(totalMs));
}

void RxWorker::run()
{
    stopIssued_.store(false, std::memory_order_release);
    registerContext(nullptr, false);

    const QString tag = runTag(config_);
    lw39x0_context* ctx = nullptr;
    bool receiveStarted = false;
    AsyncIqWriter writer;
    bool writerStarted = false;

    emit logMessage(tr("[采集] 参数：%1 | %2 | %3 MHz | %4 MS/s | %5 dB")
                    .arg(modelName(config_.model))
                    .arg(channelMatrixText(config_))
                    .arg(config_.centerFrequencyHz / 1.0e6, 0, 'f', 3)
                    .arg(config_.sampleRateHz / 1.0e6, 0, 'f', 3)
                    .arg(config_.gainDb, 0, 'f', 1));
    emit diagnosticMessage(tr("[%1][CONFIG] model=%2 uri=%3 channels=%4 display=%5 fc=%6 Hz fs=%7 Hz gain=%8 dB calibration=%9 FFT=%10 frameBytes=%11 duration=%12 s save=%13 storageMode=%14")
        .arg(tag)
        .arg(modelName(config_.model))
        .arg(QString::fromLatin1(config_.uri))
        .arg(channelMatrixText(config_))
        .arg(channelName(config_.displayChannel))
        .arg(config_.centerFrequencyHz, 0, 'f', 0)
        .arg(config_.sampleRateHz)
        .arg(config_.gainDb, 0, 'f', 1)
        .arg(config_.performCalibration ? 1 : 0)
        .arg(config_.fftSize)
        .arg(static_cast<qulonglong>(config_.frameBytes))
        .arg(config_.captureDurationSeconds, 0, 'f', 3)
        .arg(config_.saveIq ? 1 : 0)
        .arg(storageModeText(config_.iqStorageMode)));

    void* rawPtr = nullptr;
    if (posix_memalign(&rawPtr, kAlignment, config_.frameBytes) != 0) {
        emit diagnosticMessage(tr("[%1][INIT] posix_memalign failed alignment=%2 bytes=%3")
            .arg(tag).arg(static_cast<qulonglong>(kAlignment))
            .arg(static_cast<qulonglong>(config_.frameBytes)));
        emit errorOccurred(tr("RX 缓冲区分配失败"));
        emit stopped();
        return;
    }
    std::unique_ptr<char, FreeDeleter> buffer(static_cast<char*>(rawPtr));
    std::memset(buffer.get(), 0, config_.frameBytes);
    emit diagnosticMessage(tr("[%1][INIT] RX buffer allocated ptr=%2 alignment=%3 bytes=%4")
        .arg(tag)
        .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(buffer.get())), 0, 16)
        .arg(static_cast<qulonglong>(kAlignment))
        .arg(static_cast<qulonglong>(config_.frameBytes)));

    const auto makeBegin = std::chrono::steady_clock::now();
    ctx = lw39x0_make(config_.uri);
    const auto makeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - makeBegin).count();
    if (!ctx) {
        emit diagnosticMessage(tr("[%1][INIT] lw39x0_make failed elapsed=%2 ms")
            .arg(tag).arg(makeMs));
        emit errorOccurred(tr("设备打开失败：pcie_0"));
        emit stopped();
        return;
    }
    registerContext(ctx, false);
    emit diagnosticMessage(tr("[%1][INIT] lw39x0_make OK elapsed=%2 ms")
        .arg(tag).arg(makeMs));

    (void)lw39x0_show_context_info(ctx);
    lw39x0_show_channels_info(ctx);
    const auto configBegin = std::chrono::steady_clock::now();
    configureDevice(ctx);
    const auto configMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - configBegin).count();
    emit diagnosticMessage(tr("[%1][INIT] configureDevice completed elapsed=%2 ms")
        .arg(tag).arg(configMs));

    const double expectedMibps = static_cast<double>(config_.sampleRateHz)
        * static_cast<double>(enabledChannelCount(config_)) * 4.0 / 1048576.0;

    if (config_.saveIq) {
        const QFileInfo fileInfo(config_.iqFilePath);
        if (fileInfo.exists()) {
            emit logMessage(tr("[警告] IQ 文件已存在，将覆盖：%1").arg(config_.iqFilePath));
            emit diagnosticMessage(tr("[%1][STORAGE] overwrite existing file size=%2 path=%3")
                .arg(tag)
                .arg(bytesText(static_cast<quint64>(std::max<qint64>(0, fileInfo.size()))))
                .arg(config_.iqFilePath));
        }
        const QStorageInfo storage(fileInfo.absolutePath());
        if (storage.isValid() && storage.isReady()) {
            const quint64 availableBytes = static_cast<quint64>(std::max<qint64>(0, storage.bytesAvailable()));
            const double availableSeconds = expectedMibps > 0.0
                ? availableBytes / (expectedMibps * 1048576.0) : 0.0;
            emit logMessage(tr("[存储] 可用空间：%1 | 按当前数据率理论约 %2 min")
                .arg(bytesText(availableBytes))
                .arg(availableSeconds / 60.0, 0, 'f', 1));
            emit diagnosticMessage(tr("[%1][STORAGE] fs=%2 device=%3 available=%4 total=%5 path=%6")
                .arg(tag)
                .arg(QString::fromLocal8Bit(storage.fileSystemType()))
                .arg(QString::fromLocal8Bit(storage.device()))
                .arg(bytesText(static_cast<quint64>(std::max<qint64>(0, storage.bytesAvailable()))))
                .arg(bytesText(static_cast<quint64>(std::max<qint64>(0, storage.bytesTotal()))))
                .arg(config_.iqFilePath));
        } else {
            emit diagnosticMessage(tr("[%1][STORAGE] QStorageInfo unavailable for %2")
                .arg(tag).arg(config_.iqFilePath));
        }

        QString writerError;
        if (!writer.start(config_.iqFilePath, config_.frameBytes,
                          config_.iqBufferBytes, config_.iqStorageMode, writerError)) {
            emit diagnosticMessage(tr("[%1][STORAGE] writer start failed: %2")
                .arg(tag).arg(writerError));
            emit errorOccurred(writerError);
            cleanup(ctx, receiveStarted);
            emit deviceReleased();
            emit stopped();
            return;
        }
        writerStarted = true;
        const double writerBufferSeconds = expectedMibps > 0.0
            ? static_cast<double>(writer.capacityBytes()) / (expectedMibps * 1048576.0)
            : 0.0;
        emit logMessage(tr("[存储] IQ 文件：%1").arg(config_.iqFilePath));
        emit logMessage(tr("[存储] 预计原始数据率：%1 MiB/s（%2 GB/s）")
                        .arg(expectedMibps, 0, 'f', 1)
                        .arg(expectedMibps * 1048576.0 / 1.0e9, 0, 'f', 2));
        emit logMessage(tr("[存储] IQ 写入缓存：%1%2 | 当前数据率下约 %3 s")
                        .arg(bytesText(static_cast<quint64>(writer.capacityBytes())))
                        .arg(config_.iqBufferAuto ? tr("（自动）") : tr("（手动）"))
                        .arg(writerBufferSeconds, 0, 'f', 2));
        if (writerBufferSeconds < 0.25) {
            emit logMessage(tr("[警告] 当前 IQ 缓存仅约 %1 ms；对调度/存储抖动的容忍度较低")
                .arg(writerBufferSeconds * 1000.0, 0, 'f', 0));
        } else if (config_.iqBufferAuto && writerBufferSeconds < 0.50) {
            emit logMessage(tr("[警告] 自动缓存受内存/上限约束，当前仅约 %1 s")
                .arg(writerBufferSeconds, 0, 'f', 2));
        }
        emit logMessage(tr("[存储] 写盘模式：%1").arg(writer.backendName()));
        if (config_.iqStorageMode == IqStorageMode::Auto)
            emit logMessage(tr("[存储] Auto：优先采用 Buffered 顺序写入；保留连续性监控与缓存保护"));
        const NvmeThermalSnapshot startThermal = readNvmeThermal();
        if (startThermal.valid) {
            emit logMessage(tr("[存储] NVMe 温度：Composite %1 °C | 最高传感器 %2 °C")
                .arg(startThermal.compositeC, 0, 'f', 1)
                .arg(startThermal.maxSensorC, 0, 'f', 1));
            emit diagnosticMessage(tr("[%1][NVME] start temp=%2 source=%3")
                .arg(tag).arg(nvmeThermalText(startThermal)).arg(startThermal.source));
        } else {
            emit diagnosticMessage(tr("[%1][NVME] temperature unavailable via /sys/class/hwmon").arg(tag));
        }
        if (expectedMibps >= 1000.0) {
            emit logMessage(tr("[存储] 高速保存要求：目标磁盘/文件系统需持续写入不低于 %1 MiB/s；PCIe RX 正常不等同于存储可持续该吞吐")
                            .arg(expectedMibps, 0, 'f', 1));
        }
        const ProcIoSnapshot procIoAtStart = readProcIo();
        const MemInfoSnapshot memAtStart = readMemInfo();
        emit diagnosticMessage(tr("[%1][STORAGE] writer started requestedMode=%2 backend=%3 direct=%4 directOpenErrno=%5 capacity=%6 bufferMode=%7 bufferWindow=%8 ms allocPrefault=%9 ms block=%10 B directBatchMax=%11 directAlign=%12 expected=%13 MiB/s procWriteBytes=%14 dirty=%15 writeback=%16 memAvail=%17")
            .arg(tag)
            .arg(storageModeText(config_.iqStorageMode))
            .arg(writer.backendName())
            .arg(writer.directIo() ? 1 : 0)
            .arg(writer.directOpenErrno())
            .arg(bytesText(static_cast<quint64>(writer.capacityBytes())))
            .arg(config_.iqBufferAuto ? QStringLiteral("AUTO") : QStringLiteral("MANUAL"))
            .arg(writerBufferSeconds * 1000.0, 0, 'f', 1)
            .arg(writer.allocationUsec() / 1000.0, 0, 'f', 1)
            .arg(static_cast<qulonglong>(config_.frameBytes))
            .arg(bytesText(kWriterBatchMaxBytes))
            .arg(bytesText(kWriterDirectAlignment))
            .arg(expectedMibps, 0, 'f', 1)
            .arg(procIoAtStart.valid ? bytesText(procIoAtStart.writeBytes) : QStringLiteral("N/A"))
            .arg(memAtStart.valid ? bytesText(memAtStart.dirtyBytes) : QStringLiteral("N/A"))
            .arg(memAtStart.valid ? bytesText(memAtStart.writebackBytes) : QStringLiteral("N/A"))
            .arg(memAtStart.valid ? bytesText(memAtStart.availableBytes) : QStringLiteral("N/A")));
    }

    LatestPreviewProcessor preview(config_, [this](const DisplayFrame& frame) {
        emit displayFrameReady(frame);
    });
    if (!preview.start()) {
        emit diagnosticMessage(tr("[%1][PREVIEW] thread start failed").arg(tag));
        emit errorOccurred(tr("显示处理线程启动失败"));
        cleanup(ctx, receiveStarted);
        emit deviceReleased();
        writer.finish();
        emit stopped();
        return;
    }
    emit diagnosticMessage(tr("[%1][PREVIEW] thread started interval=%2 ms")
        .arg(tag).arg(kPreviewIntervalMs));

    if (stopRequested_.load(std::memory_order_acquire)) {
        emit diagnosticMessage(tr("[%1][STOP] stop observed before recv_start").arg(tag));
        preview.finish();
        cleanup(ctx, receiveStarted);
        emit deviceReleased();
        writer.finish();
        emit logMessage(tr("[采集] 已停止 | 原因：启动阶段收到停止请求"));
        emit stopped();
        return;
    }

    const auto probePattern = makeProbePattern();
    std::memcpy(buffer.get(), probePattern.data(), probePattern.size());

    const auto startBegin = std::chrono::steady_clock::now();
    const int startRet = lw39x0_issue_recv_start(ctx);
    const auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startBegin).count();
    emit diagnosticMessage(tr("[%1][RX] lw39x0_issue_recv_start ret=%2 elapsed=%3 ms")
        .arg(tag).arg(startRet).arg(startMs));
    if (startRet != 0) {
        emit errorOccurred(tr("接收启动失败：%1").arg(startRet));
        preview.finish();
        cleanup(ctx, receiveStarted);
        emit deviceReleased();
        writer.finish();
        emit stopped();
        return;
    }

    receiveStarted = true;
    registerContext(ctx, true);
    if (stopRequested_.load(std::memory_order_acquire)) {
        interruptReceive();
        preview.finish();
        cleanup(ctx, receiveStarted);
        emit deviceReleased();
        writer.finish();
        emit logMessage(tr("[采集] 已停止 | 原因：启动完成前收到停止请求"));
        emit stopped();
        return;
    }

    emit started();
    emit logMessage(tr("[采集] 已启动"));

    QElapsedTimer timer;
    timer.start();
    qint64 lastPreview = -kPreviewIntervalMs;
    qint64 lastStats = 0;
    const qint64 iqCheckIntervalMs = config_.developerMode ? kIqCheckIntervalMs : 5000;
    qint64 lastIqCheck = -iqCheckIntervalMs;
    qint64 lastDeveloperStatus = -kDeveloperStatusIntervalMs;
    qint64 lastDeveloperSampleAt = 0;
    qint64 lastStorageStatus = 0;
    qint64 lastReceiveAt = -1;
    qint64 maxReceiveGapMs = 0;
    quint64 totalBytes = 0;
    quint64 bytesAtLastStats = 0;
    quint64 writerBytesAtLastDeveloperSample = 0;
    quint64 writerBytesAtLastStorageSample = 0;
    ProcIoSnapshot procIoAtLastStorageSample = config_.developerMode ? readProcIo() : ProcIoSnapshot{};
    quint64 recvCalls = 0;
    quint64 partialBlocks = 0;
    WriterSnapshot metadataWriter;
    quint64 metadataFileBytes = 0;

    const double bytesPerSecond = expectedMibps * 1048576.0;
    const double expectedBlockMs = bytesPerSecond > 0.0
        ? static_cast<double>(config_.frameBytes) / bytesPerSecond * 1000.0 : 0.0;
    const qint64 severeGapThresholdMs = std::max<qint64>(1000,
        static_cast<qint64>(std::ceil(expectedBlockMs * 100.0)));

    bool firstReceive = true;
    bool everValidIq = false;
    bool zeroWarningIssued = false;
    int consecutiveZeroChecks = 0;
    bool storageStoppedCapture = false;
    bool storage75Warned = false;
    bool storage90Warned = false;
    bool storageThroughputWarned = false;
    bool storageHealthyLogged = false;
    bool nvmeThermalWarned = false;
    int lowWriterConsecutive = 0;
    bool abnormalTermination = false;
    bool timedCaptureCompleted = false;
    const qint64 captureDurationMs = config_.captureDurationSeconds > 0.0
        ? std::max<qint64>(1, static_cast<qint64>(std::llround(config_.captureDurationSeconds * 1000.0)))
        : 0;
    QString terminationReason = tr("用户停止");

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const long n = lw39x0_recv(
            ctx,
            reinterpret_cast<volatile char*>(buffer.get()),
            static_cast<unsigned long>(config_.frameBytes));
        ++recvCalls;
        const qint64 now = timer.elapsed();

        if (lastReceiveAt >= 0) {
            const qint64 gap = now - lastReceiveAt;
            maxReceiveGapMs = std::max(maxReceiveGapMs, gap);
            if (gap >= severeGapThresholdMs
                && !stopRequested_.load(std::memory_order_acquire)) {
                emit logMessage(tr("[警告] RX 数据间隔异常：%1 ms").arg(gap));
                emit diagnosticMessage(tr("[%1][RX] long recv gap=%2 ms threshold=%3 ms call=%4")
                    .arg(tag).arg(gap).arg(severeGapThresholdMs).arg(recvCalls));
            }
        }
        lastReceiveAt = now;

        if (n <= 0) {
            if (stopRequested_.load(std::memory_order_acquire)) {
                terminationReason = tr("用户停止");
                emit diagnosticMessage(tr("[%1][RX] recv returned %2 after stop request")
                    .arg(tag).arg(n));
                break;
            }
            abnormalTermination = true;
            terminationReason = n < 0
                ? tr("lw39x0_recv 返回错误 %1").arg(n)
                : tr("lw39x0_recv 返回长度 0");
            emit diagnosticMessage(tr("[%1][RX] abnormal recv return=%2 call=%3 elapsed=%4 ms")
                .arg(tag).arg(n).arg(recvCalls).arg(now));
            emit errorOccurred(n < 0
                ? tr("接收错误：%1").arg(n)
                : tr("接收中断：返回长度为 0"));
            break;
        }

        if (static_cast<std::size_t>(n) > config_.frameBytes) {
            abnormalTermination = true;
            terminationReason = tr("驱动返回长度超过 RX 缓冲区");
            emit diagnosticMessage(tr("[%1][RX] invalid length=%2 > frameBytes=%3")
                .arg(tag).arg(n).arg(static_cast<qulonglong>(config_.frameBytes)));
            emit errorOccurred(tr("接收长度异常：%1 B > 缓冲区 %2 B")
                .arg(n).arg(static_cast<qulonglong>(config_.frameBytes)));
            break;
        }

        if (static_cast<std::size_t>(n) != config_.frameBytes) {
            ++partialBlocks;
            if (partialBlocks == 1 || partialBlocks % 100 == 0) {
                emit diagnosticMessage(tr("[%1][RX] partial block=%2 B expected=%3 B count=%4")
                    .arg(tag).arg(n)
                    .arg(static_cast<qulonglong>(config_.frameBytes))
                    .arg(partialBlocks));
            }
        }

        totalBytes += static_cast<quint64>(n);

        if (firstReceive) {
            firstReceive = false;
            double unchanged = 0.0;
            const bool dmaBufferUpdated = probeWasOverwritten(buffer.get(), probePattern, &unchanged);
            if (dmaBufferUpdated) {
                emit logMessage(tr("[数据] DMA 接收正常：%1 B/block")
                                .arg(static_cast<qulonglong>(n)));
                emit diagnosticMessage(tr("[%1][RX] first DMA overwrite OK unchangedProbe=%2%")
                    .arg(tag).arg(unchanged, 0, 'f', 1));
            } else {
                emit logMessage(tr("[错误] DMA 缓冲区未更新：返回 %1 B")
                                .arg(static_cast<qulonglong>(n)));
                emit diagnosticMessage(tr("[%1][RX] first DMA overwrite FAILED unchangedProbe=%2%")
                    .arg(tag).arg(unchanged, 0, 'f', 1));
                std::memset(buffer.get(), 0, config_.frameBytes);
                lastIqCheck = now;
                continue;
            }
        }

        // Storage gets the raw block before any preview work. The writer queue
        // preserves block order. If the disk cannot keep up, stop rather than
        // silently discard an interior block and continue producing a corrupt
        // time sequence.
        if (writerStarted) {
            QString reason;
            if (!writer.enqueue(buffer.get(), static_cast<std::size_t>(n), reason)) {
                storageStoppedCapture = true;
                abnormalTermination = true;
                const WriterSnapshot ws = writer.snapshot();
                const double elapsedSec = std::max(0.001, now / 1000.0);
                const double rxAverageMibps = (totalBytes / 1048576.0) / elapsedSec;
                const double writerAverageMibps = (ws.totalWritten / 1048576.0) / elapsedSec;
                const bool queueFull = ws.slotCount > 0 && ws.queuedSlots >= ws.slotCount;
                if (queueFull) {
                    terminationReason = tr("IQ 保存吞吐不足：缓存已满（RX %1 MiB/s，写入约 %2 MiB/s）")
                        .arg(rxAverageMibps, 0, 'f', 1)
                        .arg(writerAverageMibps, 0, 'f', 1);
                    emit logMessage(tr("[错误] IQ 保存无法持续：RX 平均 %1 MiB/s，文件写入平均约 %2 MiB/s，缓存 %3 已满")
                        .arg(rxAverageMibps, 0, 'f', 1)
                        .arg(writerAverageMibps, 0, 'f', 1)
                        .arg(bytesText(static_cast<quint64>(writer.capacityBytes()))));
                    emit logMessage(tr("[存储] 写盘模式：%1 | 缓存模式：%2")
                        .arg(writer.backendName())
                        .arg(config_.iqBufferAuto ? tr("自动") : tr("手动")));
                    const NvmeThermalSnapshot failThermal = readNvmeThermal();
                    if (failThermal.valid) {
                        emit logMessage(tr("[存储] 自动停止时 NVMe 温度：Composite %1 °C | 最高传感器 %2 °C")
                            .arg(failThermal.compositeC, 0, 'f', 1)
                            .arg(failThermal.maxSensorC, 0, 'f', 1));
                    }
                    emit logMessage(tr("[存储] 本次自动停止由保存链路触发；此前未检测到 lw39x0_recv 接收错误。若写速持续低于 RX，增大缓存只能延长容忍时间；请检查 SSD 高速缓存/温度/后台负载，或降低通道与采样率"));
                } else {
                    terminationReason = reason;
                }
                const ProcIoSnapshot pio = config_.developerMode ? readProcIo() : ProcIoSnapshot{};
                const MemInfoSnapshot mem = config_.developerMode ? readMemInfo() : MemInfoSnapshot{};
                const double avgWriteUsec = ws.writeCalls
                    ? static_cast<double>(ws.totalWriteUsec) / ws.writeCalls : 0.0;
                emit diagnosticMessage(tr("[%1][STORAGE] enqueue failed: %2 | queued=%3/%4 (%5%) enq=%6 written=%7 rxAvg=%8 MiB/s writerAvg=%9 MiB/s writeCalls=%10 avg/maxWrite=%11/%12 us maxBatch=%13 slots maxWrite=%14 slowWrites=%15 flush=%16 maxFlush=%17 us procWchar=%18 procWriteBytes=%19 syscw=%20 dirty=%21 writeback=%22 memAvail=%23")
                    .arg(tag).arg(reason)
                    .arg(static_cast<qulonglong>(ws.queuedSlots))
                    .arg(static_cast<qulonglong>(ws.slotCount))
                    .arg(ws.fillPercent(), 0, 'f', 1)
                    .arg(bytesText(ws.totalEnqueued))
                    .arg(bytesText(ws.totalWritten))
                    .arg(rxAverageMibps, 0, 'f', 1)
                    .arg(writerAverageMibps, 0, 'f', 1)
                    .arg(ws.writeCalls)
                    .arg(avgWriteUsec, 0, 'f', 1)
                    .arg(ws.maxWriteUsec)
                    .arg(ws.maxBatchSlots)
                    .arg(bytesText(ws.maxWriteBytes))
                    .arg(ws.slowWriteCalls)
                    .arg(ws.flushCalls)
                    .arg(ws.maxFlushUsec)
                    .arg(pio.valid ? bytesText(pio.wcharBytes) : QStringLiteral("N/A"))
                    .arg(pio.valid ? bytesText(pio.writeBytes) : QStringLiteral("N/A"))
                    .arg(pio.valid ? QString::number(pio.writeSyscalls) : QStringLiteral("N/A"))
                    .arg(mem.valid ? bytesText(mem.dirtyBytes) : QStringLiteral("N/A"))
                    .arg(mem.valid ? bytesText(mem.writebackBytes) : QStringLiteral("N/A"))
                    .arg(mem.valid ? bytesText(mem.availableBytes) : QStringLiteral("N/A")));
                emit errorOccurred(terminationReason);
                break;
            }
        }

        if (writerStarted) {
            const qint64 storageInterval = now < kStorageStartupWindowMs
                ? kStorageStartupStatusIntervalMs : kStorageSteadyStatusIntervalMs;
            if (now - lastStorageStatus >= storageInterval) {
                const WriterSnapshot ws = writer.snapshot();
                const qint64 dt = std::max<qint64>(1, now - lastStorageStatus);
                const quint64 writtenDelta = ws.totalWritten >= writerBytesAtLastStorageSample
                    ? ws.totalWritten - writerBytesAtLastStorageSample : 0;
                const double writerMibps = (writtenDelta / 1048576.0) / (dt / 1000.0);
                const double rxAverageMibps = now > 0
                    ? (totalBytes / 1048576.0) / (now / 1000.0) : 0.0;
                const ProcIoSnapshot pio = config_.developerMode ? readProcIo() : ProcIoSnapshot{};
                const MemInfoSnapshot mem = config_.developerMode ? readMemInfo() : MemInfoSnapshot{};
                const quint64 procWriteDelta = pio.valid && procIoAtLastStorageSample.valid
                    && pio.writeBytes >= procIoAtLastStorageSample.writeBytes
                    ? pio.writeBytes - procIoAtLastStorageSample.writeBytes : 0;
                const double procWriteMibps = (procWriteDelta / 1048576.0) / (dt / 1000.0);
                const double avgWriteUsec = ws.writeCalls
                    ? static_cast<double>(ws.totalWriteUsec) / ws.writeCalls : 0.0;
                const NvmeThermalSnapshot thermal = readNvmeThermal();
                if (config_.developerMode)
                    emit diagnosticMessage(tr("[%1][STORAGE_FAST] t=%2 ms queue=%3/%4 (%5%) backlog=%6 RXavg=%7 MiB/s writerDelta=%8 MiB/s procWriteDelta=%9 MiB/s calls=%10 avg/maxWrite=%11/%12 us maxBatch=%13 slots slow=%14 sync/flush=%15 maxSyncFlush=%16 us dirty=%17 writeback=%18 memAvail=%19 backend=%20 nvmeTemp=%21")
                    .arg(tag).arg(now)
                    .arg(static_cast<qulonglong>(ws.queuedSlots))
                    .arg(static_cast<qulonglong>(ws.slotCount))
                    .arg(ws.fillPercent(), 0, 'f', 1)
                    .arg(bytesText(ws.queuedBytesApprox()))
                    .arg(rxAverageMibps, 0, 'f', 1)
                    .arg(writerMibps, 0, 'f', 1)
                    .arg(procWriteMibps, 0, 'f', 1)
                    .arg(ws.writeCalls)
                    .arg(avgWriteUsec, 0, 'f', 1)
                    .arg(ws.maxWriteUsec)
                    .arg(ws.maxBatchSlots)
                    .arg(ws.slowWriteCalls)
                    .arg(ws.flushCalls)
                    .arg(ws.maxFlushUsec)
                    .arg(mem.valid ? bytesText(mem.dirtyBytes) : QStringLiteral("N/A"))
                    .arg(mem.valid ? bytesText(mem.writebackBytes) : QStringLiteral("N/A"))
                    .arg(mem.valid ? bytesText(mem.availableBytes) : QStringLiteral("N/A"))
                    .arg(writer.backendName())
                    .arg(nvmeThermalText(thermal)));

                if (thermal.valid && !nvmeThermalWarned
                    && (thermal.compositeC >= 70.0 || thermal.maxSensorC >= 80.0)) {
                    nvmeThermalWarned = true;
                    emit logMessage(tr("[警告] NVMe 温度较高：Composite %1 °C | 最高传感器 %2 °C；持续高速写入可能触发降速")
                        .arg(thermal.compositeC, 0, 'f', 1)
                        .arg(thermal.maxSensorC, 0, 'f', 1));
                }

                const bool lowWriter = now >= 2000 && writerMibps > 0.0
                    && writerMibps < expectedMibps * 0.90
                    && ws.fillPercent() >= 10.0;
                lowWriterConsecutive = lowWriter ? lowWriterConsecutive + 1 : 0;
                if (lowWriterConsecutive >= 2 && !storageThroughputWarned) {
                    storageThroughputWarned = true;
                    emit logMessage(tr("[警告] 存储持续写入低于 RX：近端写入约 %1 MiB/s < 输入 %2 MiB/s，缓存已使用 %3%；可能为 SSD 高速缓存耗尽、温度降速或其他存储负载")
                        .arg(writerMibps, 0, 'f', 1)
                        .arg(expectedMibps, 0, 'f', 1)
                        .arg(ws.fillPercent(), 0, 'f', 0));
                }
                if (now >= 3000 && lowWriterConsecutive >= 2
                    && ws.fillPercent() >= kStorageProtectiveStopFillPercent) {
                    storageStoppedCapture = true;
                    abnormalTermination = true;
                    terminationReason = tr("IQ 保存吞吐不足：缓存达到保护阈值 %1%（写入约 %2 MiB/s，RX %3 MiB/s）")
                        .arg(kStorageProtectiveStopFillPercent, 0, 'f', 0)
                        .arg(writerMibps, 0, 'f', 1)
                        .arg(expectedMibps, 0, 'f', 1);
                    emit logMessage(tr("[错误] 存储持续吞吐不足，IQ 缓存达到 %1% 保护阈值；已提前停止以减少积压并保持连续数据前缀")
                        .arg(kStorageProtectiveStopFillPercent, 0, 'f', 0));
                    emit errorOccurred(terminationReason);
                    break;
                }
                if (!storageHealthyLogged && now >= 5000
                    && writerMibps >= expectedMibps * 0.95
                    && ws.fillPercent() < 25.0) {
                    storageHealthyLogged = true;
                    emit logMessage(tr("[存储] 连续写入状态正常：近端写入约 %1 MiB/s | RX 需求 %2 MiB/s | 缓存 %3%")
                        .arg(writerMibps, 0, 'f', 1)
                        .arg(expectedMibps, 0, 'f', 1)
                        .arg(ws.fillPercent(), 0, 'f', 0));
                }

                if (ws.fillPercent() >= 90.0 && !storage90Warned) {
                    storage90Warned = true;
                    storage75Warned = true;
                    emit logMessage(tr("[警告] IQ 写入缓存已使用 %1%，当前存储吞吐低于 RX 输入速率")
                        .arg(ws.fillPercent(), 0, 'f', 0));
                } else if (ws.fillPercent() >= 75.0 && !storage75Warned) {
                    storage75Warned = true;
                    emit logMessage(tr("[警告] IQ 写入缓存已使用 %1%，正在吸收短时存储抖动")
                        .arg(ws.fillPercent(), 0, 'f', 0));
                }

                writerBytesAtLastStorageSample = ws.totalWritten;
                if (pio.valid) procIoAtLastStorageSample = pio;
                lastStorageStatus = now;
            }
        }

        if (now - lastPreview >= kPreviewIntervalMs) {
            preview.submit(buffer.get(), static_cast<std::size_t>(n));
            lastPreview = now;
        }

        if (now - lastIqCheck >= iqCheckIntervalMs) {
            const RawStats stats = analyzeRaw(buffer.get(), static_cast<std::size_t>(n));
            if (stats.allZero()) {
                ++consecutiveZeroChecks;
                if (consecutiveZeroChecks >= 3 && !zeroWarningIssued) {
                    zeroWarningIssued = true;
                    emit logMessage(tr("[警告] IQ 数据无效：连续采样为全零"));
                    emit diagnosticMessage(tr("[%1][DATA] zero-IQ warning after %2 consecutive checks")
                        .arg(tag).arg(consecutiveZeroChecks));
                }
            } else if (stats.active()) {
                consecutiveZeroChecks = 0;
                if (!everValidIq || zeroWarningIssued) {
                    emit logMessage(tr("[数据] IQ 数据有效：%1").arg(statsText(stats)));
                    const QString channels = perChannelSummary(
                        buffer.get(), static_cast<std::size_t>(n), config_);
                    if (!channels.isEmpty())
                        emit logMessage(tr("[数据] 通道检查：%1").arg(channels));
                }
                everValidIq = true;
                zeroWarningIssued = false;
            } else {
                ++consecutiveZeroChecks;
                if (consecutiveZeroChecks >= 3 && !zeroWarningIssued) {
                    zeroWarningIssued = true;
                    emit logMessage(tr("[警告] IQ 数据变化异常：%1").arg(statsText(stats)));
                    emit diagnosticMessage(tr("[%1][DATA] abnormal-IQ warning: %2")
                        .arg(tag).arg(statsText(stats)));
                }
            }
            lastIqCheck = now;
        }

        if (now - lastStats >= kStatsIntervalMs) {
            const qint64 dt = std::max<qint64>(1, now - lastStats);
            const quint64 delta = totalBytes - bytesAtLastStats;
            const double mibps = (delta / 1048576.0) / (dt / 1000.0);
            emit statisticsUpdated(totalBytes, mibps, now);
            lastStats = now;
            bytesAtLastStats = totalBytes;
        }

        if (config_.developerMode && now - lastDeveloperStatus >= kDeveloperStatusIntervalMs) {
            const PreviewSnapshot ps = preview.snapshot();
            emit diagnosticMessage(tr("[%1][RX] t=%2 ms calls=%3 bytes=%4 last=%5 B partial=%6 maxGap=%7 ms preview submit/process/replace/fail=%8/%9/%10/%11")
                .arg(tag).arg(now).arg(recvCalls)
                .arg(bytesText(totalBytes)).arg(n).arg(partialBlocks).arg(maxReceiveGapMs)
                .arg(ps.submitted).arg(ps.processed).arg(ps.replaced).arg(ps.failures));

            if (writerStarted) {
                const WriterSnapshot ws = writer.snapshot();
                const qint64 devDt = std::max<qint64>(1, now - lastDeveloperSampleAt);
                const quint64 writtenDelta = ws.totalWritten >= writerBytesAtLastDeveloperSample
                    ? ws.totalWritten - writerBytesAtLastDeveloperSample : 0;
                const double writerMibps = (writtenDelta / 1048576.0) / (devDt / 1000.0);
                emit diagnosticMessage(tr("[%1][STORAGE] queue=%2/%3 (%4%) peak=%5 backlog=%6 enqueued=%7 written=%8 writeRate=%9 MiB/s expected=%10 MiB/s failed=%11")
                    .arg(tag)
                    .arg(static_cast<qulonglong>(ws.queuedSlots))
                    .arg(static_cast<qulonglong>(ws.slotCount))
                    .arg(ws.fillPercent(), 0, 'f', 1)
                    .arg(static_cast<qulonglong>(ws.peakQueuedSlots))
                    .arg(bytesText(ws.queuedBytesApprox()))
                    .arg(bytesText(ws.totalEnqueued))
                    .arg(bytesText(ws.totalWritten))
                    .arg(writerMibps, 0, 'f', 1)
                    .arg(expectedMibps, 0, 'f', 1)
                    .arg(ws.failed ? 1 : 0));
                writerBytesAtLastDeveloperSample = ws.totalWritten;

            }
            lastDeveloperSampleAt = now;
            lastDeveloperStatus = now;
        }

        // Timed capture is checked only after the just-received block has been
        // enqueued to storage and processed for normal diagnostics. This makes
        // the automatic end a clean file boundary rather than an interior gap.
        if (captureDurationMs > 0 && now >= captureDurationMs) {
            timedCaptureCompleted = true;
            terminationReason = tr("达到设定采集时间 %1 s")
                .arg(config_.captureDurationSeconds, 0, 'f', 3);
            emit logMessage(tr("[采集] 已达到设定时长 %1 s，正在自动安全停止")
                .arg(config_.captureDurationSeconds, 0, 'f', 3));
            emit diagnosticMessage(tr("[%1][TIMER] duration reached target=%2 ms actual=%3 ms recvCalls=%4 bytes=%5")
                .arg(tag).arg(captureDurationMs).arg(now).arg(recvCalls).arg(bytesText(totalBytes)));
            const qint64 finalDt = std::max<qint64>(1, now - lastStats);
            const quint64 finalDelta = totalBytes - bytesAtLastStats;
            const double finalMibps = (finalDelta / 1048576.0) / (finalDt / 1000.0);
            emit statisticsUpdated(totalBytes, finalMibps, now);
            break;
        }
    }

    const qint64 rxElapsedMs = timer.elapsed();

    if (stopRequested_.load(std::memory_order_acquire) && !abnormalTermination)
        terminationReason = tr("用户停止");

    // Stop preview work first, then release the device before waiting for the
    // storage queue to drain. Disk flushing must never keep the PCIe context
    // alive after RX has been stopped.
    const auto previewStopBegin = std::chrono::steady_clock::now();
    const PreviewSnapshot previewBeforeStop = preview.snapshot();
    preview.finish();
    const auto previewStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - previewStopBegin).count();
    emit diagnosticMessage(tr("[%1][PREVIEW] finish=%2 ms final submit/process/replace/fail=%3/%4/%5/%6")
        .arg(tag).arg(previewStopMs)
        .arg(previewBeforeStop.submitted).arg(previewBeforeStop.processed)
        .arg(previewBeforeStop.replaced).arg(previewBeforeStop.failures));

    if (writerStarted) {
        const WriterSnapshot beforeDrain = writer.snapshot();
        if (beforeDrain.queuedSlots > 0) {
            emit logMessage(tr("[存储] 正在完成写入缓存：%1")
                .arg(bytesText(beforeDrain.queuedBytesApprox())));
        }
        emit diagnosticMessage(tr("[%1][STORAGE] pre-drain queue=%2/%3 backlog=%4")
            .arg(tag)
            .arg(static_cast<qulonglong>(beforeDrain.queuedSlots))
            .arg(static_cast<qulonglong>(beforeDrain.slotCount))
            .arg(bytesText(beforeDrain.queuedBytesApprox())));
    }

    cleanup(ctx, receiveStarted);
    emit deviceReleased();

    if (writerStarted) {
        const auto writerFinishBegin = std::chrono::steady_clock::now();
        writer.finish();
        const auto writerFinishMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - writerFinishBegin).count();
        const WriterSnapshot finalWriter = writer.snapshot();
        metadataWriter = finalWriter;
        const quint64 totalWritten = finalWriter.totalWritten;
        const quint64 totalEnqueued = finalWriter.totalEnqueued;

        emit diagnosticMessage(tr("[%1][STORAGE] sourceBytes=%2 enqueued=%3 sourceDelta=%4")
            .arg(tag).arg(totalBytes).arg(totalEnqueued)
            .arg(totalBytes >= totalEnqueued ? totalBytes - totalEnqueued : 0));
        const double finalAvgWriteUsec = finalWriter.writeCalls
            ? static_cast<double>(finalWriter.totalWriteUsec) / finalWriter.writeCalls : 0.0;
        const ProcIoSnapshot finalProcIo = config_.developerMode ? readProcIo() : ProcIoSnapshot{};
        const MemInfoSnapshot finalMem = config_.developerMode ? readMemInfo() : MemInfoSnapshot{};
        const NvmeThermalSnapshot finalThermal = readNvmeThermal();
        const double activeIoMibps = finalWriter.totalWriteUsec > 0
            ? (finalWriter.totalWritten / 1048576.0)
              / (finalWriter.totalWriteUsec / 1000000.0)
            : 0.0;
        emit logMessage(tr("[存储] 写盘统计：有效 I/O 约 %1 MiB/s | 最大单次写入延迟 %2 ms%3")
            .arg(activeIoMibps, 0, 'f', 1)
            .arg(finalWriter.maxWriteUsec / 1000.0, 0, 'f', 2)
            .arg(finalThermal.valid
                ? tr(" | NVMe %1/%2 °C")
                    .arg(finalThermal.compositeC, 0, 'f', 1)
                    .arg(finalThermal.maxSensorC, 0, 'f', 1)
                : QString()));
        if (activeIoMibps > 0.0 && activeIoMibps < expectedMibps * 0.90) {
            emit logMessage(tr("[警告] 本次存储有效写入低于当前 RX 需求；增大 RAM 缓存只能延长容忍时间，不能解决持续吞吐不足"));
        }
        emit diagnosticMessage(tr("[%1][NVME] finish temp=%2 source=%3")
            .arg(tag).arg(nvmeThermalText(finalThermal)).arg(finalThermal.source));
        emit diagnosticMessage(tr("[%1][STORAGE] finish=%2 ms enqueued=%3 written=%4 peakQueue=%5/%6 failed=%7 writeCalls=%8 avg/maxWrite=%9/%10 us maxBatch=%11 slots maxWrite=%12 slow=%13 flush=%14 avg/maxFlush=%15/%16 us procWchar=%17 procWriteBytes=%18 syscw=%19 dirty=%20 writeback=%21")
            .arg(tag).arg(writerFinishMs)
            .arg(bytesText(totalEnqueued)).arg(bytesText(totalWritten))
            .arg(static_cast<qulonglong>(finalWriter.peakQueuedSlots))
            .arg(static_cast<qulonglong>(finalWriter.slotCount))
            .arg(finalWriter.failed ? 1 : 0)
            .arg(finalWriter.writeCalls)
            .arg(finalAvgWriteUsec, 0, 'f', 1)
            .arg(finalWriter.maxWriteUsec)
            .arg(finalWriter.maxBatchSlots)
            .arg(bytesText(finalWriter.maxWriteBytes))
            .arg(finalWriter.slowWriteCalls)
            .arg(finalWriter.flushCalls)
            .arg(finalWriter.flushCalls ? static_cast<double>(finalWriter.totalFlushUsec) / finalWriter.flushCalls : 0.0, 0, 'f', 1)
            .arg(finalWriter.maxFlushUsec)
            .arg(finalProcIo.valid ? bytesText(finalProcIo.wcharBytes) : QStringLiteral("N/A"))
            .arg(finalProcIo.valid ? bytesText(finalProcIo.writeBytes) : QStringLiteral("N/A"))
            .arg(finalProcIo.valid ? QString::number(finalProcIo.writeSyscalls) : QStringLiteral("N/A"))
            .arg(finalMem.valid ? bytesText(finalMem.dirtyBytes) : QStringLiteral("N/A"))
            .arg(finalMem.valid ? bytesText(finalMem.writebackBytes) : QStringLiteral("N/A")));

        if (writer.failed()) {
            emit logMessage(tr("[错误] %1").arg(writer.errorText()));
        }
        if (totalWritten != totalEnqueued) {
            emit logMessage(tr("[警告] IQ 写入计数不一致：入队 %1，实际写入 %2")
                .arg(mibText(totalEnqueued)).arg(mibText(totalWritten)));
            emit diagnosticMessage(tr("[%1][STORAGE] COUNTER_MISMATCH enqueued=%2 written=%3 delta=%4")
                .arg(tag).arg(totalEnqueued).arg(totalWritten)
                .arg(totalEnqueued > totalWritten ? totalEnqueued - totalWritten : 0));
        }

        const QFileInfo finalFileInfo(config_.iqFilePath);
        metadataFileBytes = finalFileInfo.exists()
            ? static_cast<quint64>(std::max<qint64>(0, finalFileInfo.size())) : 0;
        if (!finalFileInfo.exists()) {
            emit logMessage(tr("[错误] IQ 文件不存在：%1").arg(config_.iqFilePath));
        } else if (metadataFileBytes == 0) {
            emit logMessage(tr("[错误] IQ 文件为空"));
        } else if (metadataFileBytes != totalWritten) {
            emit logMessage(tr("[警告] IQ 文件大小异常：写入 %1，文件 %2")
                .arg(mibText(totalWritten)).arg(mibText(metadataFileBytes)));
        } else {
            emit logMessage(tr("[存储] 文件完成：%1").arg(mibText(metadataFileBytes)));
        }

        const quint64 sampleGroupBytes = static_cast<quint64>(
            std::max(1, enabledChannelCount(config_)) * 4);
        if (sampleGroupBytes > 0 && metadataFileBytes % sampleGroupBytes != 0) {
            emit logMessage(tr("[警告] IQ 文件长度未按通道采样组对齐：余 %1 B")
                .arg(metadataFileBytes % sampleGroupBytes));
        }
        // V1.1.1 no longer performs the old 192 KiB content spot-check here.
        // Full-file readability/content validation is user-triggered after RX
        // has stopped, so it never competes with acquisition or writer drain.
        if (storageStoppedCapture) {
            emit logMessage(tr("[存储] 已保留连续数据前缀；未继续写入不连续数据"));
        }
    }

    if (config_.saveIq && !config_.captureMetadataPath.isEmpty()) {
        if (!writeCaptureMetadata(config_, rxElapsedMs, totalBytes, recvCalls, partialBlocks,
                                  maxReceiveGapMs, everValidIq, storageStoppedCapture,
                                  abnormalTermination, terminationReason, metadataWriter,
                                  metadataFileBytes)) {
            emit logMessage(tr("[警告] 采集元数据保存失败：%1").arg(config_.captureMetadataPath));
        }
    }

    if (!everValidIq) {
        emit logMessage(tr("[警告] 本次采集未检测到有效 IQ 数据"));
    }

    emit diagnosticMessage(tr("[%1][SUMMARY] reason=%2 abnormal=%3 timed=%4 recvCalls=%5 bytes=%6 partial=%7 maxGap=%8 ms validIq=%9 storageStopped=%10")
        .arg(tag).arg(terminationReason).arg(abnormalTermination ? 1 : 0)
        .arg(timedCaptureCompleted ? 1 : 0)
        .arg(recvCalls).arg(bytesText(totalBytes)).arg(partialBlocks)
        .arg(maxReceiveGapMs).arg(everValidIq ? 1 : 0)
        .arg(storageStoppedCapture ? 1 : 0));

    if (abnormalTermination)
        emit logMessage(tr("[采集] 异常结束 | 原因：%1").arg(terminationReason));
    else
        emit logMessage(tr("[采集] 已停止 | 原因：%1").arg(terminationReason));
    emit stopped();
}

