#include "plot_widgets.hpp"

#include <QCoreApplication>
#include <QFont>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace {
QRectF plotRect(const QWidget* w)
{
    constexpr int left = 66;
    constexpr int right = 18;
    constexpr int top = 34;
    constexpr int bottom = 34;
    return QRectF(left, top,
                  std::max(1, w->width() - left - right),
                  std::max(1, w->height() - top - bottom));
}

void drawGrid(QPainter& p, const QRectF& r)
{
    p.setPen(QPen(QColor(64, 77, 82), 1));
    for (int i = 0; i <= 10; ++i) {
        const qreal x = r.left() + r.width() * i / 10.0;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    for (int i = 0; i <= 8; ++i) {
        const qreal y = r.top() + r.height() * i / 8.0;
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.setPen(QPen(QColor(91, 106, 112), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
}

QString mhz(double hz)
{
    return QString::number(hz / 1.0e6, 'f', 3) + QStringLiteral(" MHz");
}

QString channelNameByIndex(int index)
{
    static const char* names[] = {"A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2"};
    return QString::fromLatin1(names[std::clamp(index, 0, 7)]);
}

QColor channelColor(int index)
{
    static const QColor colors[] = {
        QColor(255, 213, 79),   // A1 yellow
        QColor(79, 195, 247),   // A2 cyan
        QColor(129, 199, 132),  // B1 green
        QColor(255, 138, 101),  // B2 orange
        QColor(186, 104, 200),  // C1 purple
        QColor(100, 181, 246),  // C2 blue
        QColor(240, 98, 146),   // D1 pink
        QColor(174, 213, 129)   // D2 lime
    };
    return colors[std::clamp(index, 0, 7)];
}

QColor markerColor(int index)
{
    static const QColor colors[] = {
        QColor(255, 235, 59),
        QColor(38, 198, 218),
        QColor(255, 112, 176),
        QColor(126, 211, 33)
    };
    return colors[std::clamp(index, 0, 3)];
}

QString traceModeText(SpectrumTraceMode mode)
{
    return mode == SpectrumTraceMode::RealTime
        ? QCoreApplication::translate("PlotWidgets", "REALTIME")
        : QCoreApplication::translate("PlotWidgets", "AVERAGE");
}

QString markerSourceText(MarkerTraceSource source)
{
    switch (source) {
    case MarkerTraceSource::MaxHold: return QStringLiteral("MAX");
    case MarkerTraceSource::MinHold: return QStringLiteral("MIN");
    default: return QStringLiteral("TRC");
    }
}

QString paletteText(WaterfallPalette palette)
{
    switch (palette) {
    case WaterfallPalette::Inferno:  return QStringLiteral("Inferno");
    case WaterfallPalette::Jet:      return QStringLiteral("Jet");
    case WaterfallPalette::WhiteHot: return QStringLiteral("White Hot");
    case WaterfallPalette::BlackHot: return QStringLiteral("Black Hot");
    }
    return QStringLiteral("Inferno");
}

QColor interpolate(const QColor& a, const QColor& b, float u)
{
    u = std::clamp(u, 0.0f, 1.0f);
    return QColor(
        static_cast<int>(a.red()   + u * (b.red()   - a.red())),
        static_cast<int>(a.green() + u * (b.green() - a.green())),
        static_cast<int>(a.blue()  + u * (b.blue()  - a.blue())));
}

QColor waterfallColor(float t, WaterfallPalette palette)
{
    t = std::clamp(t, 0.0f, 1.0f);

    if (palette == WaterfallPalette::WhiteHot) {
        const int v = static_cast<int>(255.0f * std::pow(t, 0.82f));
        return QColor(v, v, v);
    }
    if (palette == WaterfallPalette::BlackHot) {
        const int v = static_cast<int>(255.0f * (1.0f - std::pow(t, 0.82f)));
        return QColor(v, v, v);
    }
    if (palette == WaterfallPalette::Jet) {
        const auto component = [t](float center) {
            return std::clamp(1.5f - std::fabs(4.0f * t - center), 0.0f, 1.0f);
        };
        return QColor::fromRgbF(component(3.0f), component(2.0f), component(1.0f));
    }

    // Inferno-like default: restrained on a dark UI and visually dense without
    // the large blue region of Jet.
    struct Stop { float x; QColor color; };
    static const Stop stops[] = {
        {0.00f, QColor(5, 5, 10)},
        {0.18f, QColor(42, 12, 58)},
        {0.38f, QColor(108, 22, 72)},
        {0.58f, QColor(188, 50, 48)},
        {0.78f, QColor(241, 132, 39)},
        {1.00f, QColor(255, 236, 150)}
    };

    for (std::size_t i = 1; i < std::size(stops); ++i) {
        if (t > stops[i].x) continue;
        const Stop& a = stops[i - 1];
        const Stop& b = stops[i];
        const float u = (t - a.x) / std::max(0.0001f, b.x - a.x);
        return interpolate(a.color, b.color, u);
    }
    return stops[std::size(stops) - 1].color;
}

// Keep plotting cost tied to screen width rather than FFT length. For each
// screen bucket retain the local maximum so narrow peaks are not lost.
QPolygonF decimatedSpectrum(const QVector<float>& db, const QRectF& r,
                            float bottomDb, float topDb,
                            int firstBin = 0, int lastBinExclusive = -1)
{
    QPolygonF line;
    if (db.size() < 2) return line;

    const int pointCount = static_cast<int>(db.size());
    firstBin = std::clamp(firstBin, 0, pointCount - 1);
    if (lastBinExclusive < 0) lastBinExclusive = pointCount;
    lastBinExclusive = std::clamp(lastBinExclusive, firstBin + 1, pointCount);
    const int visibleCount = lastBinExclusive - firstBin;
    if (visibleCount < 2) return line;

    const int targetPoints = std::clamp(static_cast<int>(r.width() * 0.95), 220, 900);
    const int bucket = std::max(1, (visibleCount + targetPoints - 1) / targetPoints);
    line.reserve((visibleCount + bucket - 1) / bucket);

    for (int begin = firstBin; begin < lastBinExclusive; begin += bucket) {
        const int end = std::min(begin + bucket, lastBinExclusive);
        float value = db[begin];
        for (int k = begin + 1; k < end; ++k) value = std::max(value, db[k]);

        const qreal sampleIndex = 0.5 * (begin + end - 1);
        const qreal x = r.left() + r.width()
            * (sampleIndex - firstBin) / (visibleCount - 1.0);
        const float clipped = std::clamp(value, bottomDb, topDb);
        const qreal y = r.top() + r.height() * (clipped - topDb) / (bottomDb - topDb);
        line << QPointF(x, y);
    }
    return line;
}

std::pair<int, int> visibleBinRange(int pointCount, float spanFraction)
{
    if (pointCount <= 1) return {0, std::max(0, pointCount)};
    spanFraction = std::clamp(spanFraction, 0.05f, 1.0f);
    const int visibleCount = std::clamp(
        static_cast<int>(std::llround(pointCount * spanFraction)), 2, pointCount);
    const int first = (pointCount - visibleCount) / 2;
    return {first, first + visibleCount};
}


double binFrequencyHz(int bin, int pointCount, double centerHz, double sampleRateHz)
{
    if (pointCount <= 0 || sampleRateHz <= 0.0) return centerHz;
    return centerHz
        + (static_cast<double>(bin) - static_cast<double>(pointCount) / 2.0)
            * sampleRateHz / static_cast<double>(pointCount);
}

int frequencyBin(double frequencyHz, int pointCount, double centerHz, double sampleRateHz)
{
    if (pointCount <= 0 || sampleRateHz <= 0.0) return -1;
    return static_cast<int>(std::llround(
        (frequencyHz - centerHz) * static_cast<double>(pointCount) / sampleRateHz
        + static_cast<double>(pointCount) / 2.0));
}

void drawVerticalDbfsLabel(QPainter& p, const QRectF& r)
{
    p.save();
    p.setPen(QColor(165, 177, 181));
    p.translate(13.0, r.center().y());
    p.rotate(-90.0);
    p.drawText(QRectF(-r.height() / 2.0, -9.0, r.height(), 18.0),
               Qt::AlignCenter, QStringLiteral("dBFS"));
    p.restore();
}
}

SpectrumWidget::SpectrumWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(270);
    setAutoFillBackground(false);
    setMouseTracking(true);
}

void SpectrumWidget::updateTraceHistory(const std::array<QVector<float>, 8>& traces,
                                        const std::array<bool, 8>& enabled)
{
    for (int ch = 0; ch < 8; ++ch) {
        if (!enabled[ch] || traces[ch].isEmpty()) {
            realtime_[ch].clear();
            continue;
        }

        realtime_[ch] = traces[ch];
        const int count = static_cast<int>(traces[ch].size());

        // Average is updated only while Average is the active base trace. This
        // both saves GUI-thread work and makes switching back to Average start
        // from the current spectrum instead of replaying stale history.
        if (traceMode_ == SpectrumTraceMode::Average) {
            if (averageResetPending_ || average_[ch].size() != count) {
                average_[ch] = traces[ch];
            } else {
                for (int k = 0; k < count; ++k) {
                    average_[ch][k] = average_[ch][k] * (1.0f - averageAlpha_)
                                    + traces[ch][k] * averageAlpha_;
                }
            }
        }

        if (maxHoldEnabled_ && ch == maxHoldChannel_) {
            if (maxHold_[ch].size() != count) {
                maxHold_[ch] = traces[ch];
            } else {
                for (int k = 0; k < count; ++k)
                    maxHold_[ch][k] = std::max(maxHold_[ch][k], traces[ch][k]);
            }
        }

        if (minHoldEnabled_ && ch == minHoldChannel_) {
            if (minHold_[ch].size() != count) {
                minHold_[ch] = traces[ch];
            } else {
                for (int k = 0; k < count; ++k)
                    minHold_[ch][k] = std::min(minHold_[ch][k], traces[ch][k]);
            }
        }
    }
    if (traceMode_ == SpectrumTraceMode::Average) averageResetPending_ = false;
}

const QVector<float>& SpectrumWidget::baseTrace(int channel) const
{
    if (traceMode_ == SpectrumTraceMode::RealTime) return realtime_[channel];
    return (averageResetPending_ || average_[channel].isEmpty())
        ? realtime_[channel] : average_[channel];
}

const QVector<float>& SpectrumWidget::markerTrace(const MarkerState& marker) const
{
    static const QVector<float> empty;
    if (marker.channel < 0 || marker.channel >= 8 || !enabled_[marker.channel]) return empty;

    switch (marker.source) {
    case MarkerTraceSource::MaxHold:
        return maxHoldEnabled_ ? maxHold_[marker.channel] : empty;
    case MarkerTraceSource::MinHold:
        return minHoldEnabled_ ? minHold_[marker.channel] : empty;
    default:
        return baseTrace(marker.channel);
    }
}

void SpectrumWidget::setSpectra(const std::array<QVector<float>, 8>& traces,
                                const std::array<bool, 8>& enabled,
                                double centerHz, double sampleRateHz)
{
    enabled_ = enabled;
    centerHz_ = centerHz;
    sampleRateHz_ = sampleRateHz;
    updateTraceHistory(traces, enabled);
    update();
}

void SpectrumWidget::setTraceMode(SpectrumTraceMode mode)
{
    if (traceMode_ == mode) return;
    traceMode_ = mode;
    if (mode == SpectrumTraceMode::Average) averageResetPending_ = true;
    update();
}

void SpectrumWidget::setAverageAlpha(float alpha)
{
    alpha = std::clamp(alpha, 0.01f, 1.0f);
    if (std::fabs(averageAlpha_ - alpha) < 0.0001f) return;
    averageAlpha_ = alpha;
    averageResetPending_ = true;
    update();
}

void SpectrumWidget::setLevelRange(float floorDbfs, float ceilingDbfs)
{
    if (ceilingDbfs <= floorDbfs + 1.0f) return;
    if (std::fabs(floorDbfs_ - floorDbfs) < 0.01f
        && std::fabs(ceilingDbfs_ - ceilingDbfs) < 0.01f) return;
    floorDbfs_ = floorDbfs;
    ceilingDbfs_ = ceilingDbfs;
    update();
}

void SpectrumWidget::setSpanFraction(float fraction)
{
    fraction = std::clamp(fraction, 0.05f, 1.0f);
    if (std::fabs(spanFraction_ - fraction) < 0.0001f) return;
    spanFraction_ = fraction;
    update();
}

void SpectrumWidget::setMaxHoldEnabled(bool enabled)
{
    if (maxHoldEnabled_ == enabled) return;
    maxHoldEnabled_ = enabled;
    if (enabled) maxHold_[maxHoldChannel_].clear();
    update();
}

void SpectrumWidget::setMinHoldEnabled(bool enabled)
{
    if (minHoldEnabled_ == enabled) return;
    minHoldEnabled_ = enabled;
    if (enabled) minHold_[minHoldChannel_].clear();
    update();
}

void SpectrumWidget::setMaxHoldChannel(int channel)
{
    channel = std::clamp(channel, 0, 7);
    if (maxHoldChannel_ == channel) return;
    maxHoldChannel_ = channel;
    maxHold_[channel].clear();
    for (auto& marker : markers_) {
        if (marker.source == MarkerTraceSource::MaxHold) marker.channel = channel;
    }
    update();
}

void SpectrumWidget::setMinHoldChannel(int channel)
{
    channel = std::clamp(channel, 0, 7);
    if (minHoldChannel_ == channel) return;
    minHoldChannel_ = channel;
    minHold_[channel].clear();
    for (auto& marker : markers_) {
        if (marker.source == MarkerTraceSource::MinHold) marker.channel = channel;
    }
    update();
}

void SpectrumWidget::clearHolds()
{
    for (auto& trace : maxHold_) trace.clear();
    for (auto& trace : minHold_) trace.clear();
    update();
}

void SpectrumWidget::setActiveMarker(int markerIndex)
{
    activeMarker_ = std::clamp(markerIndex, 0, 3);
    setCursor(markers_[activeMarker_].enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void SpectrumWidget::setMarkerEnabled(int markerIndex, bool enabled)
{
    markerIndex = std::clamp(markerIndex, 0, 3);
    MarkerState& marker = markers_[markerIndex];
    marker.enabled = enabled;
    if (enabled && marker.frequencyHz == 0.0) marker.frequencyHz = centerHz_;
    if (markerIndex == activeMarker_)
        setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

bool SpectrumWidget::markerEnabled(int markerIndex) const
{
    return markers_[std::clamp(markerIndex, 0, 3)].enabled;
}

void SpectrumWidget::setMarkerChannel(int markerIndex, int channel)
{
    markerIndex = std::clamp(markerIndex, 0, 3);
    markers_[markerIndex].channel = std::clamp(channel, 0, 7);
    update();
}

int SpectrumWidget::markerChannel(int markerIndex) const
{
    return markers_[std::clamp(markerIndex, 0, 3)].channel;
}

void SpectrumWidget::setMarkerSource(int markerIndex, MarkerTraceSource source)
{
    markerIndex = std::clamp(markerIndex, 0, 3);
    MarkerState& marker = markers_[markerIndex];
    marker.source = source;
    if (source == MarkerTraceSource::MaxHold) marker.channel = maxHoldChannel_;
    if (source == MarkerTraceSource::MinHold) marker.channel = minHoldChannel_;
    update();
}

MarkerTraceSource SpectrumWidget::markerSource(int markerIndex) const
{
    return markers_[std::clamp(markerIndex, 0, 3)].source;
}

void SpectrumWidget::clearMarkers()
{
    // Preserve each marker's channel assignment; only disable markers and
    // clear positions. This avoids reverting to A1 when the current channel
    // combination contains only B/C/D channels.
    for (auto& marker : markers_) {
        marker.enabled = false;
        marker.frequencyHz = 0.0;
    }
    activeMarker_ = 0;
    setCursor(Qt::ArrowCursor);
    update();
}

void SpectrumWidget::clear()
{
    for (auto& trace : realtime_) trace.clear();
    for (auto& trace : average_) trace.clear();
    for (auto& trace : maxHold_) trace.clear();
    for (auto& trace : minHold_) trace.clear();
    enabled_.fill(false);
    averageResetPending_ = true;
    update();
}

void SpectrumWidget::moveActiveMarker(double x)
{
    MarkerState& marker = markers_[activeMarker_];
    if (!marker.enabled || sampleRateHz_ <= 0.0) return;

    const QRectF r = plotRect(this);
    const double clampedX = std::clamp(x, r.left(), r.right());
    const double fraction = (clampedX - r.left()) / std::max(1.0, r.width());
    const double displaySpanHz = sampleRateHz_ * static_cast<double>(spanFraction_);
    const double lowHz = centerHz_ - displaySpanHz / 2.0;

    // Snap to the nearest displayed FFT bin when data is available. This makes
    // the marker readout deterministic and closer to a conventional spectrum
    // analyser instead of reporting an arbitrary sub-bin mouse coordinate.
    const QVector<float>& selectedTrace = markerTrace(marker);
    const QVector<float>& fallbackTrace = baseTrace(marker.channel);
    const QVector<float>& trace = selectedTrace.size() >= 2 ? selectedTrace : fallbackTrace;
    if (trace.size() >= 2) {
        const int pointCount = static_cast<int>(trace.size());
        const double targetFrequency = lowHz + fraction * displaySpanHz;
        const int bin = std::clamp(
            frequencyBin(targetFrequency, pointCount, centerHz_, sampleRateHz_),
            0, pointCount - 1);
        marker.frequencyHz = binFrequencyHz(bin, pointCount, centerHz_, sampleRateHz_);
    } else {
        marker.frequencyHz = lowHz + fraction * displaySpanHz;
    }
    update();
}

void SpectrumWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && plotRect(this).contains(event->position())) {
        moveActiveMarker(event->position().x());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void SpectrumWidget::mouseMoveEvent(QMouseEvent* event)
{
    if ((event->buttons() & Qt::LeftButton) && plotRect(this).contains(event->position())) {
        moveActiveMarker(event->position().x());
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void SpectrumWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(25, 31, 34));
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = plotRect(this);
    drawGrid(p, r);

    const float topDb = ceilingDbfs_;
    const float bottomDb = floorDbfs_;
    const double displaySpanHz = sampleRateHz_ * static_cast<double>(spanFraction_);
    p.setPen(QColor(181, 191, 194));

    p.drawText(QRectF(r.left() + 8, 6, 310, 20), Qt::AlignLeft | Qt::AlignVCenter,
               tr("MULTI-CHANNEL RX SPECTRUM"));
    drawVerticalDbfsLabel(p, r);

    constexpr int verticalDivisions = 6;
    for (int i = 0; i <= verticalDivisions; ++i) {
        const float value = topDb
            - (topDb - bottomDb) * static_cast<float>(i) / verticalDivisions;
        const qreal y = r.top() + r.height() * (value - topDb) / (bottomDb - topDb);
        p.drawText(QRectF(18, y - 9, 40, 18), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(value, 'f', std::fabs(value - std::round(value)) < 0.05f ? 0 : 1));
    }

    p.drawText(QRectF(r.left(), r.bottom() + 7, r.width(), 20), Qt::AlignLeft,
               mhz(centerHz_ - displaySpanHz / 2.0));
    p.drawText(QRectF(r.left(), r.bottom() + 7, r.width(), 20), Qt::AlignHCenter,
               mhz(centerHz_));
    p.drawText(QRectF(r.left(), r.bottom() + 7, r.width(), 20), Qt::AlignRight,
               mhz(centerHz_ + displaySpanHz / 2.0));

    const QString modeText = traceModeText(traceMode_);
    const QRectF modeBadge(r.right() - 134, 6, 126, 20);
    p.save();
    p.setPen(QPen(QColor(94, 109, 114), 1));
    p.setBrush(QColor(54, 65, 69));
    p.drawRoundedRect(modeBadge, 4, 4);
    p.setPen(QColor(214, 221, 223));
    p.drawText(modeBadge, Qt::AlignCenter,
               tr("BASE · %1").arg(modeText));
    p.restore();

    int enabledCount = 0;
    for (bool enabled : enabled_) enabledCount += enabled ? 1 : 0;

    // Trace legend: base trace and hold traces are deliberately labelled as
    // independent paths. Max/Min Hold consume the instantaneous FFT directly;
    // they are not post-processing of the Average base trace.
    if (enabledCount > 0) {
        const int columns = enabledCount > 4 ? 2 : 1;
        const int rows = (enabledCount + columns - 1) / columns;
        const int holdRows = (maxHoldEnabled_ ? 1 : 0) + (minHoldEnabled_ ? 1 : 0);
        constexpr qreal itemWidth = 112.0;
        const qreal legendW = columns * itemWidth + 14.0;
        const qreal legendH = rows * 20.0 + holdRows * 20.0 + 12.0;
        const QRectF legend(r.right() - legendW - 8.0, r.top() + 8.0, legendW, legendH);

        p.save();
        p.setPen(QPen(QColor(84, 98, 104), 1));
        p.setBrush(QColor(12, 18, 20, 228));
        p.drawRoundedRect(legend, 4, 4);
        p.restore();

        const QString baseSuffix = traceMode_ == SpectrumTraceMode::Average
            ? QStringLiteral("AVG") : QStringLiteral("RT");
        int item = 0;
        for (int ch = 0; ch < 8; ++ch) {
            if (!enabled_[ch]) continue;
            const int col = item / rows;
            const int row = item % rows;
            const qreal x = legend.left() + 9.0 + col * itemWidth;
            const qreal y = legend.top() + 8.0 + row * 20.0;
            QPen legendPen(channelColor(ch));
            legendPen.setWidthF(1.25);
            legendPen.setCosmetic(true);
            p.setPen(legendPen);
            p.drawLine(QPointF(x, y + 6), QPointF(x + 22, y + 6));
            p.setPen(QColor(231, 235, 236));
            p.drawText(QRectF(x + 28, y - 3, itemWidth - 34.0, 18),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("%1 · %2")
                           .arg(channelNameByIndex(ch), baseSuffix));
            ++item;
        }

        qreal holdY = legend.top() + 8.0 + rows * 20.0;
        if (maxHoldEnabled_) {
            QPen holdPen(QColor(255, 183, 77), 1.15, Qt::DashLine);
            holdPen.setCosmetic(true);
            p.setPen(holdPen);
            p.drawLine(QPointF(legend.left() + 9, holdY + 6),
                       QPointF(legend.left() + 31, holdY + 6));
            p.setPen(QColor(230, 230, 235));
            p.drawText(QRectF(legend.left() + 37, holdY - 3, legend.width() - 44, 18),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       tr("MAX HOLD · %1")
                           .arg(channelNameByIndex(maxHoldChannel_)));
            holdY += 20.0;
        }
        if (minHoldEnabled_) {
            QPen holdPen(QColor(206, 147, 216), 1.15, Qt::DotLine);
            holdPen.setCosmetic(true);
            p.setPen(holdPen);
            p.drawLine(QPointF(legend.left() + 9, holdY + 6),
                       QPointF(legend.left() + 31, holdY + 6));
            p.setPen(QColor(230, 230, 235));
            p.drawText(QRectF(legend.left() + 37, holdY - 3, legend.width() - 44, 18),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       tr("MIN HOLD · %1")
                           .arg(channelNameByIndex(minHoldChannel_)));
        }
    }

    p.save();
    p.setClipRect(r.adjusted(1, 1, -1, -1));

    // Base traces: all enabled channels, deliberately thin for multi-channel use.
    for (int ch = 0; ch < 8; ++ch) {
        if (!enabled_[ch]) continue;
        const QVector<float>& trace = baseTrace(ch);
        if (trace.size() < 2) continue;

        const auto [firstBin, lastBin] = visibleBinRange(
            static_cast<int>(trace.size()), spanFraction_);

        QPen pen(channelColor(ch));
        pen.setWidthF(0.72);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawPolyline(decimatedSpectrum(trace, r, bottomDb, topDb, firstBin, lastBin));
    }

    // Hold traces are overlays. They are independent, so Max and Min can be
    // active simultaneously and target different channels.
    if (maxHoldEnabled_ && enabled_[maxHoldChannel_] && maxHold_[maxHoldChannel_].size() >= 2) {
        const auto [firstBin, lastBin] = visibleBinRange(
            static_cast<int>(maxHold_[maxHoldChannel_].size()), spanFraction_);
        QPen pen(QColor(255, 183, 77), 1.10, Qt::DashLine);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawPolyline(decimatedSpectrum(
            maxHold_[maxHoldChannel_], r, bottomDb, topDb, firstBin, lastBin));
    }
    if (minHoldEnabled_ && enabled_[minHoldChannel_] && minHold_[minHoldChannel_].size() >= 2) {
        const auto [firstBin, lastBin] = visibleBinRange(
            static_cast<int>(minHold_[minHoldChannel_].size()), spanFraction_);
        QPen pen(QColor(206, 147, 216), 1.10, Qt::DotLine);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawPolyline(decimatedSpectrum(
            minHold_[minHoldChannel_], r, bottomDb, topDb, firstBin, lastBin));
    }

    // Markers are drawn after traces. The selected marker is slightly larger;
    // all enabled markers remain visible at the same time.
    for (int markerIndex = 0; markerIndex < 4; ++markerIndex) {
        const MarkerState& marker = markers_[markerIndex];
        if (!marker.enabled || marker.channel < 0 || marker.channel >= 8
            || !enabled_[marker.channel]) continue;

        const QVector<float>& trace = markerTrace(marker);
        if (trace.size() < 2 || sampleRateHz_ <= 0.0) continue;

        const int pointCount = static_cast<int>(trace.size());
        const int bin = frequencyBin(marker.frequencyHz, pointCount, centerHz_, sampleRateHz_);
        if (bin < 0 || bin >= pointCount) continue;
        const double lowHz = centerHz_ - displaySpanHz / 2.0;
        const double highHz = centerHz_ + displaySpanHz / 2.0;
        if (marker.frequencyHz < lowHz || marker.frequencyHz > highHz) continue;
        const float value = std::clamp(trace[bin], bottomDb, topDb);
        const qreal x = r.left() + r.width()
            * (marker.frequencyHz - lowHz) / std::max(1.0, displaySpanHz);
        const qreal y = r.top() + r.height() * (value - topDb) / (bottomDb - topDb);

        const QColor color = markerColor(markerIndex);
        QPen markerPen(color, markerIndex == activeMarker_ ? 1.4 : 1.0);
        markerPen.setCosmetic(true);
        p.setPen(markerPen);
        const qreal laneOffset = 20.0 + markerIndex * 14.0;
        const qreal tagY = std::max(r.top() + 3.0, y - laneOffset - 14.0);
        p.drawLine(QPointF(x, tagY + 14.0), QPointF(x, y - 5.0));

        QPolygonF triangle;
        triangle << QPointF(x - 4.5, y - 5.0)
                 << QPointF(x + 4.5, y - 5.0)
                 << QPointF(x, y + 1.5);
        p.setBrush(color);
        p.drawPolygon(triangle);

        QFont tagFont = p.font();
        tagFont.setPointSizeF(7.0);
        tagFont.setBold(true);
        p.setFont(tagFont);
        const QRectF tagRect(x - 14.0, tagY, 28.0, 13.0);
        p.setPen(QPen(color, 1.0));
        p.setBrush(QColor(7, 11, 17, 225));
        p.drawRoundedRect(tagRect, 3.0, 3.0);
        p.drawText(tagRect, Qt::AlignCenter, QStringLiteral("M%1").arg(markerIndex + 1));
        p.setBrush(Qt::NoBrush);
    }
    p.restore();

    // Compact readout table. Small fixed columns prevent M1..M4 readouts from
    // colliding while still showing the selected trace source.
    int markerRows = 0;
    for (const auto& marker : markers_) markerRows += marker.enabled ? 1 : 0;
    if (markerRows > 0) {
        const QRectF box(r.left() + 8, r.top() + 8, 292, markerRows * 16.0 + 8.0);
        p.setPen(QPen(QColor(63, 77, 97), 1));
        p.setBrush(QColor(10, 16, 24, 220));
        p.drawRoundedRect(box, 4, 4);

        QFont readoutFont = p.font();
        readoutFont.setPointSizeF(7.5);
        p.setFont(readoutFont);

        int row = 0;
        for (int markerIndex = 0; markerIndex < 4; ++markerIndex) {
            const MarkerState& marker = markers_[markerIndex];
            if (!marker.enabled) continue;

            QString valueText = QStringLiteral("---");
            const QVector<float>& trace = markerTrace(marker);
            if (trace.size() >= 2 && sampleRateHz_ > 0.0) {
                const int pointCount = static_cast<int>(trace.size());
                const int bin = frequencyBin(
                    marker.frequencyHz, pointCount, centerHz_, sampleRateHz_);
                if (bin >= 0 && bin < pointCount)
                    valueText = QString::number(trace[bin], 'f', 1) + QStringLiteral(" dBFS");
            }

            const qreal y = box.top() + 4.0 + row * 16.0;
            p.setPen(markerColor(markerIndex));
            p.drawText(QRectF(box.left() + 6, y, 24, 16), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("M%1").arg(markerIndex + 1));
            p.setPen(QColor(200, 208, 211));
            p.drawText(QRectF(box.left() + 31, y, 32, 16), Qt::AlignLeft | Qt::AlignVCenter,
                       markerSourceText(marker.source));
            p.setPen(QColor(231, 235, 236));
            p.drawText(QRectF(box.left() + 64, y, 28, 16), Qt::AlignLeft | Qt::AlignVCenter,
                       channelNameByIndex(marker.channel));
            p.drawText(QRectF(box.left() + 94, y, 104, 16), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(marker.frequencyHz / 1.0e6, 'f', 3) + QStringLiteral(" MHz"));
            p.drawText(QRectF(box.left() + 204, y, 80, 16), Qt::AlignRight | Qt::AlignVCenter,
                       valueText);
            ++row;
        }
    }
}

WaterfallWidget::WaterfallWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(170);

    // Keep the presentation path consistent with SpectrumWidget.  The previous
    // WA_OpaquePaintEvent path was valid in principle, but on the target Qt /
    // desktop stack the widget backing store retained the parent stylesheet
    // background even though paintEvent() and drawImage() were executed.
    // Explicit QWidget painting plus a GUI-side QPixmap front buffer is more
    // robust and mirrors the proven presentation stage in the reference app.
    setAutoFillBackground(false);
}

void WaterfallWidget::resetRaster(int columns)
{
    columns = std::max(2, columns);
    rasterColumns_ = columns;
    newestRow_ = 0;
    validRows_ = 0;
    presentationPixmap_ = QPixmap();

    image_ = QImage(rasterColumns_, historyRows_, QImage::Format_RGB32);
    if (!image_.isNull()) image_.fill(QColor(13, 18, 20));

    dbRaster_.resize(rasterColumns_ * historyRows_);
    std::fill(dbRaster_.begin(), dbRaster_.end(),
              std::numeric_limits<float>::quiet_NaN());
}

void WaterfallWidget::appendSpectrum(const QVector<float>& db)
{
    const int binCount = static_cast<int>(db.size());
    if (binCount < 2) return;

    // Follow the proven raster strategy used by the reference project: reduce
    // the FFT to approximately one value per on-screen column and keep rows in
    // a circular image. This avoids allocating an FFT-width image and avoids a
    // full-image memmove for every display frame.
    const int plotColumns = std::clamp(
        static_cast<int>(std::lround(plotRect(this).width())), 128, 2048);
    const int columns = std::max(2, std::min(plotColumns, binCount));
    if (image_.isNull() || rasterColumns_ != columns
        || image_.height() != historyRows_
        || image_.format() != QImage::Format_RGB32) {
        resetRaster(columns);
    }
    if (image_.isNull() || rasterColumns_ <= 0) return;

    if (validRows_ == 0) {
        newestRow_ = 0;
    } else {
        newestRow_ = (newestRow_ - 1 + historyRows_) % historyRows_;
    }

    auto* pixels = reinterpret_cast<QRgb*>(image_.scanLine(newestRow_));
    if (!pixels) return;

    const float range = std::max(1.0f, ceilingDbfs_ - floorDbfs_);
    const int rowOffset = newestRow_ * rasterColumns_;
    for (int column = 0; column < rasterColumns_; ++column) {
        const int begin = (column * binCount) / rasterColumns_;
        const int end = std::min(binCount,
            std::max(begin + 1, ((column + 1) * binCount) / rasterColumns_));

        float peak = floorDbfs_;
        bool hasFinite = false;
        for (int bin = begin; bin < end; ++bin) {
            const float value = db[bin];
            if (!std::isfinite(value)) continue;
            peak = std::max(peak, value);
            hasFinite = true;
        }
        if (!hasFinite) peak = floorDbfs_;

        dbRaster_[rowOffset + column] = peak;
        pixels[column] = waterfallColor((peak - floorDbfs_) / range, palette_).rgb();
    }

    validRows_ = std::min(validRows_ + 1, historyRows_);
    rebuildPresentationPixmap();
    update();
}

void WaterfallWidget::setChannelLabel(const QString& label)
{
    channelLabel_ = label;
    update();
}

void WaterfallWidget::setLevelRange(float floorDbfs, float ceilingDbfs)
{
    if (ceilingDbfs <= floorDbfs + 1.0f) return;
    if (std::fabs(floorDbfs_ - floorDbfs) < 0.01f
        && std::fabs(ceilingDbfs_ - ceilingDbfs) < 0.01f) return;
    floorDbfs_ = floorDbfs;
    ceilingDbfs_ = ceilingDbfs;
    rebuildImage();
    rebuildPresentationPixmap();
    update();
}

void WaterfallWidget::setFrequencyRange(double centerHz, double sampleRateHz)
{
    centerHz_ = centerHz;
    sampleRateHz_ = sampleRateHz;
    update();
}

void WaterfallWidget::setSpanFraction(float fraction)
{
    fraction = std::clamp(fraction, 0.05f, 1.0f);
    if (std::fabs(spanFraction_ - fraction) < 0.0001f) return;
    spanFraction_ = fraction;
    rebuildPresentationPixmap();
    update();
}

void WaterfallWidget::setPalette(WaterfallPalette palette)
{
    if (palette_ == palette) return;
    palette_ = palette;
    rebuildImage();
    rebuildPresentationPixmap();
    update();
}

void WaterfallWidget::rebuildImage()
{
    if (image_.isNull() || rasterColumns_ <= 0
        || dbRaster_.size() != rasterColumns_ * historyRows_) return;

    image_.fill(QColor(13, 18, 20));
    const float range = std::max(1.0f, ceilingDbfs_ - floorDbfs_);
    for (int rowIndex = 0; rowIndex < historyRows_; ++rowIndex) {
        auto* pixels = reinterpret_cast<QRgb*>(image_.scanLine(rowIndex));
        if (!pixels) continue;
        const int offset = rowIndex * rasterColumns_;
        for (int column = 0; column < rasterColumns_; ++column) {
            const float value = dbRaster_[offset + column];
            if (!std::isfinite(value)) continue;
            pixels[column] = waterfallColor(
                (value - floorDbfs_) / range, palette_).rgb();
        }
    }
}


void WaterfallWidget::rebuildPresentationPixmap()
{
    presentationPixmap_ = QPixmap();
    if (image_.isNull() || rasterColumns_ <= 1 || validRows_ <= 0) return;

    const QRectF r = plotRect(this);
    const int targetWidth = std::max(1, static_cast<int>(std::lround(r.width())));
    const int targetHeight = std::max(1, static_cast<int>(std::lround(r.height())));
    if (targetWidth <= 1 || targetHeight <= 1) return;

    // Build a native logical-size front buffer off-screen.  QPainter on a
    // QImage uses the deterministic raster paint engine; the completed frame is
    // then converted to QPixmap on the GUI thread and paintEvent() only blits
    // that pixmap.  This avoids the target-system issue seen with scaling and
    // compositing the circular QImage directly into the QWidget backing store.
    QImage frame(targetWidth, targetHeight, QImage::Format_RGB32);
    if (frame.isNull()) return;
    frame.fill(QColor(13, 18, 20));

    const auto [firstColumn, lastColumn] =
        visibleBinRange(rasterColumns_, spanFraction_);
    const int sourceWidth = std::max(1, lastColumn - firstColumn);
    const qreal rowHeight = static_cast<qreal>(targetHeight)
        / static_cast<qreal>(historyRows_);

    QPainter fp(&frame);
    fp.setRenderHint(QPainter::SmoothPixmapTransform, false);

    // newestRow_ is chronological row 0 (Now).  Compose the circular image
    // into one contiguous front buffer before it reaches the QWidget.
    const int firstRows = std::min(validRows_, historyRows_ - newestRow_);
    if (firstRows > 0) {
        const QRectF source(firstColumn, newestRow_, sourceWidth, firstRows);
        const QRectF target(0.0, 0.0,
                            static_cast<qreal>(targetWidth),
                            rowHeight * firstRows);
        fp.drawImage(target, image_, source);
    }

    const int remainingRows = validRows_ - firstRows;
    if (remainingRows > 0) {
        const QRectF source(firstColumn, 0, sourceWidth, remainingRows);
        const QRectF target(0.0, rowHeight * firstRows,
                            static_cast<qreal>(targetWidth),
                            rowHeight * remainingRows);
        fp.drawImage(target, image_, source);
    }
    fp.end();

    presentationPixmap_ = QPixmap::fromImage(std::move(frame), Qt::NoFormatConversion);
}

void WaterfallWidget::clear()
{
    image_ = QImage();
    presentationPixmap_ = QPixmap();
    dbRaster_.clear();
    rasterColumns_ = 0;
    newestRow_ = 0;
    validRows_ = 0;
    update();
}

void WaterfallWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(25, 31, 34));

    // Keep exactly the same plot margins as SpectrumWidget so frequency bins
    // remain horizontally aligned between the two views.
    const QRectF r = plotRect(this);
    p.fillRect(r, QColor(13, 18, 20));

    p.setPen(QColor(181, 191, 194));
    p.drawText(QRectF(r.left() + 8, 6, 300, 20), Qt::AlignLeft | Qt::AlignVCenter,
               tr("WATERFALL  ·  %1").arg(channelLabel_));

    const QString badgeText = QStringLiteral("%1  %2…%3 dBFS")
        .arg(paletteText(palette_))
        .arg(ceilingDbfs_, 0, 'f', 0)
        .arg(floorDbfs_, 0, 'f', 0);
    const QRectF badge(r.right() - 190, 6, 182, 20);
    // IMPORTANT: keep the badge brush local.  In V3.2 the #364145 badge brush
    // leaked into the later drawRect(r) call; QPainter::drawRect() uses both
    // pen and brush, so that final border call repainted the entire waterfall
    // plot with the UI background and hid a correctly generated pixmap.
    p.save();
    p.setPen(QPen(QColor(94, 109, 114), 1));
    p.setBrush(QColor(54, 65, 69));
    p.drawRoundedRect(badge, 4, 4);
    p.setPen(QColor(214, 221, 223));
    p.drawText(badge, Qt::AlignCenter, badgeText);
    p.restore();

    const QSize expectedPixmapSize(
        std::max(1, static_cast<int>(std::lround(r.width()))),
        std::max(1, static_cast<int>(std::lround(r.height()))));
    if (!image_.isNull() && validRows_ > 0
        && (presentationPixmap_.isNull()
            || presentationPixmap_.size() != expectedPixmapSize)) {
        rebuildPresentationPixmap();
    }
    if (!presentationPixmap_.isNull()) {
        // Present a completed native-size pixmap in one blit.  This matches the
        // reference project's GUI presentation path and avoids direct QImage
        // scaling/compositing in the QWidget paint engine.
        p.drawPixmap(QPoint(static_cast<int>(std::lround(r.left())),
                            static_cast<int>(std::lround(r.top()))),
                     presentationPixmap_);
    }

    p.setPen(QColor(91, 106, 112));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);

    p.setPen(QColor(181, 191, 194));
    p.drawText(QRectF(18, r.top(), 40, 20), Qt::AlignRight, tr("Now"));
    p.drawText(QRectF(18, r.bottom() - 20, 40, 20), Qt::AlignRight, tr("Past"));

    const double displaySpanHz = sampleRateHz_ * static_cast<double>(spanFraction_);
    p.drawText(QRectF(r.left(), r.bottom() + 7, r.width(), 20), Qt::AlignLeft,
               mhz(centerHz_ - displaySpanHz / 2.0));
    p.drawText(QRectF(r.left(), r.bottom() + 7, r.width(), 20), Qt::AlignHCenter,
               mhz(centerHz_));
    p.drawText(QRectF(r.left(), r.bottom() + 7, r.width(), 20), Qt::AlignRight,
               mhz(centerHz_ + displaySpanHz / 2.0));
}


void WaterfallWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!image_.isNull() && validRows_ > 0) {
        rebuildPresentationPixmap();
    }
}

ConstellationPlaceholderWidget::ConstellationPlaceholderWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(240);
}

void ConstellationPlaceholderWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(25, 31, 34));
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = plotRect(this);
    drawGrid(p, r);

    // I/Q axes make the reserved region visually match the future
    // constellation view without introducing any demodulation or DSP path.
    p.save();
    p.setClipRect(r.adjusted(1, 1, -1, -1));
    p.setPen(QPen(QColor(105, 122, 128), 1.0));
    p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
    p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
    p.restore();

    p.setPen(QColor(181, 191, 194));
    p.drawText(QRectF(r.left() + 8, 6, 340, 20),
               Qt::AlignLeft | Qt::AlignVCenter,
               tr("CONSTELLATION  ·  RESERVED"));

    p.setPen(QColor(132, 148, 153));
    p.drawText(r, Qt::AlignCenter,
               tr("IQ constellation / demodulation view reserved"));

    p.setPen(QColor(181, 191, 194));
    p.drawText(QRectF(r.right() - 28, r.center().y() + 5, 24, 18),
               Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("I"));
    p.drawText(QRectF(r.center().x() + 6, r.top() + 2, 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Q"));
}

WaveformWidget::WaveformWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(300);
}

void WaveformWidget::setSamples(const QVector<float>& i, const QVector<float>& q,
                                double sampleRateHz, const QString& channelLabel)
{
    i_ = i;
    q_ = q;
    sampleRateHz_ = sampleRateHz;
    channelLabel_ = channelLabel;

    float peak = 0.0f;
    const int count = std::min(i_.size(), q_.size());
    for (int k = 0; k < count; ++k) {
        peak = std::max(peak, std::fabs(i_[k]));
        peak = std::max(peak, std::fabs(q_[k]));
    }
    // Auto-ranging prevents a valid low-level signal from looking flat while
    // retaining a stable minimum scale around the zero line.
    displayRange_ = std::clamp(peak * 1.15f, 0.02f, 1.0f);
    update();
}

void WaveformWidget::clear()
{
    i_.clear();
    q_.clear();
    displayRange_ = 1.0f;
    update();
}

void WaveformWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(25, 31, 34));
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = plotRect(this);
    drawGrid(p, r);

    p.setPen(QColor(181, 191, 194));
    p.drawText(QRectF(r.left() + 8, 6, 260, 20), Qt::AlignLeft | Qt::AlignVCenter,
               tr("TIME DOMAIN  ·  %1  ·  AUTO ±%2")
                   .arg(channelLabel_)
                   .arg(displayRange_, 0, 'f', displayRange_ < 0.1f ? 3 : 2));
    p.drawText(QRectF(10, r.top() - 9, 48, 18), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("+%1").arg(displayRange_, 0, 'f', displayRange_ < 0.1f ? 3 : 2));
    p.drawText(QRectF(18, r.center().y() - 10, 40, 20), Qt::AlignRight, QStringLiteral("0"));
    p.drawText(QRectF(10, r.bottom() - 9, 48, 18), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("-%1").arg(displayRange_, 0, 'f', displayRange_ < 0.1f ? 3 : 2));
    if (i_.size() < 2 || q_.size() != i_.size()) return;

    auto makeLine = [&](const QVector<float>& values) {
        QPolygonF line;
        const int pointCount = static_cast<int>(values.size());
        const int target = std::clamp(static_cast<int>(r.width() * 1.4), 240, 1600);
        const int step = std::max(1, (pointCount + target - 1) / target);
        line.reserve((pointCount + step - 1) / step);
        for (int k = 0; k < pointCount; k += step) {
            const qreal x = r.left() + r.width() * k / (pointCount - 1.0);
            const float normalized = std::clamp(values[k] / std::max(0.001f, displayRange_),
                                                -1.0f, 1.0f);
            const qreal y = r.center().y() - normalized * r.height() * 0.45;
            line << QPointF(x, y);
        }
        return line;
    };

    p.save();
    p.setClipRect(r.adjusted(1, 1, -1, -1));
    p.setPen(QPen(QColor(255, 213, 79), 1.0));
    p.drawPolyline(makeLine(i_));
    p.setPen(QPen(QColor(79, 195, 247), 1.0));
    p.drawPolyline(makeLine(q_));
    p.restore();

    const double us = i_.size() / std::max(1.0, sampleRateHz_) * 1.0e6;
    p.setPen(QColor(181, 191, 194));
    p.drawText(QRectF(r.left(), r.bottom() + 7, r.width(), 20), Qt::AlignRight,
               QString::number(us, 'f', 2) + QStringLiteral(" us"));

    p.setPen(QPen(QColor(255, 213, 79), 1.5));
    p.drawLine(QPointF(r.right() - 88, r.top() + 14), QPointF(r.right() - 68, r.top() + 14));
    p.setPen(QColor(231, 235, 236));
    p.drawText(QRectF(r.right() - 62, r.top() + 5, 20, 18), Qt::AlignLeft, QStringLiteral("I"));
    p.setPen(QPen(QColor(79, 195, 247), 1.5));
    p.drawLine(QPointF(r.right() - 38, r.top() + 14), QPointF(r.right() - 18, r.top() + 14));
    p.setPen(QColor(231, 235, 236));
    p.drawText(QRectF(r.right() - 12, r.top() + 5, 12, 18), Qt::AlignLeft, QStringLiteral("Q"));
}
