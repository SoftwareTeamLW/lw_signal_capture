#pragma once

#include <QString>
#include <array>
#include <cstddef>

// One immutable snapshot is created before every Start.
// The RX thread never reads widgets directly.
enum class LwModel { LW3920, LW3940, LW3980 };
enum class LwChannel { A1 = 0, A2, B1, B2, C1, C2, D1, D2 };

struct RxConfig
{
    LwModel model = LwModel::LW3940;
    const char* uri = "pcie_0";          // fixed by the current LW39X0 SDK
    double centerFrequencyHz = 1.0e9;
    long sampleRateHz = 61'440'000;
    double gainDb = 20.0;

    std::array<bool, 8> enabled{};
    // Spectrum always displays every enabled channel. This selection is used
    // only by the waterfall and time-domain views.
    LwChannel displayChannel = LwChannel::A1;

    int fftSize = 2048;
    std::size_t frameBytes = 256 * 1024; // vendor recv_demo default; 32 KiB aligned

    // 0.0 means continuous capture. A positive value requests an automatic
    // graceful stop after this many seconds of actual RX run time.
    double captureDurationSeconds = 0.0;

    bool saveIq = false;
    QString iqFilePath;

    // IQ writer RAM queue. MainWindow resolves Auto to a concrete safe size
    // immediately before Start. The worker never guesses a larger value.
    std::size_t iqBufferBytes = 256ULL * 1024ULL * 1024ULL;
    bool iqBufferAuto = true;

    // Assigned by MainWindow for logging only. It never changes hardware
    // configuration and helps correlate repeated Start/Stop cycles in one log.
    quint64 captureRunId = 0;
};

inline QString modelName(LwModel model)
{
    switch (model) {
    case LwModel::LW3920: return QStringLiteral("LW3920");
    case LwModel::LW3980: return QStringLiteral("LW3980");
    default: return QStringLiteral("LW3940");
    }
}

inline QString channelName(LwChannel ch)
{
    static const char* kNames[] = {"A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2"};
    return QString::fromLatin1(kNames[static_cast<int>(ch)]);
}

inline int enabledChannelCount(const RxConfig& cfg)
{
    int count = 0;
    for (bool enabled : cfg.enabled) count += enabled ? 1 : 0;
    return count;
}

inline bool channelEnabled(const RxConfig& cfg, LwChannel ch)
{
    return cfg.enabled[static_cast<int>(ch)];
}
