#pragma once

#include <QImage>
#include <QPixmap>
#include <QString>
#include <QVector>
#include <QWidget>
#include <array>

class QMouseEvent;
class QResizeEvent;

// Real-time and Average are the two base traces. Max/Min Hold are independent
// overlays so both can be enabled at the same time and can follow different
// RX channels.
enum class SpectrumTraceMode
{
    RealTime,
    Average
};

enum class MarkerTraceSource
{
    BaseTrace,
    MaxHold,
    MinHold
};

enum class WaterfallPalette
{
    Inferno,
    Jet,
    WhiteHot,
    BlackHot
};

class SpectrumWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit SpectrumWidget(QWidget* parent = nullptr);

    void setSpectra(const std::array<QVector<float>, 8>& traces,
                    const std::array<bool, 8>& enabled,
                    double centerHz, double sampleRateHz);

    void setTraceMode(SpectrumTraceMode mode);
    SpectrumTraceMode traceMode() const { return traceMode_; }
    void setAverageAlpha(float alpha);
    float averageAlpha() const { return averageAlpha_; }
    void setLevelRange(float floorDbfs, float ceilingDbfs);
    float floorDbfs() const { return floorDbfs_; }
    float ceilingDbfs() const { return ceilingDbfs_; }
    void setSpanFraction(float fraction);
    float spanFraction() const { return spanFraction_; }

    void setMaxHoldEnabled(bool enabled);
    void setMinHoldEnabled(bool enabled);
    void setMaxHoldChannel(int channel);
    void setMinHoldChannel(int channel);
    bool maxHoldEnabled() const { return maxHoldEnabled_; }
    bool minHoldEnabled() const { return minHoldEnabled_; }
    int maxHoldChannel() const { return maxHoldChannel_; }
    int minHoldChannel() const { return minHoldChannel_; }
    void clearHolds();

    // Marker model: up to four markers can stay enabled simultaneously. One
    // marker is selected as the active marker and is moved by clicking/dragging
    // inside the spectrum plot, similar to a bench spectrum analyser.
    void setActiveMarker(int markerIndex);
    int activeMarker() const { return activeMarker_; }
    void setMarkerEnabled(int markerIndex, bool enabled);
    bool markerEnabled(int markerIndex) const;
    void setMarkerChannel(int markerIndex, int channel);
    int markerChannel(int markerIndex) const;
    void setMarkerSource(int markerIndex, MarkerTraceSource source);
    MarkerTraceSource markerSource(int markerIndex) const;
    void clearMarkers();

    void clear();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    struct MarkerState
    {
        bool enabled = false;
        int channel = 0;
        MarkerTraceSource source = MarkerTraceSource::BaseTrace;
        double frequencyHz = 0.0;
    };

    void updateTraceHistory(const std::array<QVector<float>, 8>& traces,
                            const std::array<bool, 8>& enabled);
    const QVector<float>& baseTrace(int channel) const;
    const QVector<float>& markerTrace(const MarkerState& marker) const;
    void moveActiveMarker(double x);

    std::array<QVector<float>, 8> realtime_;
    std::array<QVector<float>, 8> average_;
    std::array<QVector<float>, 8> maxHold_;
    std::array<QVector<float>, 8> minHold_;
    std::array<bool, 8> enabled_{};
    std::array<MarkerState, 4> markers_{};

    SpectrumTraceMode traceMode_ = SpectrumTraceMode::Average;
    float averageAlpha_ = 0.35f;
    bool averageResetPending_ = true;
    float floorDbfs_ = -120.0f;
    float ceilingDbfs_ = 0.0f;
    float spanFraction_ = 1.0f;

    bool maxHoldEnabled_ = false;
    bool minHoldEnabled_ = false;
    int maxHoldChannel_ = 0;
    int minHoldChannel_ = 0;
    int activeMarker_ = 0;

    double centerHz_ = 1.0e9;
    double sampleRateHz_ = 61.44e6;
};

class WaterfallWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit WaterfallWidget(QWidget* parent = nullptr);

    void appendSpectrum(const QVector<float>& db);
    void setChannelLabel(const QString& label);
    void setLevelRange(float floorDbfs, float ceilingDbfs);
    void setFrequencyRange(double centerHz, double sampleRateHz);
    void setSpanFraction(float fraction);
    void setPalette(WaterfallPalette palette);
    WaterfallPalette palette() const { return palette_; }
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void resetRaster(int columns);
    void rebuildImage();
    void rebuildPresentationPixmap();

    // Waterfall uses a fixed-width circular raster instead of shifting the
    // entire image on every frame. dbRaster_ retains the reduced dBFS values
    // so palette/range changes can recolor existing history without touching
    // the acquisition or FFT paths.
    QImage image_;
    QPixmap presentationPixmap_;
    QVector<float> dbRaster_;
    int rasterColumns_ = 0;
    int newestRow_ = 0;
    int validRows_ = 0;
    QString channelLabel_ = QStringLiteral("A1");
    float floorDbfs_ = -110.0f;
    float ceilingDbfs_ = -20.0f;
    double centerHz_ = 1.0e9;
    double sampleRateHz_ = 61.44e6;
    float spanFraction_ = 1.0f;
    int historyRows_ = 180;
    WaterfallPalette palette_ = WaterfallPalette::Inferno;
};

class ConstellationPlaceholderWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit ConstellationPlaceholderWidget(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;
};

class WaveformWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget* parent = nullptr);
    void setSamples(const QVector<float>& i, const QVector<float>& q,
                    double sampleRateHz, const QString& channelLabel);
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<float> i_;
    QVector<float> q_;
    double sampleRateHz_ = 61.44e6;
    QString channelLabel_ = QStringLiteral("A1");
    float displayRange_ = 1.0f;
};
