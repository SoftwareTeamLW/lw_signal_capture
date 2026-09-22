#include "data_validator.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
quint64 u64(const QJsonObject& o, const char* key)
{
    return static_cast<quint64>(o.value(QString::fromLatin1(key)).toDouble(0.0));
}

qint64 i64(const QJsonObject& o, const char* key)
{
    return static_cast<qint64>(o.value(QString::fromLatin1(key)).toDouble(0.0));
}

QString bytesText(quint64 bytes)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        return QStringLiteral("%1 GiB").arg(bytes / 1073741824.0, 0, 'f', 2);
    if (bytes >= 1024ULL * 1024ULL)
        return QStringLiteral("%1 MiB").arg(bytes / 1048576.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}
}

DataValidator::DataValidator(QString metadataPath, QObject* parent)
    : QObject(parent), metadataPath_(std::move(metadataPath))
{
}

void DataValidator::requestCancel() noexcept
{
    cancelRequested_.store(true, std::memory_order_release);
}

void DataValidator::run()
{
    QFile metaFile(metadataPath_);
    if (!metaFile.open(QIODevice::ReadOnly)) {
        emit finished(false, tr("无法读取上次采集元数据"), QString());
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(metaFile.readAll());
    if (!doc.isObject()) {
        emit finished(false, tr("上次采集元数据格式无效"), QString());
        return;
    }
    const QJsonObject o = doc.object();
    const QString iqPath = o.value(QStringLiteral("iq_file_path")).toString();
    const int channels = std::max(1, o.value(QStringLiteral("enabled_channel_count")).toInt(1));
    const qint64 sampleRate = i64(o, "sample_rate_hz");
    const qint64 rxElapsedMs = i64(o, "rx_elapsed_ms");
    const quint64 recvBytes = u64(o, "recv_bytes");
    const quint64 enqueuedBytes = u64(o, "writer_enqueued_bytes");
    const quint64 writtenBytes = u64(o, "writer_written_bytes");
    const quint64 partialBlocks = u64(o, "partial_blocks");
    const quint64 runId = u64(o, "run_id");

    QFile iq(iqPath);
    if (!iq.open(QIODevice::ReadOnly)) {
        emit finished(false, tr("无法打开 IQ 文件：%1").arg(iqPath), QString());
        return;
    }

    const quint64 fileBytes = static_cast<quint64>(std::max<qint64>(0, iq.size()));
    const quint64 sampleGroupBytes = static_cast<quint64>(channels) * 4ULL;
    const bool aligned = sampleGroupBytes > 0 && (fileBytes % sampleGroupBytes == 0);
    const quint64 actualSamplesPerChannel = sampleGroupBytes ? fileBytes / sampleGroupBytes : 0;
    const long double expectedLd = sampleRate > 0 && rxElapsedMs > 0
        ? static_cast<long double>(sampleRate) * static_cast<long double>(rxElapsedMs) / 1000.0L
        : 0.0L;
    const quint64 expectedSamplesPerChannel = expectedLd > 0.0L
        ? static_cast<quint64>(std::llround(expectedLd)) : 0;
    const quint64 estimatedMissing = expectedSamplesPerChannel > actualSamplesPerChannel
        ? expectedSamplesPerChannel - actualSamplesPerChannel : 0;
    const double coverage = expectedSamplesPerChannel > 0
        ? 100.0 * static_cast<double>(actualSamplesPerChannel)
          / static_cast<double>(expectedSamplesPerChannel)
        : 0.0;

    // Offline full-file read: verifies the file can be read end-to-end without
    // putting any work on the real-time RX path. It also detects the trivial
    // all-zero-file failure. Exact interior sample-gap localization requires a
    // hardware/driver sequence counter and is therefore not claimed here.
    constexpr qint64 kChunk = 16LL * 1024LL * 1024LL;
    quint64 scanned = 0;
    bool anyNonZero = false;
    bool readError = false;
    int lastPercent = -1;
    while (!iq.atEnd()) {
        if (cancelRequested_.load(std::memory_order_acquire)) {
            emit finished(false, tr("数据校验已取消"), QString());
            return;
        }
        const QByteArray block = iq.read(kChunk);
        if (block.isEmpty() && !iq.atEnd()) {
            readError = true;
            break;
        }
        scanned += static_cast<quint64>(block.size());
        if (!anyNonZero) {
            for (char c : block) {
                if (c != 0) { anyNonZero = true; break; }
            }
        }
        const int percent = fileBytes > 0
            ? static_cast<int>(std::min<quint64>(100, scanned * 100ULL / fileBytes)) : 100;
        if (percent != lastPercent) {
            lastPercent = percent;
            emit progress(percent);
        }
    }

    const quint64 rxToQueueLoss = recvBytes > enqueuedBytes ? recvBytes - enqueuedBytes : 0;
    const quint64 queueToWriterLoss = enqueuedBytes > writtenBytes ? enqueuedBytes - writtenBytes : 0;
    const qint64 writerToFileDelta = static_cast<qint64>(fileBytes) - static_cast<qint64>(writtenBytes);
    const bool exactChainPass = rxToQueueLoss == 0 && queueToWriterLoss == 0
        && writerToFileDelta == 0 && aligned && !readError && anyNonZero;
    const bool durationGap = expectedSamplesPerChannel > 0 && coverage < 99.5;

    QStringList lines;
    lines << QStringLiteral("LuoWave LW39X0 Signal Capture V1.1.1 - Data Validation")
          << QStringLiteral("生成时间: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
          << QStringLiteral("Run: #%1").arg(runId, 3, 10, QLatin1Char('0'))
          << QStringLiteral("IQ 文件: %1").arg(iqPath)
          << QStringLiteral("元数据: %1").arg(metadataPath_)
          << QString()
          << QStringLiteral("[文件]")
          << QStringLiteral("大小: %1").arg(bytesText(fileBytes))
          << QStringLiteral("完整读取: %1").arg(readError ? QStringLiteral("FAIL") : QStringLiteral("PASS"))
          << QStringLiteral("存在非零数据: %1").arg(anyNonZero ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
          << QStringLiteral("通道采样组对齐: %1").arg(aligned ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
          << QString()
          << QStringLiteral("[应用链路计数 - 精确]")
          << QStringLiteral("RX 收到: %1").arg(bytesText(recvBytes))
          << QStringLiteral("进入 Writer: %1").arg(bytesText(enqueuedBytes))
          << QStringLiteral("Writer 完成: %1").arg(bytesText(writtenBytes))
          << QStringLiteral("文件实际: %1").arg(bytesText(fileBytes))
          << QStringLiteral("RX -> Writer 缺失: %1").arg(bytesText(rxToQueueLoss))
          << QStringLiteral("Writer 内缺失: %1").arg(bytesText(queueToWriterLoss))
          << QStringLiteral("Writer -> 文件差值: %1 B").arg(writerToFileDelta)
          << QStringLiteral("Partial block: %1").arg(partialBlocks)
          << QString()
          << QStringLiteral("[采样时长覆盖 - 估算]")
          << QStringLiteral("RX 有效时长: %1 s").arg(rxElapsedMs / 1000.0, 0, 'f', 3)
          << QStringLiteral("采样率: %1 MS/s").arg(sampleRate / 1.0e6, 0, 'f', 3)
          << QStringLiteral("通道数: %1").arg(channels)
          << QStringLiteral("理论样本/通道: %1").arg(expectedSamplesPerChannel)
          << QStringLiteral("文件样本/通道: %1").arg(actualSamplesPerChannel)
          << QStringLiteral("估算缺失/通道: %1").arg(estimatedMissing)
          << QStringLiteral("覆盖率: %1 %").arg(coverage, 0, 'f', 3)
          << QString()
          << QStringLiteral("[结论]")
          << QStringLiteral("应用链路完整性: %1").arg(exactChainPass ? QStringLiteral("PASS") : QStringLiteral("CHECK"))
          << QStringLiteral("采样时长缺口: %1").arg(durationGap
                ? QStringLiteral("存在（估算；无硬件 sample counter，不能精确定位缺口位置）")
                : QStringLiteral("未见明显缺口（估算）"));

    const QString reportDir = QFileInfo(metadataPath_).absolutePath();
    const QString reportPath = QDir(reportDir).filePath(
        QStringLiteral("validation_run_%1_%2.txt")
            .arg(runId, 3, 10, QLatin1Char('0'))
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
    QSaveFile report(reportPath);
    bool reportOk = report.open(QIODevice::WriteOnly | QIODevice::Text);
    if (reportOk) {
        report.write(lines.join(QLatin1Char('\n')).toUtf8());
        report.write("\n");
        reportOk = report.commit();
    }

    const QString summary = durationGap
        ? tr("校验完成：应用链路%1；估算覆盖率 %2%，约缺失 %3 samples/ch")
            .arg(exactChainPass ? tr("正常") : tr("需检查"))
            .arg(coverage, 0, 'f', 3)
            .arg(estimatedMissing)
        : tr("校验完成：应用链路%1；估算覆盖率 %2%")
            .arg(exactChainPass ? tr("正常") : tr("需检查"))
            .arg(coverage, 0, 'f', 3);
    emit finished(reportOk, summary, reportOk ? reportPath : QString());
}
