#include "main_window.hpp"

#include "plot_widgets.hpp"
#include "rx_worker.hpp"
#include "ui_main_window.h"


#include <QAbstractItemView>
#include <QApplication>
#include <QByteArray>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QList>
#include <QMessageBox>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QRegularExpression>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QSpacerItem>
#include <QScreen>
#include <QFontMetrics>
#include <QStandardItemModel>
#include <QThread>
#include <QSysInfo>
#include <QTimer>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
struct Mode { const char* text; std::array<bool, 8> enabled; };

QVector<Mode> modesFor(LwModel model)
{
    auto mode = [](const char* text, std::initializer_list<int> indices) {
        Mode out{text, {false, false, false, false, false, false, false, false}};
        for (int i : indices) out.enabled[i] = true;
        return out;
    };

    if (model == LwModel::LW3920) {
        return {mode("A1", {0}), mode("A2", {1}), mode("A1 + A2", {0, 1})};
    }
    if (model == LwModel::LW3940) {
        return {mode("A1", {0}), mode("A2", {1}), mode("B1", {2}), mode("B2", {3}),
                mode("A1 + A2", {0, 1}), mode("B1 + B2", {2, 3}),
                mode("A1 + B1", {0, 2}), mode("A1 + B2", {0, 3}),
                mode("A1 + A2 + B1 + B2", {0, 1, 2, 3})};
    }
    return {mode("A1", {0}), mode("A2", {1}), mode("B1", {2}), mode("B2", {3}),
            mode("C1", {4}), mode("C2", {5}), mode("D1", {6}), mode("D2", {7}),
            mode("A1 + A2", {0, 1}), mode("B1 + B2", {2, 3}),
            mode("A1 + B1", {0, 2}), mode("A1 + B2", {0, 3}),
            mode("C1 + C2", {4, 5}), mode("D1 + D2", {6, 7}),
            mode("A1 + A2 + B1 + B2", {0, 1, 2, 3}),
            mode("A1 + A2 + C1 + C2", {0, 1, 4, 5}),
            mode("A1 + A2 + D1 + D2", {0, 1, 6, 7}),
            mode("B1 + B2 + C1 + C2", {2, 3, 4, 5}),
            mode("B1 + B2 + D1 + D2", {2, 3, 6, 7}),
            mode("A1 + A2 + B1 + B2 + C1 + C2 + D1 + D2", {0, 1, 2, 3, 4, 5, 6, 7})};
}

LwModel modelFromIndex(int index)
{
    if (index == 0) return LwModel::LW3920;
    if (index == 2) return LwModel::LW3980;
    return LwModel::LW3940;
}

QString selectedChannelsText(const std::array<bool, 8>& enabled)
{
    QStringList names;
    for (int ch = 0; ch < 8; ++ch) {
        if (enabled[ch]) names << channelName(static_cast<LwChannel>(ch));
    }
    return names.join(QStringLiteral(" + "));
}

constexpr quint64 kMiB = 1024ULL * 1024ULL;
constexpr quint64 kGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr quint64 kMinIqBufferBytes = 256ULL * kMiB;
constexpr quint64 kMaxIqBufferBytes = 16ULL * kGiB;
constexpr double kIqBufferSafeFraction = 0.25;
constexpr double kIqBufferReserveFraction = 0.15;
constexpr quint64 kIqBufferReserveFloorBytes = 2ULL * kGiB;
constexpr double kIqBufferAutoTargetSeconds = 2.0;
constexpr double kPcieContinuousSafetyFraction = 0.85;

struct HostMemoryBudget
{
    quint64 hostAvailable = 0;
    quint64 cgroupAvailable = std::numeric_limits<quint64>::max();
    quint64 effectiveAvailable = 0;
    quint64 safeIqBufferLimit = 0;
};

quint64 readMemAvailableBytes()
{
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray& line : lines) {
        if (!line.startsWith("MemAvailable:")) continue;
        const QList<QByteArray> parts = line.mid(sizeof("MemAvailable:") - 1).simplified().split(' ');
        if (parts.isEmpty()) return 0;
        bool ok = false;
        const quint64 kb = parts[0].toULongLong(&ok);
        return ok ? kb * 1024ULL : 0;
    }
    return 0;
}

quint64 readCgroupValue(const QString& path, bool& finite)
{
    finite = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const QByteArray text = file.readAll().trimmed();
    if (text == "max" || text.isEmpty()) return 0;
    bool ok = false;
    const quint64 value = text.toULongLong(&ok);
    finite = ok;
    return ok ? value : 0;
}

HostMemoryBudget detectHostMemoryBudget()
{
    HostMemoryBudget out;
    out.hostAvailable = readMemAvailableBytes();

    bool limitFinite = false;
    bool currentFinite = false;
    const quint64 cgLimit = readCgroupValue(QStringLiteral("/sys/fs/cgroup/memory.max"), limitFinite);
    const quint64 cgCurrent = readCgroupValue(QStringLiteral("/sys/fs/cgroup/memory.current"), currentFinite);
    if (limitFinite) {
        const quint64 current = currentFinite ? cgCurrent : 0;
        out.cgroupAvailable = cgLimit > current ? cgLimit - current : 0;
    }

    if (out.hostAvailable == 0) {
        out.effectiveAvailable = out.cgroupAvailable == std::numeric_limits<quint64>::max()
            ? 0 : out.cgroupAvailable;
    } else if (out.cgroupAvailable == std::numeric_limits<quint64>::max()) {
        out.effectiveAvailable = out.hostAvailable;
    } else {
        out.effectiveAvailable = std::min(out.hostAvailable, out.cgroupAvailable);
    }

    if (out.effectiveAvailable > 0) {
        const quint64 byFraction = static_cast<quint64>(
            static_cast<long double>(out.effectiveAvailable) * kIqBufferSafeFraction);
        const quint64 reserve = std::max<quint64>(
            kIqBufferReserveFloorBytes,
            static_cast<quint64>(static_cast<long double>(out.effectiveAvailable)
                                 * kIqBufferReserveFraction));
        const quint64 byReserve = out.effectiveAvailable > reserve
            ? out.effectiveAvailable - reserve : 0;
        out.safeIqBufferLimit = std::min({kMaxIqBufferBytes, byFraction, byReserve});
    }
    return out;
}

QString compactBytes(quint64 bytes)
{
    if (bytes >= kGiB)
        return QStringLiteral("%1 GiB").arg(bytes / static_cast<double>(kGiB), 0, 'f', bytes % kGiB ? 1 : 0);
    return QStringLiteral("%1 MiB").arg(bytes / static_cast<double>(kMiB), 0, 'f', 0);
}

quint64 expectedIqBytesPerSecond(const RxConfig& cfg)
{
    return static_cast<quint64>(std::max<long>(0, cfg.sampleRateHz))
        * static_cast<quint64>(std::max(0, enabledChannelCount(cfg)))
        * 4ULL;
}

quint64 chooseAutoIqBufferBytes(quint64 expectedBytesPerSecond, quint64 safeLimit)
{
    static constexpr std::array<quint64, 8> kTiers = {
        256ULL * kMiB, 512ULL * kMiB, 1ULL * kGiB,
        2ULL * kGiB, 4ULL * kGiB, 8ULL * kGiB,
        12ULL * kGiB, 16ULL * kGiB
    };
    if (safeLimit < kMinIqBufferBytes) return 0;

    const long double target = static_cast<long double>(expectedBytesPerSecond)
        * kIqBufferAutoTargetSeconds;
    quint64 largestSafe = 0;
    for (quint64 tier : kTiers) {
        if (tier > safeLimit) break;
        largestSafe = tier;
        if (static_cast<long double>(tier) >= target) return tier;
    }
    return largestSafe;
}

struct PcieLinkBudget
{
    bool valid = false;
    bool ambiguous = false;
    int candidateCount = 0;
    QString bdf;
    double gtps = 0.0;
    int width = 0;
    double theoreticalMiBps = 0.0;
    double recommendedMiBps = 0.0;
};

QString readSmallTextFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    return QString::fromLatin1(file.readAll()).trimmed();
}

PcieLinkBudget detectLwPcieBudget()
{
    PcieLinkBudget best;
    QDir pci(QStringLiteral("/sys/bus/pci/devices"));
    const QStringList entries = pci.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& bdf : entries) {
        const QString base = pci.filePath(bdf);
        const QString vendor = readSmallTextFile(base + QStringLiteral("/vendor")).toLower();
        if (vendor != QStringLiteral("0x10ee")) continue; // Xilinx / AMD FPGA

        const QString speedText = readSmallTextFile(base + QStringLiteral("/current_link_speed"));
        const QString widthText = readSmallTextFile(base + QStringLiteral("/current_link_width"));
        if (speedText.isEmpty() || widthText.isEmpty()) continue;

        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("([0-9]+(?:\\.[0-9]+)?)"))
            .match(speedText);
        bool widthOk = false;
        const int width = widthText.toInt(&widthOk);
        if (!m.hasMatch() || !widthOk || width <= 0) continue;
        bool speedOk = false;
        const double gtps = m.captured(1).toDouble(&speedOk);
        if (!speedOk || gtps <= 0.0) continue;

        // PCIe Gen1/2 use 8b/10b; Gen3+ use 128b/130b. This is still a
        // line-rate estimate, so continuous no-drop operation uses an 85%
        // engineering budget to leave room for TLP/DLLP/flow-control overhead.
        const double coding = gtps >= 8.0 ? (128.0 / 130.0) : 0.8;
        const double bytesPerSecond = gtps * 1.0e9 * static_cast<double>(width)
            * coding / 8.0;
        PcieLinkBudget candidate;
        candidate.valid = true;
        candidate.bdf = bdf;
        candidate.gtps = gtps;
        candidate.width = width;
        candidate.theoreticalMiBps = bytesPerSecond / 1048576.0;
        candidate.recommendedMiBps = candidate.theoreticalMiBps * kPcieContinuousSafetyFraction;

        ++best.candidateCount;
        // Keep the best link only for diagnostics. If more than one Xilinx
        // endpoint exists, pcie_0 cannot be mapped to a BDF safely with the
        // current SDK, so validation must not hard-block on a guessed card.
        if (!best.valid || candidate.theoreticalMiBps > best.theoreticalMiBps) {
            const int count = best.candidateCount;
            best = candidate;
            best.candidateCount = count;
        }
    }
    best.ambiguous = best.candidateCount > 1;
    return best;
}

QString logDirectoryForIqPath(const QString& iqPath)
{
    const QString normalized = iqPath.trimmed().isEmpty()
        ? QStringLiteral("rx_iq.bin") : iqPath.trimmed();
    const QFileInfo info(normalized);
    return QDir(info.absolutePath()).filePath(QStringLiteral("log"));
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    QSettings settings;
    const int storedLanguage = std::clamp(settings.value(QStringLiteral("ui/language"), 0).toInt(), 0, 2);
    language_ = static_cast<AppLanguage>(storedLanguage);
    translator_.setLanguage(language_);
    QCoreApplication::installTranslator(&translator_);

    ui->setupUi(this);
    {
        const QSignalBlocker blocker(ui->languageComboBox);
        ui->languageComboBox->setCurrentIndex(storedLanguage);
    }

    qRegisterMetaType<DisplayFrame>("DisplayFrame");
    initializeSessionLog(ui->filePathEdit->text().trimmed());

    setupAdaptiveContainers();
    polishUi();
    buildPlots();
    setupStatusBar();
    wireUi();
    updateChannelModes();
    refreshIqBufferOptions();
    updateConnectionUi();
    setState(State::Idle);
    retranslateDynamicUi();
    QTimer::singleShot(0, this, &MainWindow::updateResponsiveLayout);

    appendLog(tr("[系统] 就绪 · V4.0"));
    appendLog(tr("[系统] 设备接口：PCIe / pcie_0"));
    if (!sessionLogPath_.isEmpty())
        appendLog(tr("[系统] 运行日志：%1").arg(sessionLogPath_));
    else
        appendLog(tr("[警告] 运行日志文件创建失败；本次仅保留界面日志"));
    appendDeveloperLog(tr("[APP] Qt=%1 | OS=%2 | CPU=%3 | build=%4 %5")
        .arg(QString::fromLatin1(qVersion()))
        .arg(QSysInfo::prettyProductName())
        .arg(QSysInfo::currentCpuArchitecture())
        .arg(QString::fromLatin1(__DATE__))
        .arg(QString::fromLatin1(__TIME__)));
}

MainWindow::~MainWindow()
{
    if (!shutdownLogged_) {
        writeLogLine(tr("[系统] 程序退出"), false);
        shutdownLogged_ = true;
    }
    if (sessionLogFile_.isOpen()) {
        sessionLogFile_.flush();
        sessionLogFile_.close();
    }
    delete ui;
    ui = nullptr;
    QCoreApplication::removeTranslator(&translator_);
}

void MainWindow::setupAdaptiveContainers()
{
    // Left-side configuration can become taller in English/Russian and on
    // 1366x768 or high-DPI displays. Put it in a vertical scroll area so no
    // control is clipped even when the available screen height is small.
    auto* leftContent = new QWidget(ui->leftPanel);
    leftContent->setObjectName(QStringLiteral("leftScrollContent"));
    auto* contentLayout = new QVBoxLayout(leftContent);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(7);

    while (ui->leftLayout->count() > 0) {
        QLayoutItem* item = ui->leftLayout->takeAt(0);
        if (!item) break;
        if (QWidget* widget = item->widget()) {
            widget->setParent(leftContent);
            contentLayout->addWidget(widget);
            delete item;
        } else if (QSpacerItem* spacer = item->spacerItem()) {
            contentLayout->addItem(spacer);
        } else if (QLayout* layout = item->layout()) {
            contentLayout->addLayout(layout);
        } else {
            delete item;
        }
    }

    leftScrollArea_ = new QScrollArea(ui->leftPanel);
    leftScrollArea_->setObjectName(QStringLiteral("leftScrollArea"));
    leftScrollArea_->setWidgetResizable(true);
    leftScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    leftScrollArea_->setFrameShape(QFrame::NoFrame);
    leftScrollArea_->setStyleSheet(QStringLiteral(
        "QScrollArea#leftScrollArea { background:transparent; border:0; }"
        "QWidget#leftScrollContent { background:#303B40; }"));
    leftScrollArea_->setWidget(leftContent);
    ui->leftLayout->addWidget(leftScrollArea_);

    // The spectrum control rows are intentionally feature-dense. On a narrow
    // screen or with longer Russian labels, use a horizontal scroll fallback
    // instead of clipping labels or forcing widgets to overlap.
    ui->spectrumTabLayout->removeWidget(ui->spectrumToolFrame);
    spectrumToolScrollArea_ = new QScrollArea(ui->spectrumTab);
    spectrumToolScrollArea_->setObjectName(QStringLiteral("spectrumToolScrollArea"));
    spectrumToolScrollArea_->setWidgetResizable(true);
    spectrumToolScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    spectrumToolScrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    spectrumToolScrollArea_->setFrameShape(QFrame::NoFrame);
    spectrumToolScrollArea_->setStyleSheet(QStringLiteral(
        "QScrollArea#spectrumToolScrollArea { background:transparent; border:0; }"));
    spectrumToolScrollArea_->setWidget(ui->spectrumToolFrame);
    ui->spectrumTabLayout->insertWidget(0, spectrumToolScrollArea_);
}

void MainWindow::polishUi()
{
    setWindowTitle(tr("LuoWave · LW39X0 Signal Capture"));
    // Bound the on-screen log so a long capture cannot make the GUI document
    // grow indefinitely. The complete developer log is persisted to disk.
    ui->logEdit->setMaximumBlockCount(3000);

    // Never request a window larger than the active screen. Qt 6 uses logical
    // pixels, so this also behaves correctly with 125/150/200% DPI scaling.
    const QRect available = QApplication::primaryScreen()
        ? QApplication::primaryScreen()->availableGeometry()
        : QRect(0, 0, 1600, 900);
    setMinimumSize(std::min(860, available.width()), std::min(500, available.height()));
    const int initialWidth = std::max(860, static_cast<int>(available.width() * 0.96));
    const int initialHeight = std::max(500, static_cast<int>(available.height() * 0.94));
    resize(std::min(initialWidth, available.width()),
           std::min(initialHeight, available.height()));

    ui->mainSplitter->setChildrenCollapsible(false);
    ui->mainSplitter->setStretchFactor(0, 0);
    ui->mainSplitter->setStretchFactor(1, 1);
    ui->mainSplitter->setStretchFactor(2, 0);

    // QComboBox popup views are separate windows on Linux. Styling only the
    // combobox itself may therefore leave the unselected popup area white.
    // Apply the dark popup style directly to every view as well.
    const QString popupStyle = QStringLiteral(
        "QAbstractItemView {"
        " background:#3A474D; color:#F0F4F5;"
        " border:1px solid #708289; outline:0; padding:4px;"
        " selection-background-color:#596D75; selection-color:#FFFFFF;"
        " }"
        "QAbstractItemView::item { min-height:26px; padding:3px 7px; }"
        "QAbstractItemView::item:disabled { background:#354045; color:#748287; }"
        "QAbstractItemView::item:hover { background:#46565D; color:#FFFFFF; }"
    );
    for (QComboBox* combo : findChildren<QComboBox*>()) {
        combo->setMaxVisibleItems(14);
        combo->view()->setStyleSheet(popupStyle);
        combo->view()->setMinimumWidth(std::max(combo->width(), 180));
    }
    ui->channelModeComboBox->view()->setMinimumWidth(330);

    // IQ buffer choices use exact byte values as item data. Index 0 is Auto.
    const std::array<quint64, 9> iqBufferValues = {
        0ULL, 256ULL * kMiB, 512ULL * kMiB, 1ULL * kGiB,
        2ULL * kGiB, 4ULL * kGiB, 8ULL * kGiB,
        12ULL * kGiB, 16ULL * kGiB
    };
    for (int i = 0; i < ui->iqBufferComboBox->count() && i < static_cast<int>(iqBufferValues.size()); ++i)
        ui->iqBufferComboBox->setItemData(i, QVariant::fromValue<qulonglong>(iqBufferValues[static_cast<std::size_t>(i)]));

    ui->averageButton->setToolTip(tr("Base Trace 的指数平均模式；平均强度由“平均 α”调节"));
    ui->averageAlphaSpinBox->setToolTip(tr("指数平均系数 α：越小越平滑、响应越慢；越大越接近实时。1.00 等效为不平均。"));
    ui->spectrumFloorSpinBox->setToolTip(tr("频谱纵轴显示下限"));
    ui->spectrumCeilingSpinBox->setToolTip(tr("频谱纵轴显示上限"));
    ui->spectrumSpanComboBox->setToolTip(tr("仅调整频谱/瀑布可视频率跨度，不改变设备采样率或保存数据"));
    ui->maxHoldButton->setToolTip(tr("独立 Max Hold Trace：直接对实时 FFT 做峰值保持，不经过 Average；因此正常情况下可高于平均谱线"));
    ui->minHoldButton->setToolTip(tr("独立 Min Hold Trace：直接对实时 FFT 做最小值保持，不经过 Average；因此与 Base Trace 独立"));
    ui->maxHoldChannelComboBox->setToolTip(tr("选择 Max Hold 跟踪的 RX 通道"));
    ui->minHoldChannelComboBox->setToolTip(tr("选择 Min Hold 跟踪的 RX 通道"));
    ui->clearHoldButton->setToolTip(tr("清除 Max Hold / Min Hold 历史"));
    ui->markerNumberComboBox->setToolTip(tr("选择当前要设置的 Marker，最多 4 个"));
    ui->markerSourceComboBox->setToolTip(tr("Marker 数据源：当前 Trace / Max Hold / Min Hold"));
    ui->markerChannelComboBox->setToolTip(tr("Trace 模式可选通道；选择 Max/Min Hold 时自动跟随对应 Hold 通道"));
    ui->markerEnableButton->setToolTip(tr("开启当前 Marker；开启后可在频谱中左键点击或拖动定位"));
    ui->clearMarkersButton->setToolTip(tr("关闭并清除全部 Marker"));
    ui->waterfallFloorSpinBox->setToolTip(tr("瀑布图颜色映射下限（噪声底附近）"));
    ui->waterfallCeilingSpinBox->setToolTip(tr("瀑布图颜色映射上限（强信号附近）"));
    ui->waterfallPaletteComboBox->setToolTip(tr("瀑布图配色：Inferno / Jet / White Hot / Black Hot"));
    ui->screenshotButton->setToolTip(tr("保存当前频谱/瀑布页为 PNG"));

    // Branding is embedded for release reliability. An external asset beside
    // the executable remains an optional override for field/custom builds.
    updateCompanyLogo();
}

void MainWindow::updateCompanyLogo()
{
    if (!ui || !ui->companyLogoLabel) return;

    static const QPixmap logo = [] {
        const QString externalPath = QCoreApplication::applicationDirPath()
            + QStringLiteral("/assets/company_logo.png");
        QPixmap p(externalPath);
        if (p.isNull())
            p.load(QStringLiteral(":/ui/company_logo.png"));
        return p;
    }();

    if (logo.isNull()) {
        ui->companyLogoLabel->setPixmap(QPixmap());
        ui->companyLogoLabel->setText(QStringLiteral("LUOWAVE"));
        return;
    }

    const int targetWidth = std::max(96, ui->companyLogoLabel->width() - 14);
    const int targetHeight = std::max(34, ui->companyLogoLabel->height() - 8);
    ui->companyLogoLabel->setText(QString());
    ui->companyLogoLabel->setPixmap(logo.scaled(
        targetWidth, targetHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void MainWindow::applyLanguage(AppLanguage language)
{
    language_ = language;
    translator_.setLanguage(language_);

    // ui_main_window.h uses QCoreApplication::translate(), so retranslateUi()
    // immediately applies the custom runtime translator without .qm files.
    ui->retranslateUi(this);
    {
        const QSignalBlocker blocker(ui->languageComboBox);
        ui->languageComboBox->setCurrentIndex(static_cast<int>(language_));
    }
    QSettings().setValue(QStringLiteral("ui/language"), static_cast<int>(language_));

    retranslateDynamicUi();
    updateResponsiveLayout();
    if (spectrum_) spectrum_->update();
    if (waterfall_) waterfall_->update();
    if (constellation_) constellation_->update();
    if (waveform_) waveform_->update();
}

void MainWindow::retranslateDynamicUi()
{
    setWindowTitle(tr("LuoWave · LW39X0 Signal Capture"));

    ui->averageButton->setToolTip(tr("Base Trace 的指数平均模式；平均强度由“平均 α”调节"));
    ui->averageAlphaSpinBox->setToolTip(tr("指数平均系数 α：越小越平滑、响应越慢；越大越接近实时。1.00 等效为不平均。"));
    ui->spectrumFloorSpinBox->setToolTip(tr("频谱纵轴显示下限"));
    ui->spectrumCeilingSpinBox->setToolTip(tr("频谱纵轴显示上限"));
    ui->spectrumSpanComboBox->setToolTip(tr("仅调整频谱/瀑布可视频率跨度，不改变设备采样率或保存数据"));
    ui->maxHoldButton->setToolTip(tr("独立 Max Hold Trace：直接对实时 FFT 做峰值保持，不经过 Average；因此正常情况下可高于平均谱线"));
    ui->minHoldButton->setToolTip(tr("独立 Min Hold Trace：直接对实时 FFT 做最小值保持，不经过 Average；因此与 Base Trace 独立"));
    ui->maxHoldChannelComboBox->setToolTip(tr("选择 Max Hold 跟踪的 RX 通道"));
    ui->minHoldChannelComboBox->setToolTip(tr("选择 Min Hold 跟踪的 RX 通道"));
    ui->clearHoldButton->setToolTip(tr("清除 Max Hold / Min Hold 历史"));
    ui->markerNumberComboBox->setToolTip(tr("选择当前要设置的 Marker，最多 4 个"));
    ui->markerSourceComboBox->setToolTip(tr("Marker 数据源：当前 Trace / Max Hold / Min Hold"));
    ui->markerChannelComboBox->setToolTip(tr("Trace 模式可选通道；选择 Max/Min Hold 时自动跟随对应 Hold 通道"));
    ui->markerEnableButton->setToolTip(tr("开启当前 Marker；开启后可在频谱中左键点击或拖动定位"));
    ui->clearMarkersButton->setToolTip(tr("关闭并清除全部 Marker"));
    ui->waterfallFloorSpinBox->setToolTip(tr("瀑布图颜色映射下限（噪声底附近）"));
    ui->waterfallCeilingSpinBox->setToolTip(tr("瀑布图颜色映射上限（强信号附近）"));
    ui->waterfallPaletteComboBox->setToolTip(tr("瀑布图配色：Inferno / Jet / White Hot / Black Hot"));
    ui->screenshotButton->setToolTip(tr("保存当前频谱/瀑布页为 PNG"));

    refreshIqBufferOptions();
    syncMarkerControls();
    updateConnectionUi();
    setState(state_);

    if (state_ == State::Running) {
        ui->connectionStatusValueLabel->setText(tr("● 已连接 / RX"));
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color:#38d67a;font-weight:700;"));
        ui->statusBar->showMessage(tr("● RX 流式采集运行中"));
    }
}

void MainWindow::updateResponsiveLayout()
{
    if (!ui || !ui->mainSplitter) return;

    const int w = std::max(860, width());
    const int h = std::max(500, height());
    const bool compact = (w < 1400 || h < 800);
    const bool veryCompact = (w < 1180 || h < 700);

    // Side panels have flexible limits; the center plot keeps the remaining
    // space. Form layouts wrap long translated labels rather than clipping.
    ui->leftPanel->setMinimumWidth(veryCompact ? 210 : (compact ? 225 : 250));
    ui->leftPanel->setMaximumWidth(compact ? 330 : 380);
    ui->rightPanel->setMinimumWidth(veryCompact ? 220 : (compact ? 235 : 270));
    ui->rightPanel->setMaximumWidth(compact ? 340 : 390);
    ui->centerPanel->setMinimumWidth(veryCompact ? 340 : (compact ? 420 : 560));

    ui->rxForm->setRowWrapPolicy((compact || language_ == AppLanguage::Russian)
        ? QFormLayout::WrapLongRows : QFormLayout::DontWrapRows);
    ui->infoForm->setRowWrapPolicy((veryCompact || language_ == AppLanguage::Russian)
        ? QFormLayout::WrapLongRows : QFormLayout::DontWrapRows);

    const int splitterWidth = std::max(800, ui->mainSplitter->width());
    const int leftPreferred = std::clamp(static_cast<int>(splitterWidth * (compact ? 0.21 : 0.18)),
                                         ui->leftPanel->minimumWidth(), ui->leftPanel->maximumWidth());
    const int rightPreferred = std::clamp(static_cast<int>(splitterWidth * (compact ? 0.22 : 0.19)),
                                          ui->rightPanel->minimumWidth(), ui->rightPanel->maximumWidth());
    const int centerPreferred = std::max(ui->centerPanel->minimumWidth(),
                                         splitterWidth - leftPreferred - rightPreferred - 8);
    ui->mainSplitter->setSizes({leftPreferred, centerPreferred, rightPreferred});

    const int logoHeight = veryCompact ? 50 : (compact ? 58 : 70);
    ui->companyLogoLabel->setMinimumHeight(logoHeight);
    ui->companyLogoLabel->setMaximumHeight(logoHeight);
    updateCompanyLogo();

    // Decorative text yields space first on small screens; all functional
    // controls remain accessible.
    ui->appSubTitleLabel->setVisible(w >= 1180);
    ui->driverBadgeLabel->setVisible(w >= 1040);

    if (spectrumToolScrollArea_) {
        const int natural = std::max(1040, ui->spectrumToolFrame->sizeHint().width());
        ui->spectrumToolFrame->setMinimumWidth(natural);
        spectrumToolScrollArea_->setMinimumHeight(116);
        spectrumToolScrollArea_->setMaximumHeight(134);
    }

    // Popup lists are independent top-level windows on XCB. Size them from the
    // translated text so English/Russian items are not truncated.
    for (QComboBox* combo : findChildren<QComboBox*>()) {
        QFontMetrics fm(combo->font());
        int textWidth = 0;
        for (int i = 0; i < combo->count(); ++i)
            textWidth = std::max(textWidth, fm.horizontalAdvance(combo->itemText(i)));
        const int popupWidth = std::clamp(textWidth + 54, 140, 520);
        combo->view()->setMinimumWidth(std::max(combo->width(), popupWidth));
    }
    ui->channelModeComboBox->view()->setMinimumWidth(std::max(330, ui->channelModeComboBox->view()->minimumWidth()));
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (!ui || !event) return;
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
        retranslateDynamicUi();
        updateResponsiveLayout();
    } else if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange) {
        updateResponsiveLayout();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // Coalesce resize storms while dragging a window edge. This matters on
    // lower-end hosts where repeated font/combobox measurements are not free.
    if (responsiveLayoutUpdatePending_) return;
    responsiveLayoutUpdatePending_ = true;
    QTimer::singleShot(0, this, [this] {
        responsiveLayoutUpdatePending_ = false;
        updateResponsiveLayout();
    });
}

void MainWindow::buildPlots()
{
    // Item 0 is the compact spectrum toolbar; keep it fixed-height.
    // The actual spectrum and waterfall retain the 3:2 vertical ratio.
    ui->spectrumTabLayout->setStretch(0, 0);
    ui->spectrumTabLayout->setStretch(1, 3);
    ui->spectrumTabLayout->setStretch(2, 2);
    ui->rightLayout->setStretch(0, 0);
    ui->rightLayout->setStretch(1, 1);
    // Reserve the upper half of the second display tab for a future
    // constellation/demodulation view.  No acquisition or DSP path is changed.
    ui->timeTabLayout->setStretch(0, 1);
    ui->timeTabLayout->setStretch(1, 1);

    spectrum_ = new SpectrumWidget(ui->spectrumFrame);
    waterfall_ = new WaterfallWidget(ui->waterfallFrame);
    constellation_ = new ConstellationPlaceholderWidget(ui->constellationFrame);
    waveform_ = new WaveformWidget(ui->timeFrame);
    ui->spectrumLayout->addWidget(spectrum_);
    ui->waterfallLayout->addWidget(waterfall_);
    ui->constellationLayout->addWidget(constellation_);
    ui->timeLayout->addWidget(waveform_);

    spectrum_->setTraceMode(SpectrumTraceMode::Average);
    spectrum_->setAverageAlpha(static_cast<float>(ui->averageAlphaSpinBox->value()));
    spectrum_->setLevelRange(static_cast<float>(ui->spectrumFloorSpinBox->value()),
                             static_cast<float>(ui->spectrumCeilingSpinBox->value()));
    spectrum_->setSpanFraction(1.0f);
    waterfall_->setSpanFraction(1.0f);
    waterfall_->setPalette(WaterfallPalette::Inferno);
    updateWaterfallLevels();
}

void MainWindow::setupStatusBar()
{
    footerDeviceLabel_ = new QLabel(tr("设备  未连接"), this);
    footerStreamLabel_ = new QLabel(tr("采集  空闲"), this);
    footerRateLabel_ = new QLabel(tr("RX  0.0 MiB/s  |  0.0 MiB  |  0.0 s"), this);

    footerDeviceLabel_->setObjectName(QStringLiteral("footerDeviceLabel"));
    footerStreamLabel_->setObjectName(QStringLiteral("footerStreamLabel"));
    footerRateLabel_->setObjectName(QStringLiteral("footerRateLabel"));
    footerDeviceLabel_->setMinimumWidth(150);
    footerStreamLabel_->setMinimumWidth(105);
    footerRateLabel_->setMinimumWidth(260);
    footerRateLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    ui->statusBar->addPermanentWidget(footerDeviceLabel_);
    ui->statusBar->addPermanentWidget(footerStreamLabel_);
    ui->statusBar->addPermanentWidget(footerRateLabel_);
    ui->statusBar->showMessage(tr("● 等待设备连接"));
}

void MainWindow::wireUi()
{
    connect(ui->languageComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        index = std::clamp(index, 0, 2);
        applyLanguage(static_cast<AppLanguage>(index));
    });

    // RealTime and Average are mutually exclusive base traces. Max/Min Hold are
    // independent overlays, so both may be active at the same time.
    auto* traceModeGroup = new QButtonGroup(this);
    traceModeGroup->setExclusive(true);
    traceModeGroup->addButton(ui->realTimeButton);
    traceModeGroup->addButton(ui->averageButton);

    connect(ui->realTimeButton, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) spectrum_->setTraceMode(SpectrumTraceMode::RealTime);
    });
    connect(ui->averageButton, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) spectrum_->setTraceMode(SpectrumTraceMode::Average);
    });
    connect(ui->averageAlphaSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double alpha) {
        spectrum_->setAverageAlpha(static_cast<float>(alpha));
    });

    connect(ui->spectrumFloorSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double floor) {
        if (floor >= ui->spectrumCeilingSpinBox->value() - 5.0)
            ui->spectrumCeilingSpinBox->setValue(std::min(20.0, floor + 5.0));
        spectrum_->setLevelRange(static_cast<float>(ui->spectrumFloorSpinBox->value()),
                                 static_cast<float>(ui->spectrumCeilingSpinBox->value()));
    });
    connect(ui->spectrumCeilingSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double ceiling) {
        if (ceiling <= ui->spectrumFloorSpinBox->value() + 5.0)
            ui->spectrumFloorSpinBox->setValue(std::max(-180.0, ceiling - 5.0));
        spectrum_->setLevelRange(static_cast<float>(ui->spectrumFloorSpinBox->value()),
                                 static_cast<float>(ui->spectrumCeilingSpinBox->value()));
    });
    connect(ui->spectrumSpanComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        float fraction = 1.0f;
        if (index == 1) fraction = 0.5f;
        else if (index == 2) fraction = 0.25f;
        else if (index == 3) fraction = 0.10f;
        spectrum_->setSpanFraction(fraction);
        waterfall_->setSpanFraction(fraction);
    });

    connect(ui->maxHoldButton, &QPushButton::toggled, this, [this](bool checked) {
        spectrum_->setMaxHoldEnabled(checked);
        appendLog(checked
            ? tr("[频谱] Max Hold Trace 开启：%1 | 数据源：实时 FFT（独立于 Base Average/Realtime）")
                .arg(ui->maxHoldChannelComboBox->currentText())
            : tr("[频谱] Max Hold Trace 关闭"));
    });
    connect(ui->minHoldButton, &QPushButton::toggled, this, [this](bool checked) {
        spectrum_->setMinHoldEnabled(checked);
        appendLog(checked
            ? tr("[频谱] Min Hold Trace 开启：%1 | 数据源：实时 FFT（独立于 Base Average/Realtime）")
                .arg(ui->minHoldChannelComboBox->currentText())
            : tr("[频谱] Min Hold Trace 关闭"));
    });
    connect(ui->maxHoldChannelComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (ui->maxHoldChannelComboBox->currentIndex() >= 0) {
            spectrum_->setMaxHoldChannel(ui->maxHoldChannelComboBox->currentData().toInt());
            syncMarkerControls();
        }
    });
    connect(ui->minHoldChannelComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (ui->minHoldChannelComboBox->currentIndex() >= 0) {
            spectrum_->setMinHoldChannel(ui->minHoldChannelComboBox->currentData().toInt());
            syncMarkerControls();
        }
    });
    connect(ui->clearHoldButton, &QPushButton::clicked, this, [this] {
        spectrum_->clearHolds();
        appendLog(tr("[频谱] Hold 历史已清除"));
    });

    connect(ui->markerNumberComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int marker) {
        spectrum_->setActiveMarker(marker);
        syncMarkerControls();
    });
    connect(ui->markerSourceComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        const int marker = ui->markerNumberComboBox->currentIndex();
        MarkerTraceSource source = MarkerTraceSource::BaseTrace;
        if (index == 1) {
            source = MarkerTraceSource::MaxHold;
            if (!ui->maxHoldButton->isChecked()) ui->maxHoldButton->setChecked(true);
        }
        if (index == 2) {
            source = MarkerTraceSource::MinHold;
            if (!ui->minHoldButton->isChecked()) ui->minHoldButton->setChecked(true);
        }
        spectrum_->setMarkerSource(marker, source);
        syncMarkerControls();
    });
    connect(ui->markerChannelComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (ui->markerChannelComboBox->currentIndex() < 0) return;
        const int marker = ui->markerNumberComboBox->currentIndex();
        if (spectrum_->markerSource(marker) == MarkerTraceSource::BaseTrace)
            spectrum_->setMarkerChannel(marker, ui->markerChannelComboBox->currentData().toInt());
    });
    connect(ui->markerEnableButton, &QPushButton::toggled, this, [this](bool checked) {
        const int marker = ui->markerNumberComboBox->currentIndex();
        spectrum_->setActiveMarker(marker);
        spectrum_->setMarkerEnabled(marker, checked);
        ui->markerEnableButton->setText(checked ? tr("已开启") : tr("开启"));
    });
    connect(ui->clearMarkersButton, &QPushButton::clicked, this, [this] {
        spectrum_->clearMarkers();
        syncMarkerControls();
        appendLog(tr("[Marker] 已全部清除"));
    });

    connect(ui->screenshotButton, &QPushButton::clicked,
            this, &MainWindow::saveSpectrumScreenshot);

    connect(ui->waterfallFloorSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double floor) {
        if (floor >= ui->waterfallCeilingSpinBox->value() - 5.0)
            ui->waterfallCeilingSpinBox->setValue(std::min(20.0, floor + 5.0));
        updateWaterfallLevels();
    });
    connect(ui->waterfallCeilingSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double ceiling) {
        if (ceiling <= ui->waterfallFloorSpinBox->value() + 5.0)
            ui->waterfallFloorSpinBox->setValue(std::max(-180.0, ceiling - 5.0));
        updateWaterfallLevels();
    });
    connect(ui->waterfallPaletteComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        switch (index) {
        case 1: waterfall_->setPalette(WaterfallPalette::Jet); break;
        case 2: waterfall_->setPalette(WaterfallPalette::WhiteHot); break;
        case 3: waterfall_->setPalette(WaterfallPalette::BlackHot); break;
        default: waterfall_->setPalette(WaterfallPalette::Inferno); break;
        }
    });

    connect(ui->modelComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { updateChannelModes(); refreshIqBufferOptions(); });
    connect(ui->channelModeComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { updateDisplayChannels(); refreshIqBufferOptions(); });
    connect(ui->sampleRateComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { refreshIqBufferOptions(); });
    connect(ui->iqBufferComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { refreshIqBufferOptions(); });

    connect(ui->connectDeviceButton, &QPushButton::clicked,
            this, &MainWindow::connectDevice);
    connect(ui->disconnectDeviceButton, &QPushButton::clicked,
            this, &MainWindow::disconnectDevice);

    connect(ui->browseButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("保存 IQ 数据"), ui->filePathEdit->text(),
            tr("Binary IQ (*.bin);;All files (*)"));
        if (!path.isEmpty()) ui->filePathEdit->setText(path);
    });
    connect(ui->saveIqCheckBox, &QCheckBox::toggled,
            ui->filePathEdit, &QWidget::setEnabled);
    connect(ui->saveIqCheckBox, &QCheckBox::toggled,
            ui->browseButton, &QWidget::setEnabled);
    connect(ui->saveIqCheckBox, &QCheckBox::toggled,
            this, [this] { refreshIqBufferOptions(); });
    connect(ui->startButton, &QPushButton::clicked,
            this, &MainWindow::startCapture);
    connect(ui->stopButton, &QPushButton::clicked,
            this, &MainWindow::stopCapture);
}

void MainWindow::updateChannelModes()
{
    const auto modes = modesFor(modelFromIndex(ui->modelComboBox->currentIndex()));
    ui->channelModeComboBox->clear();
    for (const auto& mode : modes)
        ui->channelModeComboBox->addItem(QString::fromLatin1(mode.text));

    const QString wanted = ui->modelComboBox->currentIndex() == 0
        ? QStringLiteral("A1 + A2")
        : ui->modelComboBox->currentIndex() == 1
            ? QStringLiteral("A1 + A2 + B1 + B2")
            : QStringLiteral("A1 + A2 + B1 + B2 + C1 + C2 + D1 + D2");
    const int index = ui->channelModeComboBox->findText(wanted);
    if (index >= 0) ui->channelModeComboBox->setCurrentIndex(index);
    updateDisplayChannels();
}

void MainWindow::updateDisplayChannels()
{
    const auto modes = modesFor(modelFromIndex(ui->modelComboBox->currentIndex()));
    const int index = ui->channelModeComboBox->currentIndex();
    if (index < 0 || index >= modes.size()) return;

    const auto enabled = modes[index].enabled;
    int firstEnabled = 0;
    for (int ch = 0; ch < 8; ++ch) {
        if (enabled[ch]) { firstEnabled = ch; break; }
    }

    auto refill = [&](QComboBox* combo, int preferredChannel) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (int ch = 0; ch < 8; ++ch) {
            if (enabled[ch])
                combo->addItem(channelName(static_cast<LwChannel>(ch)), ch);
        }
        int restore = combo->findData(preferredChannel);
        if (restore < 0) restore = combo->findData(firstEnabled);
        if (restore >= 0) combo->setCurrentIndex(restore);
    };

    const int displayPrevious = ui->displayChannelComboBox->currentIndex() >= 0
        ? ui->displayChannelComboBox->currentData().toInt() : firstEnabled;
    refill(ui->displayChannelComboBox, displayPrevious);
    refill(ui->maxHoldChannelComboBox, spectrum_->maxHoldChannel());
    refill(ui->minHoldChannelComboBox, spectrum_->minHoldChannel());

    if (ui->maxHoldChannelComboBox->currentIndex() >= 0)
        spectrum_->setMaxHoldChannel(ui->maxHoldChannelComboBox->currentData().toInt());
    if (ui->minHoldChannelComboBox->currentIndex() >= 0)
        spectrum_->setMinHoldChannel(ui->minHoldChannelComboBox->currentData().toInt());

    // Keep every stored marker attached to an enabled channel when the user
    // changes device model or channel combination.
    for (int marker = 0; marker < 4; ++marker) {
        if (spectrum_->markerSource(marker) != MarkerTraceSource::BaseTrace) continue;
        const int oldChannel = spectrum_->markerChannel(marker);
        if (oldChannel < 0 || oldChannel >= 8 || !enabled[oldChannel])
            spectrum_->setMarkerChannel(marker, firstEnabled);
    }

    refill(ui->markerChannelComboBox,
           spectrum_->markerChannel(ui->markerNumberComboBox->currentIndex()));
    syncMarkerControls();
}

void MainWindow::refreshIqBufferOptions()
{
    const HostMemoryBudget budget = detectHostMemoryBudget();
    const quint64 safeLimit = budget.safeIqBufferLimit;

    auto* model = qobject_cast<QStandardItemModel*>(ui->iqBufferComboBox->model());
    if (model) {
        for (int i = 0; i < ui->iqBufferComboBox->count(); ++i) {
            QStandardItem* item = model->item(i);
            if (!item) continue;
            const quint64 bytes = ui->iqBufferComboBox->itemData(i).toULongLong();
            const bool autoItem = (i == 0 || bytes == 0);
            const bool allowed = autoItem ? safeLimit >= kMinIqBufferBytes : bytes <= safeLimit;
            item->setEnabled(allowed);
            item->setToolTip(allowed
                ? (autoItem ? tr("按当前数据率与主机内存自动选择约 2 秒的安全缓存档位")
                            : tr("当前主机允许该缓存档位"))
                : tr("当前主机安全内存上限为 %1，此档位已禁用").arg(compactBytes(safeLimit)));
        }
    }

    // If a previously selected manual tier is no longer safe because available
    // memory changed, move the UI back to Auto before the next Start.
    const quint64 currentBytes = ui->iqBufferComboBox->currentData().toULongLong();
    if (ui->iqBufferComboBox->currentIndex() > 0 && currentBytes > safeLimit
        && state_ == State::Idle) {
        const QSignalBlocker blocker(ui->iqBufferComboBox);
        ui->iqBufferComboBox->setCurrentIndex(0);
    }

    RxConfig preview;
    preview.model = modelFromIndex(ui->modelComboBox->currentIndex());
    preview.sampleRateHz = static_cast<long>(
        ui->sampleRateComboBox->currentText().toDouble() * 1.0e6 + 0.5);
    const auto modes = modesFor(preview.model);
    const int modeIndex = ui->channelModeComboBox->currentIndex();
    if (modeIndex >= 0 && modeIndex < modes.size()) preview.enabled = modes[modeIndex].enabled;
    const quint64 expected = expectedIqBytesPerSecond(preview);

    const bool isAuto = ui->iqBufferComboBox->currentIndex() == 0;
    quint64 selected = ui->iqBufferComboBox->currentData().toULongLong();
    if (isAuto) selected = chooseAutoIqBufferBytes(expected, safeLimit);

    QString status;
    if (safeLimit < kMinIqBufferBytes) {
        status = tr("可用内存 %1 · 不足以安全分配 256 MiB，IQ 保存已禁用")
            .arg(compactBytes(budget.effectiveAvailable));
    } else {
        const double seconds = expected > 0
            ? static_cast<double>(selected) / static_cast<double>(expected) : 0.0;
        status = tr("可用内存 %1 · 内存安全上限 %2 · %3%4")
            .arg(compactBytes(budget.effectiveAvailable))
            .arg(compactBytes(safeLimit))
            .arg(isAuto ? tr("自动→") : QString())
            .arg(compactBytes(selected));
        if (expected > 0)
            status += tr("（当前配置约 %1 s）").arg(seconds, 0, 'f', 2);
    }
    ui->iqBufferStatusLabel->setText(status);

    if (state_ == State::Idle) {
        const bool configurable = deviceConnected_;
        const bool available = safeLimit >= kMinIqBufferBytes;
        ui->saveIqCheckBox->setEnabled(configurable && available);
        ui->iqBufferComboBox->setEnabled(configurable && available && ui->saveIqCheckBox->isChecked());
    }
}

void MainWindow::syncMarkerControls()
{
    const int marker = std::clamp(ui->markerNumberComboBox->currentIndex(), 0, 3);
    spectrum_->setActiveMarker(marker);

    const MarkerTraceSource source = spectrum_->markerSource(marker);
    int sourceIndex = 0;
    if (source == MarkerTraceSource::MaxHold) sourceIndex = 1;
    if (source == MarkerTraceSource::MinHold) sourceIndex = 2;
    {
        const QSignalBlocker blocker(ui->markerSourceComboBox);
        ui->markerSourceComboBox->setCurrentIndex(sourceIndex);
    }

    const int channel = spectrum_->markerChannel(marker);
    {
        const QSignalBlocker blocker(ui->markerChannelComboBox);
        const int index = ui->markerChannelComboBox->findData(channel);
        if (index >= 0) ui->markerChannelComboBox->setCurrentIndex(index);
    }
    ui->markerChannelComboBox->setEnabled(source == MarkerTraceSource::BaseTrace);

    const bool enabled = spectrum_->markerEnabled(marker);
    {
        const QSignalBlocker blocker(ui->markerEnableButton);
        ui->markerEnableButton->setChecked(enabled);
    }
    ui->markerEnableButton->setText(enabled ? tr("已开启") : tr("开启"));
}

void MainWindow::updateConnectionUi()
{
    if (deviceConnected_) {
        const QString model = modelName(connectedModel_);
        ui->connectionStatusValueLabel->setText(tr("● 已就绪"));
        ui->connectionStatusValueLabel->setStyleSheet(
            QStringLiteral("color:#38d67a;font-weight:700;"));
        ui->deviceModelInfoValueLabel->setText(model);
        ui->connectionInfoValueLabel->setText(tr("PCIe / pcie_0（Start连接）"));
        footerDeviceLabel_->setText(tr("设备  %1 · 已就绪").arg(model));
        footerDeviceLabel_->setStyleSheet(QStringLiteral("color:#62df95;font-weight:600;"));
        ui->statusBar->showMessage(tr("● 设备参数已就绪，等待 Start"));
    } else {
        ui->connectionStatusValueLabel->setText(tr("● 未连接"));
        ui->connectionStatusValueLabel->setStyleSheet(
            QStringLiteral("color:#f0b94b;font-weight:700;"));
        ui->deviceModelInfoValueLabel->setText(QStringLiteral("--"));
        ui->connectionInfoValueLabel->setText(QStringLiteral("--"));
        footerDeviceLabel_->setText(tr("设备  未连接"));
        footerDeviceLabel_->setStyleSheet(QStringLiteral("color:#f0b94b;font-weight:600;"));
        ui->statusBar->showMessage(tr("● 等待设备连接"));
    }
}

void MainWindow::setState(State state)
{
    state_ = state;
    const bool idle = state == State::Idle;
    const bool running = state == State::Running;
    const bool configurable = idle && deviceConnected_;

    // Model is a product capability selection. Current SDK context info prints
    // version information but does not expose a programmatic model query, so
    // lock the user's model choice for the duration of the logical connection.
    ui->modelComboBox->setEnabled(idle && !deviceConnected_);
    ui->connectDeviceButton->setEnabled(idle && !deviceConnected_);
    ui->disconnectDeviceButton->setEnabled(idle && deviceConnected_);

    ui->centerFrequencySpinBox->setEnabled(configurable);
    ui->sampleRateComboBox->setEnabled(configurable);
    ui->gainSpinBox->setEnabled(configurable);
    ui->channelModeComboBox->setEnabled(configurable);
    ui->displayChannelComboBox->setEnabled(configurable);
    ui->fftSizeComboBox->setEnabled(configurable);
    ui->captureDurationSpinBox->setEnabled(configurable);
    const HostMemoryBudget memBudget = detectHostMemoryBudget();
    const bool iqBufferAvailable = memBudget.safeIqBufferLimit >= kMinIqBufferBytes;
    ui->saveIqCheckBox->setEnabled(configurable && iqBufferAvailable);
    ui->iqBufferComboBox->setEnabled(configurable && ui->saveIqCheckBox->isChecked() && iqBufferAvailable);
    ui->filePathEdit->setEnabled(configurable && iqBufferAvailable && ui->saveIqCheckBox->isChecked());
    ui->browseButton->setEnabled(configurable && iqBufferAvailable && ui->saveIqCheckBox->isChecked());
    ui->startButton->setEnabled(configurable);
    ui->stopButton->setEnabled(running || state == State::Starting);

    QString text;
    QString color;
    if (idle) {
        text = tr("空闲");
        color = QStringLiteral("#8fd6ff");
        footerStreamLabel_->setText(tr("采集  空闲"));
        footerStreamLabel_->setStyleSheet(QStringLiteral("color:#9fb1c8;"));
    } else if (state == State::Starting) {
        text = tr("启动中");
        color = QStringLiteral("#f0b94b");
        footerStreamLabel_->setText(tr("采集  启动中"));
        footerStreamLabel_->setStyleSheet(QStringLiteral("color:#f0b94b;font-weight:600;"));
    } else if (running) {
        text = tr("采集中");
        color = QStringLiteral("#38d67a");
        footerStreamLabel_->setText(tr("采集  ● RUN"));
        footerStreamLabel_->setStyleSheet(QStringLiteral("color:#38d67a;font-weight:700;"));
    } else {
        text = tr("停止中…");
        color = QStringLiteral("#f0b94b");
        footerStreamLabel_->setText(tr("采集  停止中"));
        footerStreamLabel_->setStyleSheet(QStringLiteral("color:#f0b94b;font-weight:600;"));
    }

    ui->statusValueLabel->setText(text);
    ui->statusValueLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:700;").arg(color));
    ui->captureStatusInfoValueLabel->setText(text);
    ui->captureStatusInfoValueLabel->setStyleSheet(
        QStringLiteral("color:%1;font-weight:700;").arg(color));
}

void MainWindow::initializeSessionLog(const QString& iqPath)
{
    const QString logDirPath = logDirectoryForIqPath(iqPath);
    QDir logDir;
    if (!logDir.mkpath(logDirPath)) return;

    QDir dir(logDirPath);
    const QFileInfoList existing = dir.entryInfoList(
        {QStringLiteral("lw_signal_capture_*.log")},
        QDir::Files | QDir::Readable, QDir::Time);
    // Keep the latest 50 sessions beside the IQ files. Field service can copy
    // one capture directory and obtain both raw data and its diagnostics.
    for (int i = 50; i < existing.size(); ++i)
        QFile::remove(existing[i].absoluteFilePath());

    const QString stamp = QDateTime::currentDateTime()
        .toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    sessionLogPath_ = dir.filePath(
        QStringLiteral("lw_signal_capture_%1.log").arg(stamp));
    sessionLogFile_.setFileName(sessionLogPath_);
    if (!sessionLogFile_.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sessionLogPath_.clear();
        return;
    }

    const QByteArray header = QStringLiteral(
        "# LuoWave LW39X0 Signal Capture V4.0\n"
        "# Full session log: customer-visible events + [DEV] diagnostics\n"
        "# Stored in <IQ directory>/log for capture-by-capture field analysis\n")
        .toUtf8();
    sessionLogFile_.write(header);
    sessionLogFile_.flush();
}

void MainWindow::relocateSessionLogForIqPath(const QString& iqPath)
{
    const QString targetDirPath = logDirectoryForIqPath(iqPath);
    if (targetDirPath.isEmpty()) return;
    QDir().mkpath(targetDirPath);
    {
        QDir targetDir(targetDirPath);
        const QFileInfoList existing = targetDir.entryInfoList(
            {QStringLiteral("lw_signal_capture_*.log")},
            QDir::Files | QDir::Readable, QDir::Time);
        for (int i = 50; i < existing.size(); ++i)
            QFile::remove(existing[i].absoluteFilePath());
    }

    const QFileInfo currentInfo(sessionLogPath_);
    if (!sessionLogPath_.isEmpty()
        && QDir::cleanPath(currentInfo.absolutePath()) == QDir::cleanPath(targetDirPath))
        return;

    const QString oldPath = sessionLogPath_;
    QString baseName = currentInfo.fileName();
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("lw_signal_capture_%1.log")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    }
    const QString targetPath = QDir(targetDirPath).filePath(baseName);

    if (sessionLogFile_.isOpen()) {
        sessionLogFile_.flush();
        sessionLogFile_.close();
    }

    bool moved = oldPath.isEmpty();
    if (!oldPath.isEmpty() && QFileInfo::exists(oldPath)) {
        moved = QFile::rename(oldPath, targetPath);
        if (!moved) {
            moved = QFile::copy(oldPath, targetPath);
            if (moved) QFile::remove(oldPath);
        }
    }

    sessionLogPath_ = targetPath;
    sessionLogFile_.setFileName(sessionLogPath_);
    if (!sessionLogFile_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        sessionLogPath_.clear();
        return;
    }
    if (!moved && !oldPath.isEmpty()) {
        const QString line = QStringLiteral("[%1] %2\n")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                 tr("[系统] 日志目录切换；旧日志保留于 %1").arg(oldPath));
        sessionLogFile_.write(line.toUtf8());
        sessionLogFile_.flush();
    }
}

void MainWindow::writeLogLine(const QString& text, bool showInUi)
{
    const QDateTime now = QDateTime::currentDateTime();
    if (showInUi && ui) {
        ui->logEdit->appendPlainText(QStringLiteral("[%1] %2")
            .arg(now.toString(QStringLiteral("HH:mm:ss.zzz")), text));
    }

    if (sessionLogFile_.isOpen()) {
        const QString line = QStringLiteral("[%1] %2\n")
            .arg(now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")), text);
        sessionLogFile_.write(line.toUtf8());
        // Flush each event so the last useful lines survive an abnormal process
        // exit. Log traffic is tiny relative to the raw-IQ stream.
        sessionLogFile_.flush();
    }
}

void MainWindow::appendLog(const QString& text)
{
    writeLogLine(text, true);
}

void MainWindow::appendDeveloperLog(const QString& text)
{
    writeLogLine(QStringLiteral("[DEV] %1").arg(text), false);
}

void MainWindow::updateWaterfallLevels()
{
    if (!waterfall_) return;
    const float floor = static_cast<float>(ui->waterfallFloorSpinBox->value());
    const float ceiling = static_cast<float>(ui->waterfallCeilingSpinBox->value());
    waterfall_->setLevelRange(floor, ceiling);
}

void MainWindow::saveSpectrumScreenshot()
{
    const QString suggested = QDir::homePath() + QStringLiteral("/LW_spectrum_")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))
        + QStringLiteral(".png");

    QString path = QFileDialog::getSaveFileName(
        this, tr("保存频谱截图"), suggested, tr("PNG image (*.png)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");

    const QPixmap shot = ui->spectrumTab->grab();
    if (shot.save(path, "PNG")) {
        appendLog(tr("[截图] 已保存：%1").arg(path));
        ui->statusBar->showMessage(tr("● 截图已保存"), 3000);
    } else {
        QMessageBox::warning(this, tr("截图失败"), tr("无法写入文件：\n%1").arg(path));
    }
}

RxConfig MainWindow::currentConfig() const
{
    RxConfig cfg;
    cfg.model = modelFromIndex(ui->modelComboBox->currentIndex());
    cfg.centerFrequencyHz = ui->centerFrequencySpinBox->value() * 1.0e6;
    cfg.sampleRateHz = static_cast<long>(
        ui->sampleRateComboBox->currentText().toDouble() * 1.0e6 + 0.5);
    cfg.gainDb = ui->gainSpinBox->value();
    cfg.fftSize = ui->fftSizeComboBox->currentText().toInt();
    cfg.captureDurationSeconds = ui->captureDurationSpinBox->value();
    cfg.saveIq = ui->saveIqCheckBox->isChecked();
    cfg.iqFilePath = ui->filePathEdit->text().trimmed();

    const auto modes = modesFor(cfg.model);
    const int modeIndex = ui->channelModeComboBox->currentIndex();
    if (modeIndex >= 0 && modeIndex < modes.size()) cfg.enabled = modes[modeIndex].enabled;
    cfg.displayChannel = static_cast<LwChannel>(
        ui->displayChannelComboBox->currentData().toInt());

    const quint64 selectedBuffer = ui->iqBufferComboBox->currentData().toULongLong();
    cfg.iqBufferAuto = (ui->iqBufferComboBox->currentIndex() == 0 || selectedBuffer == 0);
    if (cfg.saveIq) {
        const HostMemoryBudget budget = detectHostMemoryBudget();
        const quint64 expected = expectedIqBytesPerSecond(cfg);
        cfg.iqBufferBytes = static_cast<std::size_t>(cfg.iqBufferAuto
            ? chooseAutoIqBufferBytes(expected, budget.safeIqBufferLimit)
            : selectedBuffer);
    }
    return cfg;
}

bool MainWindow::validateConfig(const RxConfig& cfg, QString& reason) const
{
    if (!deviceConnected_) {
        reason = tr("请先连接设备");
        return false;
    }
    if (cfg.model != connectedModel_) {
        reason = tr("当前配置型号与已连接型号不一致，请断开后重新选择型号");
        return false;
    }
    if (cfg.centerFrequencyHz < 60e6 || cfg.centerFrequencyHz > 6e9) {
        reason = tr("中心频率必须在 60 MHz ~ 6000 MHz 范围内");
        return false;
    }
    if (cfg.gainDb < 0.0 || cfg.gainDb > 54.0) {
        reason = tr("RX 增益必须在 0 ~ 54 dB 范围内");
        return false;
    }
    if (enabledChannelCount(cfg) == 0 || !channelEnabled(cfg, cfg.displayChannel)) {
        reason = tr("时域/瀑布通道必须属于当前已使能通道");
        return false;
    }
    if (cfg.sampleRateHz <= 0) {
        reason = tr("采样率无效");
        return false;
    }
    if (cfg.captureDurationSeconds < 0.0 || cfg.captureDurationSeconds > 86400.0) {
        reason = tr("采集时间必须为 0（连续）或 0.001 ~ 86400 s");
        return false;
    }
    const double divisor = 245.76e6 / static_cast<double>(cfg.sampleRateHz);
    if (std::fabs(divisor - std::round(divisor)) > 1e-6 || divisor < 1.0 || divisor > 1000.0) {
        reason = tr("采样率必须是 245.76 MS/s 的整数分频值");
        return false;
    }
    if (cfg.saveIq && cfg.iqFilePath.isEmpty()) {
        reason = tr("已启用 IQ 保存，但未设置文件路径");
        return false;
    }
    if (cfg.saveIq) {
        const HostMemoryBudget budget = detectHostMemoryBudget();
        if (budget.safeIqBufferLimit < kMinIqBufferBytes) {
            reason = tr("当前主机可用内存不足，无法安全分配最小 256 MiB IQ 写入缓存");
            return false;
        }
        if (cfg.iqBufferBytes < kMinIqBufferBytes) {
            reason = tr("IQ 写入缓存配置无效；请重新选择缓存档位");
            return false;
        }
        if (static_cast<quint64>(cfg.iqBufferBytes) > budget.safeIqBufferLimit) {
            reason = tr("IQ 写入缓存 %1 超过当前主机安全上限 %2；可用内存已变化，请降低缓存档位")
                .arg(compactBytes(static_cast<quint64>(cfg.iqBufferBytes)))
                .arg(compactBytes(budget.safeIqBufferLimit));
            return false;
        }
    }
    const PcieLinkBudget pcie = detectLwPcieBudget();
    if (pcie.valid && !pcie.ambiguous) {
        const double requiredMiBps = expectedIqBytesPerSecond(cfg) / 1048576.0;
        if (requiredMiBps > pcie.recommendedMiBps) {
            reason = tr("当前 RX 配置需要约 %1 MiB/s，已超过检测到的 FPGA PCIe 链路连续采集建议上限 %2 MiB/s（%3 GT/s ×%4，理论约 %5 MiB/s）。为避免掉点，请降低通道数或采样率")
                .arg(requiredMiBps, 0, 'f', 1)
                .arg(pcie.recommendedMiBps, 0, 'f', 1)
                .arg(pcie.gtps, 0, 'f', 1)
                .arg(pcie.width)
                .arg(pcie.theoreticalMiBps, 0, 'f', 1);
            return false;
        }
    }
    if (cfg.frameBytes % (32 * 1024) != 0) {
        reason = tr("RX frameBytes 必须是 32 KiB 的整数倍");
        return false;
    }
    return true;
}

void MainWindow::connectDevice()
{
    if (state_ != State::Idle || deviceConnected_ || rxThread_) return;

    // The current SDK has no side-effect-free device enumeration/probe API.
    // Earlier versions called make/show/destroy here and then created another
    // context on Start. The vendor recv_demo does not do that; it creates one
    // context immediately before configuration/receive. Keep Connect as a
    // logical "device selected / ready" action and let Start own the complete
    // hardware session from lw39x0_make() to lw39x0_destroy().
    connectedModel_ = modelFromIndex(ui->modelComboBox->currentIndex());
    deviceConnected_ = true;
    updateConnectionUi();
    setState(State::Idle);

    appendLog(tr("[设备] 已选择：%1 | PCIe | pcie_0").arg(modelName(connectedModel_)));
}

void MainWindow::disconnectDevice()
{
    if (state_ != State::Idle || rxThread_ || !deviceConnected_) return;

    appendLog(tr("[设备] 已断开"));
    deviceConnected_ = false;
    updateConnectionUi();
    setState(State::Idle);
}

void MainWindow::startCapture()
{
    if (state_ != State::Idle || rxThread_ || !deviceConnected_) return;

    RxConfig cfg = currentConfig();
    // Keep the complete developer log beside the configured IQ file. If the
    // user browsed to another capture directory since launch, move this session
    // log before the run starts so data and diagnostics stay together.
    const QString oldLogPath = sessionLogPath_;
    relocateSessionLogForIqPath(cfg.iqFilePath);
    if (!sessionLogPath_.isEmpty() && sessionLogPath_ != oldLogPath)
        appendLog(tr("[系统] 运行日志：%1").arg(sessionLogPath_));
    else if (sessionLogPath_.isEmpty())
        appendLog(tr("[警告] 运行日志文件不可用；本次仅保留界面日志"));
    QString reason;
    if (!validateConfig(cfg, reason)) {
        appendLog(tr("[错误] 启动前检查未通过：%1").arg(reason));
        QMessageBox::warning(this, tr("参数错误"), reason);
        return;
    }
    cfg.captureRunId = ++captureRunSequence_;

    setState(State::Starting);
    deviceReleaseConfirmed_ = false;
    ui->totalDataValueLabel->setText(QStringLiteral("0 MiB"));
    ui->throughputValueLabel->setText(QStringLiteral("0 MiB/s"));
    ui->elapsedValueLabel->setText(QStringLiteral("0.000 s"));
    ui->activeChannelValueLabel->setText(selectedChannelsText(cfg.enabled));
    ui->displayChannelValueLabel->setText(channelName(cfg.displayChannel));
    ui->frequencyValueLabel->setText(
        QString::number(cfg.centerFrequencyHz / 1.0e6, 'f', 3) + QStringLiteral(" MHz"));
    ui->sampleRateValueLabel->setText(
        QString::number(cfg.sampleRateHz / 1.0e6, 'f', 3) + QStringLiteral(" MS/s"));
    footerRateLabel_->setText(tr("RX  0.0 MiB/s  |  0.0 MiB  |  0.0 s"));

    spectrum_->clear();
    waterfall_->clear();
    waveform_->clear();
    waterfall_->setChannelLabel(channelName(cfg.displayChannel));
    waterfall_->setFrequencyRange(cfg.centerFrequencyHz, cfg.sampleRateHz);
    updateWaterfallLevels();

    // The RX worker owns the device session. Preview FFT/time-domain work and
    // optional file writes are handled asynchronously inside the worker so UI
    // load and storage latency do not sit on the lw39x0_recv() hot path.
    rxThread_ = new QThread(this);
    rxWorker_ = new RxWorker(cfg);
    rxWorker_->moveToThread(rxThread_);

    connect(rxThread_, &QThread::started, rxWorker_, &RxWorker::run);
    connect(rxWorker_, &RxWorker::started, this, [this] {
        if (state_ == State::Stopping) return;
        setState(State::Running);
        ui->connectionStatusValueLabel->setText(tr("● 已连接 / RX"));
        ui->connectionStatusValueLabel->setStyleSheet(
            QStringLiteral("color:#38d67a;font-weight:700;"));
        ui->statusBar->showMessage(tr("● RX 流式采集运行中"));
    });
    connect(rxWorker_, &RxWorker::deviceReleased, this, [this] {
        deviceReleaseConfirmed_ = true;
        appendLog(tr("[设备] RX 资源已释放"));
        appendDeveloperLog(tr("[GUI] deviceReleased signal received"));
        ui->statusBar->showMessage(tr("● RX 已停止，正在完成剩余处理"));
    });
    connect(rxWorker_, &RxWorker::logMessage, this, &MainWindow::appendLog);
    connect(rxWorker_, &RxWorker::diagnosticMessage,
            this, &MainWindow::appendDeveloperLog, Qt::QueuedConnection);
    connect(rxWorker_, &RxWorker::errorOccurred, this, [this, runId = cfg.captureRunId](const QString& message) {
        appendDeveloperLog(tr("[Run#%1][ERROR] %2").arg(runId, 3, 10, QLatin1Char('0')).arg(message));
        appendLog(tr("[错误] %1").arg(message));
        ui->statusValueLabel->setText(tr("错误"));
        ui->statusValueLabel->setStyleSheet(
            QStringLiteral("color:#ff6670;font-weight:700;"));
        ui->captureStatusInfoValueLabel->setText(tr("错误"));
        ui->captureStatusInfoValueLabel->setStyleSheet(
            QStringLiteral("color:#ff6670;font-weight:700;"));
        footerStreamLabel_->setText(tr("采集  ERROR"));
        footerStreamLabel_->setStyleSheet(
            QStringLiteral("color:#ff6670;font-weight:700;"));
    });

    connect(rxWorker_, &RxWorker::displayFrameReady, this,
            [this, cfg](const DisplayFrame& frame) {
        spectrum_->setSpectra(frame.spectrumDb, frame.enabled,
                              cfg.centerFrequencyHz, cfg.sampleRateHz);
        const int monitor = static_cast<int>(cfg.displayChannel);
        int waterfallChannel = monitor;
        if (waterfallChannel < 0 || waterfallChannel >= 8 ||
            frame.spectrumDb[waterfallChannel].isEmpty()) {
            waterfallChannel = -1;
            for (int ch = 0; ch < 8; ++ch) {
                if (frame.enabled[ch] && !frame.spectrumDb[ch].isEmpty()) {
                    waterfallChannel = ch;
                    break;
                }
            }
        }
        if (waterfallChannel >= 0)
            waterfall_->appendSpectrum(frame.spectrumDb[waterfallChannel]);

        waveform_->setSamples(frame.monitorI, frame.monitorQ, cfg.sampleRateHz,
                              channelName(cfg.displayChannel));
    }, Qt::QueuedConnection);
    connect(rxWorker_, &RxWorker::statisticsUpdated, this,
            [this](quint64 bytes, double mibps, qint64 ms) {
        const double mib = bytes / 1048576.0;
        const double seconds = ms / 1000.0;
        ui->totalDataValueLabel->setText(QString::number(mib, 'f', 1) + QStringLiteral(" MiB"));
        ui->throughputValueLabel->setText(QString::number(mibps, 'f', 1) + QStringLiteral(" MiB/s"));
        ui->elapsedValueLabel->setText(QString::number(seconds, 'f', 3) + QStringLiteral(" s"));
        footerRateLabel_->setText(
            tr("RX  %1 MiB/s  |  %2 MiB  |  %3 s")
                .arg(mibps, 0, 'f', 1)
                .arg(mib, 0, 'f', 1)
                .arg(seconds, 0, 'f', 1));
    });

    connect(rxWorker_, &RxWorker::stopped, this, &MainWindow::onWorkerStopped);
    connect(rxWorker_, &RxWorker::stopped, this, [this, runId = cfg.captureRunId] {
        appendDeveloperLog(tr("[Run#%1][WORKER] stopped signal")
            .arg(runId, 3, 10, QLatin1Char('0')));
    });
    connect(rxWorker_, &RxWorker::stopped, rxThread_, &QThread::quit);
    connect(rxThread_, &QThread::finished, rxWorker_, &QObject::deleteLater);
    connect(rxThread_, &QThread::finished, this, [this, runId = cfg.captureRunId] {
        appendDeveloperLog(tr("[Run#%1][THREAD] QThread finished")
            .arg(runId, 3, 10, QLatin1Char('0')));
        QThread* oldThread = rxThread_;
        rxThread_ = nullptr;
        rxWorker_ = nullptr;
        if (oldThread) oldThread->deleteLater();
        setState(State::Idle);
        refreshIqBufferOptions();
        updateConnectionUi();
        if (closeAfterStop_) {
            closeAfterStop_ = false;
            QTimer::singleShot(0, this, &QWidget::close);
        }
    });

    appendLog(tr("[采集] Run#%1 启动请求：%2 | %3 通道")
              .arg(cfg.captureRunId, 3, 10, QLatin1Char('0'))
              .arg(modelName(cfg.model))
              .arg(enabledChannelCount(cfg)));
    if (cfg.captureDurationSeconds > 0.0) {
        appendLog(tr("[采集] 采集时间：%1 s | 到时自动安全停止")
            .arg(cfg.captureDurationSeconds, 0, 'f', 3));
    } else {
        appendLog(tr("[采集] 采集时间：连续"));
    }
    appendDeveloperLog(tr("[Run#%1][GUI] start | display=%2 | FFT=%3 | duration=%4 s | save=%5 | path=%6")
        .arg(cfg.captureRunId, 3, 10, QLatin1Char('0'))
        .arg(channelName(cfg.displayChannel))
        .arg(cfg.fftSize)
        .arg(cfg.captureDurationSeconds, 0, 'f', 3)
        .arg(cfg.saveIq ? QStringLiteral("1") : QStringLiteral("0"))
        .arg(cfg.saveIq ? cfg.iqFilePath : QStringLiteral("-")));
    if (cfg.saveIq) {
        const HostMemoryBudget budget = detectHostMemoryBudget();
        const quint64 expected = expectedIqBytesPerSecond(cfg);
        const double windowSec = expected > 0
            ? static_cast<double>(cfg.iqBufferBytes) / static_cast<double>(expected) : 0.0;
        appendLog(tr("[存储] 原始 IQ 保存已启用（连续无跳块策略）"));
        appendLog(tr("[存储] 缓存配置：%1%2 | 主机可用内存 %3 | 安全上限 %4 | 约 %5 s")
            .arg(cfg.iqBufferAuto ? tr("自动→") : QString())
            .arg(compactBytes(static_cast<quint64>(cfg.iqBufferBytes)))
            .arg(compactBytes(budget.effectiveAvailable))
            .arg(compactBytes(budget.safeIqBufferLimit))
            .arg(windowSec, 0, 'f', 2));
        appendDeveloperLog(tr("[Run#%1][GUI][STORAGE] memAvailable=%2 safeLimit=%3 selected=%4 auto=%5 expected=%6 MiB/s bufferWindow=%7 s")
            .arg(cfg.captureRunId, 3, 10, QLatin1Char('0'))
            .arg(compactBytes(budget.effectiveAvailable))
            .arg(compactBytes(budget.safeIqBufferLimit))
            .arg(compactBytes(static_cast<quint64>(cfg.iqBufferBytes)))
            .arg(cfg.iqBufferAuto ? 1 : 0)
            .arg(expected / 1048576.0, 0, 'f', 1)
            .arg(windowSec, 0, 'f', 2));
    }
    {
        const PcieLinkBudget pcie = detectLwPcieBudget();
        const double requiredMiBps = expectedIqBytesPerSecond(cfg) / 1048576.0;
        if (pcie.valid && pcie.ambiguous) {
            appendLog(tr("[链路] 检测到 %1 个 Xilinx PCIe 端点，当前 SDK 无法可靠映射 pcie_0 到具体 BDF；本次不执行硬性链路带宽预限制")
                .arg(pcie.candidateCount));
            appendDeveloperLog(tr("[Run#%1][PCIE] ambiguous endpoints=%2 diagnosticBestBdf=%3 gtps=%4 width=%5 theoretical=%6 MiB/s required=%7 MiB/s")
                .arg(cfg.captureRunId, 3, 10, QLatin1Char('0')).arg(pcie.candidateCount).arg(pcie.bdf)
                .arg(pcie.gtps, 0, 'f', 1).arg(pcie.width)
                .arg(pcie.theoreticalMiBps, 0, 'f', 1)
                .arg(requiredMiBps, 0, 'f', 1));
        } else if (pcie.valid) {
            appendLog(tr("[链路] FPGA PCIe %1 GT/s ×%2 | 理论约 %3 MiB/s | 连续采集建议≤%4 MiB/s | 当前需求 %5 MiB/s")
                .arg(pcie.gtps, 0, 'f', 1).arg(pcie.width)
                .arg(pcie.theoreticalMiBps, 0, 'f', 0)
                .arg(pcie.recommendedMiBps, 0, 'f', 0)
                .arg(requiredMiBps, 0, 'f', 1));
            appendDeveloperLog(tr("[Run#%1][PCIE] bdf=%2 gtps=%3 width=%4 theoretical=%5 MiB/s recommended=%6 MiB/s required=%7 MiB/s")
                .arg(cfg.captureRunId, 3, 10, QLatin1Char('0')).arg(pcie.bdf)
                .arg(pcie.gtps, 0, 'f', 1).arg(pcie.width)
                .arg(pcie.theoreticalMiBps, 0, 'f', 1)
                .arg(pcie.recommendedMiBps, 0, 'f', 1)
                .arg(requiredMiBps, 0, 'f', 1));
        } else {
            appendLog(tr("[链路] 未读取到 FPGA PCIe 协商速率；本次不执行链路带宽预限制"));
        }
    }
    rxThread_->start();
}

void MainWindow::stopCapture()
{
    if (!rxWorker_ || state_ == State::Idle || state_ == State::Stopping) return;
    setState(State::Stopping);
    ui->statusBar->showMessage(tr("● 正在安全停止 RX…"));
    appendLog(tr("[采集] 正在停止"));
    appendDeveloperLog(tr("[GUI] Stop requested; deviceReleaseConfirmed=%1")
        .arg(deviceReleaseConfirmed_ ? 1 : 0));
    rxWorker_->requestStop();

    QTimer::singleShot(2000, this, [this] {
        if (state_ == State::Stopping && rxWorker_ && !deviceReleaseConfirmed_) {
            appendLog(tr("[警告] 接收停止响应超时（>2 s）"));
            appendDeveloperLog(tr("[STOP] >2 s without deviceReleased; waiting for worker cleanup"));
        }
    });
}

void MainWindow::onWorkerStopped()
{
    deviceReleaseConfirmed_ = true;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (rxThread_ && rxThread_->isRunning()) {
        closeAfterStop_ = true;
        appendDeveloperLog(tr("[APP] close requested while RX thread is running"));
        stopCapture();
        event->ignore();
        return;
    }
    if (!shutdownLogged_) {
        appendLog(tr("[系统] 程序关闭"));
        shutdownLogged_ = true;
    }
    event->accept();
}
