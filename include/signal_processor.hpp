#pragma once

#include "rx_config.hpp"

#include <QMetaType>
#include <QVector>
#include <array>
#include <cstddef>

// A preview frame is deliberately small. Raw DMA data never goes through the
// GUI event queue. Spectrum data is generated for every enabled RX channel,
// while I/Q time samples are kept only for the selected monitor channel.
struct DisplayFrame
{
    std::array<QVector<float>, 8> spectrumDb;
    std::array<bool, 8> enabled{};
    QVector<float> monitorI;
    QVector<float> monitorQ;
};
Q_DECLARE_METATYPE(DisplayFrame)

class SignalProcessor
{
public:
    DisplayFrame makeFrame(const char* raw, std::size_t bytes, const RxConfig& cfg);

private:
    void ensureWorkspace(int fftSize);
    static void fft(QVector<float>& real, QVector<float>& imag);

    QVector<float> window_;
    std::array<QVector<float>, 8> realScratch_;
    std::array<QVector<float>, 8> imagScratch_;
};
