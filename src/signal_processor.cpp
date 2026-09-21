#include "signal_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
constexpr float kPi = 3.14159265358979323846f;

std::array<int, 8> relativeChannelIndices(const RxConfig& cfg)
{
    std::array<int, 8> relative{};
    relative.fill(-1);
    int packedIndex = 0;
    for (int ch = 0; ch < 8; ++ch) {
        if (!cfg.enabled[ch]) continue;
        relative[ch] = packedIndex++;
    }
    return relative;
}
}

void SignalProcessor::ensureWorkspace(int fftSize)
{
    if (fftSize <= 0 || window_.size() == fftSize) return;

    window_.resize(fftSize);
    for (int k = 0; k < fftSize; ++k) {
        window_[k] = 0.5f - 0.5f * std::cos(
            2.0f * kPi * static_cast<float>(k) / static_cast<float>(fftSize - 1));
    }
    for (auto& v : realScratch_) v.resize(fftSize);
    for (auto& v : imagScratch_) v.resize(fftSize);
}

DisplayFrame SignalProcessor::makeFrame(const char* raw, std::size_t bytes,
                                        const RxConfig& cfg)
{
    DisplayFrame out;
    out.enabled = cfg.enabled;

    const int channelCount = enabledChannelCount(cfg);
    if (!raw || channelCount <= 0 || cfg.fftSize <= 0) return out;

    // The vendor multi-channel demo checks samples with a stride of
    // 2 * enabled_channels int16 values, so this decoder keeps the packing
    // assumption in one place:
    //   ch0 I,Q, ch1 I,Q, ...
    const auto* samples = reinterpret_cast<const std::int16_t*>(raw);
    const std::size_t shortCount = bytes / sizeof(std::int16_t);
    const std::size_t stride = static_cast<std::size_t>(2 * channelCount);
    const std::size_t available = shortCount / stride;
    if (available < static_cast<std::size_t>(cfg.fftSize)) return out;

    const auto relative = relativeChannelIndices(cfg);
    const int monitor = static_cast<int>(cfg.displayChannel);
    out.monitorI.resize(cfg.fftSize);
    out.monitorQ.resize(cfg.fftSize);

    // Reuse the Hann window and FFT scratch buffers between preview frames.
    // This removes repeated heap allocation/cos() work from the RX thread.
    ensureWorkspace(cfg.fftSize);

    for (int absolute = 0; absolute < 8; ++absolute) {
        if (!cfg.enabled[absolute]) continue;
        const int packed = relative[absolute];
        if (packed < 0) continue;

        QVector<float>& real = realScratch_[absolute];
        QVector<float>& imag = imagScratch_[absolute];
        const std::size_t offset = static_cast<std::size_t>(2 * packed);

        for (int k = 0; k < cfg.fftSize; ++k) {
            const std::size_t base = static_cast<std::size_t>(k) * stride + offset;
            const float i = static_cast<float>(samples[base]) / 32768.0f;
            const float q = static_cast<float>(samples[base + 1]) / 32768.0f;
            if (absolute == monitor) {
                out.monitorI[k] = i;
                out.monitorQ[k] = q;
            }
            real[k] = i * window_[k];
            imag[k] = q * window_[k];
        }

        fft(real, imag);
        auto& spectrum = out.spectrumDb[absolute];
        spectrum.resize(cfg.fftSize);
        const float norm = static_cast<float>(cfg.fftSize);
        for (int k = 0; k < cfg.fftSize; ++k) {
            const int src = (k + cfg.fftSize / 2) % cfg.fftSize; // FFT shift
            const float mag = std::sqrt(real[src] * real[src] + imag[src] * imag[src]) / norm;
            spectrum[k] = 20.0f * std::log10(std::max(mag, 1.0e-9f));
        }
    }

    return out;
}

void SignalProcessor::fft(QVector<float>& real, QVector<float>& imag)
{
    const int n = real.size();
    if (n <= 1 || (n & (n - 1)) != 0) return;

    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * kPi / static_cast<float>(len);
        const float wLenR = std::cos(angle);
        const float wLenI = std::sin(angle);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f;
            float wi = 0.0f;
            for (int j = 0; j < len / 2; ++j) {
                const int u = i + j;
                const int v = i + j + len / 2;
                const float vr = real[v] * wr - imag[v] * wi;
                const float vi = real[v] * wi + imag[v] * wr;
                const float ur = real[u];
                const float ui = imag[u];
                real[u] = ur + vr;
                imag[u] = ui + vi;
                real[v] = ur - vr;
                imag[v] = ui - vi;
                const float nextWr = wr * wLenR - wi * wLenI;
                wi = wr * wLenI + wi * wLenR;
                wr = nextWr;
            }
        }
    }
}
