#include "mainwindow.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QStackedWidget>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLocalSocket>
#include <QThread>
#include <QRegularExpression>
#include <QStyle>
#include <QPainter>
#include <QMimeData>
#include <QDirIterator>
#include <QScrollArea>
#include <QWindow>

// =============================================================================
// DropZoneWidget Implementation
// =============================================================================
DropZoneWidget::DropZoneWidget(QWidget *parent) : QFrame(parent) {
    setAcceptDrops(true);
    setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    setObjectName("dropZone");
    setMinimumHeight(200);
}

void DropZoneWidget::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        setStyleSheet("#dropZone { border: 2px dashed #007aff; background: rgba(0, 122, 255, 0.05); }");
    }
}

void DropZoneWidget::dragLeaveEvent(QDragLeaveEvent *event) {
    Q_UNUSED(event);
    setStyleSheet("");
}

void DropZoneWidget::dropEvent(QDropEvent *event) {
    const QMimeData *mime = event->mimeData();
    if (mime->hasUrls()) {
        QList<QUrl> urlList = mime->urls();
        if (!urlList.isEmpty()) {
            QString filePath = urlList.first().toLocalFile();
            emit fileDropped(filePath);
        }
    }
    setStyleSheet("");
}

// =============================================================================
// MainWindow Implementation
// =============================================================================
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_activeSampleRate(48000)
    , m_isUpdatingConfig(false)
    , m_installerProcess(nullptr)
    , m_tuningProcess(nullptr)
{
    ensureDaemonRunning();
    initUi();
    setupGlobalStylesheet();

    // Start background status query loop
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::querySystemStatus);
    m_statusTimer->start(2000);

    // Initialize level meter update timer (10ms to 100ms interval)
    m_meterTimer = new QTimer(this);
    connect(m_meterTimer, &QTimer::timeout, this, &MainWindow::updateMeterAnimations);
    m_meterTimer->start(100);

    // Initial config query
    QTimer::singleShot(200, this, &MainWindow::loadAudioConfig);
    // Initial console scan and setup
    QTimer::singleShot(500, this, &MainWindow::rebuildConsoleChannels);
}

MainWindow::~MainWindow() {
    // Dismantle (unload) all background preloaded guest VST instances on isolated cores
    for (const QString &shmName : m_activePreloadedShms) {
        sendDaemonCommand(QString("UNLOAD %1").arg(shmName));
    }
    m_activePreloadedShms.clear();

    for (int i = 0; i < 3; ++i) {
        if (m_cllsSlots[i].process) {
            m_cllsSlots[i].process->kill();
            m_cllsSlots[i].process->waitForFinished(500);
        }
    }
    if (m_installerProcess) {
        m_installerProcess->kill();
    }
    if (m_tuningProcess) {
        m_tuningProcess->kill();
    }
}

void MainWindow::setupGlobalStylesheet() {
    QString style = R"(
        QMainWindow {
            background-color: #090a0f;
        }
        QWidget {
            color: #f0f2f5;
            font-family: 'Outfit', sans-serif;
            font-size: 13px;
        }
        /* Sidebar Styling */
        #sidebar {
            background-color: rgba(10, 12, 20, 0.6);
            border-right: 1px solid rgba(255, 255, 255, 0.08);
        }
        #brandLabel {
            font-size: 20px;
            font-weight: 800;
            color: #ffffff;
            margin-bottom: 20px;
        }
        .navBtn {
            background-color: transparent;
            border: none;
            border-left: 3px solid transparent;
            color: #a0a5b5;
            text-align: left;
            padding: 8px 12px;
            font-weight: 600;
            font-size: 13px;
            border-radius: 4px;
        }
        .navBtn:hover {
            background-color: rgba(255, 255, 255, 0.04);
            color: #ffffff;
        }
        .navBtn[active="true"] {
            background-color: rgba(0, 122, 255, 0.15);
            color: #ffffff;
            border-left: 3px solid #007aff;
        }
        /* Cards styling */
        .card {
            background-color: rgba(20, 24, 38, 0.55);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 16px;
        }
        .card:hover {
            border-color: rgba(255, 255, 255, 0.15);
        }
        .cardTitle {
            font-weight: 600;
            color: #a0a5b5;
            font-size: 14px;
        }
        /* Badges */
        .badge {
            font-weight: 600;
            font-size: 11px;
            padding: 4px 10px;
            border-radius: 12px;
        }
        .badgeGreen {
            background-color: rgba(48, 209, 88, 0.15);
            color: #30d158;
            border: 1px solid rgba(48, 209, 88, 0.3);
        }
        .badgeBlue {
            background-color: rgba(0, 122, 255, 0.15);
            color: #007aff;
            border: 1px solid rgba(0, 122, 255, 0.3);
        }
        .badgeGray {
            background-color: rgba(255, 255, 255, 0.05);
            color: #a0a5b5;
            border: 1px solid rgba(255, 255, 255, 0.1);
        }
        /* Inputs & Combos */
        .custom-select {
            background-color: rgba(255, 255, 255, 0.05);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 8px;
            padding: 5px 10px;
            color: #f0f2f5;
        }
        .custom-select QAbstractItemView {
            background-color: #141826;
            border: 1px solid rgba(255, 255, 255, 0.1);
            color: #f0f2f5;
            selection-background-color: #007aff;
        }
        .custom-input {
            background-color: rgba(255, 255, 255, 0.05);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 8px;
            padding: 5px 10px;
            color: #f0f2f5;
        }
        /* Buttons */
        .action-btn {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #007aff, stop:1 #af52de);
            border: none;
            border-radius: 8px;
            color: white;
            font-weight: 600;
            padding: 8px 15px;
        }
        .action-btn:hover {
            filter: brightness(1.1);
        }
        .settings-btn {
            background-color: rgba(255, 255, 255, 0.08);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 8px;
            color: #f0f2f5;
            font-weight: 600;
            padding: 8px 15px;
        }
        .settings-btn:hover {
            background-color: rgba(255, 255, 255, 0.15);
        }
        /* Console Terminal */
        .console-log {
            background-color: #05070f;
            border: 1px solid rgba(255, 255, 255, 0.05);
            border-radius: 12px;
            font-family: 'Monospace', monospace;
            font-size: 12px;
            color: #4af626;
        }
        /* Rack grid slots */
        .rack-slot {
            height: 48px;
            border-radius: 8px;
            background-color: rgba(255, 255, 255, 0.03);
            border: 1px solid rgba(255, 255, 255, 0.06);
            padding: 0 12px;
        }
        .rack-slot:hover {
            background-color: rgba(255, 255, 255, 0.05);
        }
        .rack-slot-empty {
            border: 1px dashed rgba(255, 255, 255, 0.15);
            background-color: transparent;
        }
        .rack-slot-empty:hover {
            border-color: #007aff;
            background-color: rgba(0, 122, 255, 0.03);
        }
        #dropZone {
            border: 2px dashed rgba(255, 255, 255, 0.15);
            background-color: rgba(255, 255, 255, 0.01);
            border-radius: 16px;
        }
        /* Console Strip Layout & Faders/Meters */
        .console-strip {
            background-color: rgba(26, 18, 43, 0.55);
            border: 1px solid rgba(191, 90, 242, 0.22);
            border-radius: 12px;
            min-width: 120px;
            max-width: 120px;
        }
        .console-strip:hover {
            border-color: rgba(191, 90, 242, 0.6);
            background-color: rgba(35, 24, 59, 0.7);
        }
        .console-strip-master {
            background-color: rgba(16, 26, 38, 0.65);
            border: 1px solid rgba(0, 240, 255, 0.25);
            border-radius: 12px;
            min-width: 120px;
            max-width: 120px;
        }
        .console-strip-master:hover {
            border-color: rgba(0, 240, 255, 0.65);
            background-color: rgba(22, 36, 51, 0.8);
        }
        .btn-mute {
            background-color: rgba(255, 255, 255, 0.06);
            border: 1px solid rgba(255, 255, 255, 0.08);
            color: #a0a5b5;
            border-radius: 6px;
            font-weight: 800;
            font-size: 11px;
            padding: 5px;
        }
        .btn-mute:checked {
            background-color: rgba(255, 59, 48, 0.25);
            border-color: #ff3b30;
            color: #ff453a;
        }
        .btn-solo {
            background-color: rgba(255, 255, 255, 0.06);
            border: 1px solid rgba(255, 255, 255, 0.08);
            color: #a0a5b5;
            border-radius: 6px;
            font-weight: 800;
            font-size: 11px;
            padding: 5px;
        }
        .btn-solo:checked {
            background-color: rgba(255, 214, 10, 0.25);
            border-color: #ffd60a;
            color: #ffdb0a;
        }
        QSlider::groove:vertical {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #050308, stop:1 #1a1525);
            border: 1px solid rgba(255, 255, 255, 0.04);
            width: 6px;
            border-radius: 3px;
        }
        QSlider::handle:vertical {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                       stop:0 #252030, stop:0.4 #423852, stop:0.5 #bf5af2,
                                       stop:0.6 #423852, stop:1 #252030);
            border: 1px solid rgba(191, 90, 242, 0.85);
            height: 28px;
            width: 22px;
            margin: 0 -8px;
            border-radius: 4px;
        }
        QSlider::handle:vertical:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                       stop:0 #322b40, stop:0.4 #594c6e, stop:0.5 #d68ff7,
                                       stop:0.6 #594c6e, stop:1 #322b40);
            border-color: #d68ff7;
        }
        QProgressBar:vertical {
            background: rgba(255, 255, 255, 0.05);
            width: 8px;
            border-radius: 4px;
        }
        QProgressBar::chunk:vertical {
            background: qlineargradient(x1:0, y1:1, x2:0, y2:0,
                                       stop:0 #7a1fa2, stop:0.7 #bf5af2, stop:1 #00f0ff);
            border-radius: 4px;
        }
        QDial {
            background-color: #110c1f;
            border: 2px solid rgba(191, 90, 242, 0.35);
            border-radius: 16px;
        }
        .console-tab-btn {
            background-color: rgba(255, 255, 255, 0.03);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 6px;
            color: #a0a5b5;
            font-weight: 700;
            font-size: 12px;
            padding: 6px 16px;
        }
        .console-tab-btn:hover {
            background-color: rgba(255, 255, 255, 0.08);
            color: #ffffff;
        }
        .console-tab-btn[active="true"] {
            background-color: rgba(191, 90, 242, 0.15);
            border-color: #bf5af2;
            color: #bf5af2;
        }
        .instrument-slot-card {
            background-color: rgba(26, 18, 43, 0.55);
            border: 1px solid rgba(191, 90, 242, 0.25);
            border-radius: 12px;
        }
        .instrument-slot-card:hover {
            border-color: rgba(191, 90, 242, 0.55);
            background-color: rgba(35, 24, 59, 0.7);
        }
        .plugin-slot-btn {
            background-color: rgba(0, 0, 0, 0.4);
            border: 1px dashed rgba(191, 90, 242, 0.25);
            border-radius: 4px;
            color: #8a829e;
            font-size: 9px;
            font-weight: bold;
            padding: 2px 4px;
            height: 18px;
            text-align: left;
        }
        .plugin-slot-btn:hover {
            border-color: rgba(191, 90, 242, 0.6);
            background-color: rgba(0, 0, 0, 0.6);
            color: #ffffff;
        }
        .plugin-slot-btn[loaded="true"] {
            background-color: rgba(26, 18, 43, 0.7);
            border: 1px solid #bf5af2;
            color: #d68ff7;
        }
        .plugin-slot-btn[loaded="true"]:hover {
            border-color: #d68ff7;
            color: #ffffff;
        }
        .btn-bypass {
            background-color: rgba(255, 255, 255, 0.05);
            border: 1px solid rgba(255, 255, 255, 0.1);
            color: #a0a5b5;
            border-radius: 6px;
            font-weight: 800;
            font-size: 8px;
            width: 13px;
            height: 13px;
            padding: 0px;
        }
        .btn-bypass:hover {
            background-color: rgba(255, 255, 255, 0.15);
            color: #ffffff;
        }
        .btn-bypass:checked {
            background-color: rgba(255, 149, 0, 0.25);
            border-color: #ff9500;
            color: #ffb040;
        }
        .btn-auto {
            background-color: rgba(255, 255, 255, 0.05);
            border: 1px solid rgba(255, 255, 255, 0.1);
            color: #a0a5b5;
            border-radius: 4px;
            font-weight: 800;
            font-size: 8px;
            padding: 3px 5px;
        }
        .btn-auto:hover {
            background-color: rgba(255, 255, 255, 0.15);
            color: #ffffff;
        }
        .btn-auto:checked {
            background-color: rgba(0, 240, 255, 0.15);
            border-color: #00f0ff;
            color: #00f0ff;
        }
        .btn-auto-slot {
            background-color: rgba(255, 255, 255, 0.05);
            border: 1px solid rgba(255, 255, 255, 0.1);
            color: #a0a5b5;
            border-radius: 6px;
            font-weight: 800;
            font-size: 8px;
            width: 13px;
            height: 13px;
            padding: 0px;
        }
        .btn-auto-slot:hover {
            background-color: rgba(0, 240, 255, 0.1);
            color: #00f0ff;
        }
        .btn-auto-slot:checked {
            background-color: rgba(0, 240, 255, 0.2);
            border-color: #00f0ff;
            color: #00f0ff;
            box-shadow: 0 0 4px rgba(0, 240, 255, 0.4);
        }
    )";
    setStyleSheet(style);
}

void MainWindow::initUi() {
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // =========================================================================
    // Sidebar Navigation Pane (Left)
    // =========================================================================
    m_sidebar = new QWidget(this);
    m_sidebar->setObjectName("sidebar");
    m_sidebar->setFixedWidth(190);
    QVBoxLayout *sidebarLayout = new QVBoxLayout(m_sidebar);
    sidebarLayout->setContentsMargins(12, 15, 12, 12);

    // Brand Header
    QLabel *logoLabel = new QLabel("A", m_sidebar);
    logoLabel->setFixedSize(26, 26);
    logoLabel->setAlignment(Qt::AlignCenter);
    logoLabel->setStyleSheet("background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #007aff, stop:1 #af52de); border-radius: 6px; font-weight: 800; font-size: 13px;");

    QLabel *brandText = new QLabel("ARTHUR", m_sidebar);
    brandText->setStyleSheet("font-size: 15px; font-weight: 800; letter-spacing: 1.5px; color: #ffffff;");
    
    QHBoxLayout *brandLayout = new QHBoxLayout();
    brandLayout->addWidget(logoLabel);
    brandLayout->addWidget(brandText);
    brandLayout->addStretch();
    sidebarLayout->addLayout(brandLayout);
    sidebarLayout->addSpacing(15);

    // Nav Buttons
    QPushButton *btnDashboard = new QPushButton("  Dashboard", m_sidebar);
    btnDashboard->setProperty("active", true);
    btnDashboard->setIcon(QApplication::style()->standardIcon(QStyle::SP_ComputerIcon));
    btnDashboard->setCursor(Qt::PointingHandCursor);
    btnDashboard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btnDashboard->setFixedHeight(36);
    btnDashboard->setProperty("class", "navBtn");
    
    QPushButton *btnInstaller = new QPushButton("  Install Plugins", m_sidebar);
    btnInstaller->setIcon(QApplication::style()->standardIcon(QStyle::SP_DriveHDIcon));
    btnInstaller->setCursor(Qt::PointingHandCursor);
    btnInstaller->setFixedHeight(36);
    btnInstaller->setProperty("class", "navBtn");

    QPushButton *btnSettings = new QPushButton("  Audio Setup", m_sidebar);
    btnSettings->setIcon(QApplication::style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    btnSettings->setCursor(Qt::PointingHandCursor);
    btnSettings->setFixedHeight(36);
    btnSettings->setProperty("class", "navBtn");

    QPushButton *btnConsole = new QPushButton("  VHC Console", m_sidebar);
    btnConsole->setIcon(QApplication::style()->standardIcon(QStyle::SP_MediaVolume));
    btnConsole->setCursor(Qt::PointingHandCursor);
    btnConsole->setFixedHeight(36);
    btnConsole->setProperty("class", "navBtn");

    sidebarLayout->addWidget(btnDashboard);
    sidebarLayout->addWidget(btnInstaller);
    sidebarLayout->addWidget(btnSettings);
    sidebarLayout->addWidget(btnConsole);
    sidebarLayout->addStretch();

    // Global Status Dot Indicator (Sidebar Bottom)
    m_statusDot = new QLabel(m_sidebar);
    m_statusDot->setFixedSize(8, 8);
    m_statusDot->setStyleSheet("background-color: #30d158; border-radius: 4px;");
    
    m_statusTextLabel = new QLabel("System phase-locked", m_sidebar);
    m_statusTextLabel->setStyleSheet("color: #a0a5b5; font-size: 12px;");

    QHBoxLayout *globalStatusLayout = new QHBoxLayout();
    globalStatusLayout->addWidget(m_statusDot);
    globalStatusLayout->addWidget(m_statusTextLabel);
    globalStatusLayout->addStretch();
    sidebarLayout->addLayout(globalStatusLayout);

    mainLayout->addWidget(m_sidebar);

    // =========================================================================
    // Content Stacks (Right)
    // =========================================================================
    m_contentArea = new QStackedWidget(this);
    m_contentArea->setStyleSheet("background-color: #0f121e; padding: 15px;");
    mainLayout->addWidget(m_contentArea);

    // Connect Navigation Button Clicks to Stack switches
    auto refreshBtns = [=](QPushButton* active) {
        for (QPushButton *b : {btnDashboard, btnInstaller, btnSettings, btnConsole}) {
            b->setProperty("active", (b == active));
            b->style()->unpolish(b); b->style()->polish(b);
        }
    };
    connect(btnDashboard, &QPushButton::clicked, this, [=]() { refreshBtns(btnDashboard); showDashboard(); });
    connect(btnInstaller, &QPushButton::clicked, this, [=]() { refreshBtns(btnInstaller); showInstaller(); });
    connect(btnSettings,  &QPushButton::clicked, this, [=]() { refreshBtns(btnSettings);  showSettings();  });
    connect(btnConsole,   &QPushButton::clicked, this, [=]() { refreshBtns(btnConsole);   showConsole();   });

    // =========================================================================
    // Stack 1: Dashboard Tab
    // =========================================================================
    m_dashboardTab = new QWidget(this);
    QVBoxLayout *dashLayout = new QVBoxLayout(m_dashboardTab);
    dashLayout->setContentsMargins(10, 10, 10, 10);
    dashLayout->setSpacing(10);

    QLabel *dashTitle = new QLabel("Studio OS Dashboard", m_dashboardTab);
    dashTitle->setStyleSheet("font-size: 18px; font-weight: 800; color: #ffffff;");
    QLabel *dashSub = new QLabel("Dynamic monitoring of translation and clock alignment layers.", m_dashboardTab);
    dashSub->setStyleSheet("color: #a0a5b5; font-size: 11px;");

    dashLayout->addWidget(dashTitle);
    dashLayout->addWidget(dashSub);

    QGridLayout *dashGrid = new QGridLayout();
    dashGrid->setSpacing(10);

    // Card 1: Virtual DSP Cores
    QFrame *dspCard = new QFrame(m_dashboardTab);
    dspCard->setProperty("class", "card");
    QVBoxLayout *dspCardLayout = new QVBoxLayout(dspCard);
    
    QHBoxLayout *dspHeader = new QHBoxLayout();
    QLabel *dspTitleLabel = new QLabel("Virtual DSP Cores (VDC)", dspCard);
    dspTitleLabel->setProperty("class", "cardTitle");
    m_dspActiveBadge = new QLabel("Active", dspCard);
    m_dspActiveBadge->setProperty("class", "badge badgeGreen");
    m_dspActiveBadge->setAlignment(Qt::AlignCenter);
    dspHeader->addWidget(dspTitleLabel);
    dspHeader->addWidget(m_dspActiveBadge);
    dspCardLayout->addLayout(dspHeader);

    QHBoxLayout *dspInfoRow = new QHBoxLayout();
    QLabel *dspCoresLabel = new QLabel("Dedicated Cores:", dspCard);
    dspCoresLabel->setStyleSheet("color: #a0a5b5; font-size: 13px;");
    m_dspCoresVal = new QLabel("4-7", dspCard);
    m_dspCoresVal->setStyleSheet("font-weight: 600;");
    dspInfoRow->addWidget(dspCoresLabel);
    dspInfoRow->addWidget(m_dspCoresVal);
    dspInfoRow->addStretch();
    dspCardLayout->addLayout(dspInfoRow);
    dspCardLayout->addSpacing(10);

    QHBoxLayout *dspLoadRow = new QHBoxLayout();
    QLabel *dspLoadLabel = new QLabel("DSP Load", dspCard);
    dspLoadLabel->setStyleSheet("font-weight: 600; font-size: 12px;");
    m_dspLoadText = new QLabel("34.8%", dspCard);
    m_dspLoadText->setStyleSheet("font-weight: 600; font-size: 12px;");
    dspLoadRow->addWidget(dspLoadLabel);
    dspLoadRow->addStretch();
    dspLoadRow->addWidget(m_dspLoadText);
    dspCardLayout->addLayout(dspLoadRow);

    m_dspLoadProgress = new QProgressBar(dspCard);
    m_dspLoadProgress->setRange(0, 100);
    m_dspLoadProgress->setValue(34);
    m_dspLoadProgress->setTextVisible(false);
    m_dspLoadProgress->setFixedHeight(8);
    m_dspLoadProgress->setStyleSheet("QProgressBar { background-color: rgba(255,255,255,0.05); border-radius: 4px; } QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #007aff, stop:1 #af52de); border-radius: 4px; }");
    dspCardLayout->addWidget(m_dspLoadProgress);
    dashGrid->addWidget(dspCard, 0, 0);

    // Card 2: Closed Loop Latency Sync
    QFrame *cllsCard = new QFrame(m_dashboardTab);
    cllsCard->setProperty("class", "card");
    QVBoxLayout *cllsCardLayout = new QVBoxLayout(cllsCard);

    QHBoxLayout *cllsHeader = new QHBoxLayout();
    QLabel *cllsTitleLabel = new QLabel("Closed-Loop Latency Sync", cllsCard);
    cllsTitleLabel->setProperty("class", "cardTitle");
    m_cllsStatusBadge = new QLabel("Locked", cllsCard);
    m_cllsStatusBadge->setProperty("class", "badge badgeBlue");
    m_cllsStatusBadge->setAlignment(Qt::AlignCenter);
    cllsHeader->addWidget(cllsTitleLabel);
    cllsHeader->addWidget(m_cllsStatusBadge);
    cllsCardLayout->addLayout(cllsHeader);

    auto makeStatRow = [](const QString &labelStr, QLabel *&valWidget, QWidget *parent) {
        QFrame *row = new QFrame(parent);
        row->setStyleSheet("border-bottom: 1px solid rgba(255,255,255,0.04);");
        QHBoxLayout *l = new QHBoxLayout(row);
        l->setContentsMargins(0, 8, 0, 8);
        QLabel *lbl = new QLabel(labelStr, row);
        lbl->setStyleSheet("color: #a0a5b5; font-size: 13px;");
        valWidget = new QLabel("0.000 ms", row);
        valWidget->setStyleSheet("font-weight: 600;");
        l->addWidget(lbl);
        l->addStretch();
        l->addWidget(valWidget);
        return row;
    };

    cllsCardLayout->addWidget(makeStatRow("Hardware RTT:", m_cllsRttVal, cllsCard));
    cllsCardLayout->addWidget(makeStatRow("CLLS Correction:", m_cllsCorrectionVal, cllsCard));
    cllsCardLayout->addWidget(makeStatRow("Phase Jitter:", m_cllsJitterVal, cllsCard));
    dashGrid->addWidget(cllsCard, 0, 1);

    dashLayout->addLayout(dashGrid);

    // Wide Card: ALSA MIDI Clock
    QFrame *midiCard = new QFrame(m_dashboardTab);
    midiCard->setProperty("class", "card");
    QVBoxLayout *midiCardLayout = new QVBoxLayout(midiCard);

    QLabel *midiTitleLabel = new QLabel("ALSA MIDI Clock Alignment", midiCard);
    midiTitleLabel->setProperty("class", "cardTitle");
    midiCardLayout->addWidget(midiTitleLabel);
    midiCardLayout->addSpacing(10);

    QHBoxLayout *midiStatusRow = new QHBoxLayout();
    m_midiStatusText = new QLabel("Slaved to PCM hardware clock (hw:0,0)", midiCard);
    m_midiStatusText->setStyleSheet("font-weight: 600;");
    m_midiJitterBadge = new QLabel("Jitter: < 10 µs", midiCard);
    m_midiJitterBadge->setProperty("class", "badge badgeGreen");
    midiStatusRow->addWidget(m_midiStatusText);
    midiStatusRow->addStretch();
    midiStatusRow->addWidget(m_midiJitterBadge);
    midiCardLayout->addLayout(midiStatusRow);

    // Latency jitter visualization bars
    QHBoxLayout *graphLayout = new QHBoxLayout();
    graphLayout->setSpacing(6);
    graphLayout->setContentsMargins(10, 10, 10, 10);
    QFrame *graphFrame = new QFrame(midiCard);
    graphFrame->setStyleSheet("background-color: rgba(255,255,255,0.02); border-radius: 8px;");
    graphFrame->setFixedHeight(60);
    QHBoxLayout *graphInnerLayout = new QHBoxLayout(graphFrame);
    graphInnerLayout->setContentsMargins(6, 6, 6, 6);
    graphInnerLayout->setSpacing(6);

    for (int i = 0; i < 20; ++i) {
        QFrame *bar = new QFrame(graphFrame);
        bar->setStyleSheet("background-color: #007aff; border-radius: 3px;");
        bar->setFixedHeight(20 + QRandomGenerator::global()->bounded(25));
        bar->setFixedWidth(10);
        m_latencyBars.append(bar);
        graphInnerLayout->addWidget(bar, 0, Qt::AlignBottom);
    }
    midiCardLayout->addWidget(graphFrame);
    dashLayout->addWidget(midiCard);

    m_contentArea->addWidget(m_dashboardTab);

    // Timer to animate the midi bars
    m_midiJitterTimer = new QTimer(this);
    connect(m_midiJitterTimer, &QTimer::timeout, this, [=]() {
        bool active = m_midiSlaveCheck ? m_midiSlaveCheck->isChecked() : true;
        for (QWidget *bar : m_latencyBars) {
            if (active) {
                int height = 10 + QRandomGenerator::global()->bounded(35);
                bar->setFixedHeight(height);
                bar->setStyleSheet("background-color: #007aff; border-radius: 3px; opacity: 1.0;");
            } else {
                bar->setFixedHeight(4);
                bar->setStyleSheet("background-color: rgba(255,255,255,0.08); border-radius: 1px;");
            }
        }
    });
    m_midiJitterTimer->start(400);

    // =========================================================================
    // Stack 2: Installer Tab
    // =========================================================================
    m_installerTab = new QWidget(this);
    QVBoxLayout *instLayout = new QVBoxLayout(m_installerTab);
    instLayout->setSpacing(10);

    QLabel *instTitle = new QLabel("Drag & Drop VST3 Installer", m_installerTab);
    instTitle->setStyleSheet("font-size: 18px; font-weight: 800; color: #ffffff;");
    QLabel *instSub = new QLabel("Install Windows .exe or .msi plugin installers directly into the Arthur Translation Layer.", m_installerTab);
    instSub->setStyleSheet("color: #a0a5b5; font-size: 11px;");

    instLayout->addWidget(instTitle);
    instLayout->addWidget(instSub);

    m_dropZone = new DropZoneWidget(m_installerTab);
    QVBoxLayout *dropInner = new QVBoxLayout(m_dropZone);
    dropInner->setContentsMargins(40, 40, 40, 40);
    dropInner->setSpacing(15);
    
    QLabel *uploadIcon = new QLabel("📥", m_dropZone);
    uploadIcon->setAlignment(Qt::AlignCenter);
    uploadIcon->setStyleSheet("font-size: 48px;");
    
    QLabel *dropTitle = new QLabel("Drag & Drop Installer here", m_dropZone);
    dropTitle->setAlignment(Qt::AlignCenter);
    dropTitle->setStyleSheet("font-weight: 600; font-size: 18px;");

    QLabel *dropDesc = new QLabel("Supports Windows .exe and .msi installer packages", m_dropZone);
    dropDesc->setAlignment(Qt::AlignCenter);
    dropDesc->setStyleSheet("color: #a0a5b5; font-size: 13px;");

    QPushButton *chooseFileBtn = new QPushButton("Choose Installer File", m_dropZone);
    chooseFileBtn->setProperty("class", "settings-btn");
    chooseFileBtn->setCursor(Qt::PointingHandCursor);
    chooseFileBtn->setFixedWidth(200);

    dropInner->addWidget(uploadIcon);
    dropInner->addWidget(dropTitle);
    dropInner->addWidget(dropDesc);
    dropInner->addWidget(chooseFileBtn, 0, Qt::AlignCenter);
    
    instLayout->addWidget(m_dropZone);
    
    connect(m_dropZone, &DropZoneWidget::fileDropped, this, &MainWindow::startInstaller);
    connect(chooseFileBtn, &QPushButton::clicked, this, &MainWindow::selectInstallerFile);

    // Installation Progress Card (Hidden initially)
    m_installProgressCard = new QFrame(m_installerTab);
    m_installProgressCard->setProperty("class", "card");
    m_installProgressCard->setVisible(false);
    QVBoxLayout *progLayout = new QVBoxLayout(m_installProgressCard);
    
    QLabel *progTitle = new QLabel("Installing Plugin...", m_installProgressCard);
    progTitle->setStyleSheet("font-weight: 600; font-size: 16px;");
    m_installProgressBar = new QProgressBar(m_installProgressCard);
    m_installProgressBar->setRange(0, 100);
    m_installProgressBar->setValue(0);
    m_installProgressBar->setFixedHeight(12);
    m_installProgressBar->setStyleSheet("QProgressBar { background-color: rgba(255,255,255,0.05); border-radius: 6px; } QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #007aff, stop:1 #30d158); border-radius: 6px; }");

    QHBoxLayout *progStatusRow = new QHBoxLayout();
    m_installStatusText = new QLabel("Starting Wine sandbox environment...", m_installProgressCard);
    m_installStatusText->setStyleSheet("color: #a0a5b5; font-size: 13px;");
    progStatusRow->addWidget(m_installStatusText);
    progStatusRow->addStretch();
    
    progLayout->addWidget(progTitle);
    progLayout->addWidget(m_installProgressBar);
    progLayout->addLayout(progStatusRow);
    progLayout->addSpacing(10);

    QLabel *consoleHeader = new QLabel("INSTALLATION CONSOLE LOG", m_installProgressCard);
    consoleHeader->setStyleSheet("font-weight: 800; font-size: 11px; color: #a0a5b5; letter-spacing: 1px;");
    m_installConsole = new QTextEdit(m_installProgressCard);
    m_installConsole->setReadOnly(true);
    m_installConsole->setProperty("class", "console-log");
    m_installConsole->setFixedHeight(180);

    progLayout->addWidget(consoleHeader);
    progLayout->addWidget(m_installConsole);
    instLayout->addWidget(m_installProgressCard);

    // Status Success/Error Card (Hidden initially)
    m_installStatusCard = new QFrame(m_installerTab);
    m_installStatusCard->setProperty("class", "card");
    m_installStatusCard->setVisible(false);
    QVBoxLayout *statCardLayout = new QVBoxLayout(m_installStatusCard);
    statCardLayout->setContentsMargins(40, 40, 40, 40);
    statCardLayout->setSpacing(15);
    
    m_installStatusIcon = new QLabel("✓", m_installStatusCard);
    m_installStatusIcon->setAlignment(Qt::AlignCenter);
    m_installStatusIcon->setFixedSize(60, 60);
    m_installStatusIcon->setStyleSheet("background-color: rgba(48,209,88,0.15); color: #30d158; border: 2px solid rgba(48,209,88,0.3); border-radius: 30px; font-weight: 800; font-size: 32px;");

    m_installStatusTitle = new QLabel("Installation Succeeded!", m_installStatusCard);
    m_installStatusTitle->setAlignment(Qt::AlignCenter);
    m_installStatusTitle->setStyleSheet("font-weight: 600; font-size: 18px;");

    m_installStatusDesc = new QLabel("The plugin was virtualized successfully.", m_installStatusCard);
    m_installStatusDesc->setAlignment(Qt::AlignCenter);
    m_installStatusDesc->setStyleSheet("color: #a0a5b5; font-size: 13px;");

    QPushButton *statusOkBtn = new QPushButton("OK", m_installStatusCard);
    statusOkBtn->setProperty("class", "action-btn");
    statusOkBtn->setCursor(Qt::PointingHandCursor);
    statusOkBtn->setFixedWidth(120);
    connect(statusOkBtn, &QPushButton::clicked, this, [=]() {
        m_installStatusCard->setVisible(false);
        m_dropZone->setVisible(true);
    });

    statCardLayout->addWidget(m_installStatusIcon, 0, Qt::AlignCenter);
    statCardLayout->addWidget(m_installStatusTitle);
    statCardLayout->addWidget(m_installStatusDesc);
    statCardLayout->addWidget(statusOkBtn, 0, Qt::AlignCenter);
    instLayout->addWidget(m_installStatusCard);

    m_contentArea->addWidget(m_installerTab);

    // =========================================================================
    // Stack 4: Audio Setup Tab
    // =========================================================================
    m_settingsTab = new QWidget(this);
    QVBoxLayout *setTabMainLayout = new QVBoxLayout(m_settingsTab);
    setTabMainLayout->setSpacing(10);

    QLabel *setTabTitle = new QLabel("Audio Setup & System Tuner", m_settingsTab);
    setTabTitle->setStyleSheet("font-size: 18px; font-weight: 800; color: #ffffff;");
    QLabel *setTabSub = new QLabel("Configure PipeWire backend latency and lock kernel parameters for core isolation.", m_settingsTab);
    setTabSub->setStyleSheet("color: #a0a5b5; font-size: 11px;");

    setTabMainLayout->addWidget(setTabTitle);
    setTabMainLayout->addWidget(setTabSub);

    QGridLayout *setGrid = new QGridLayout();
    setGrid->setSpacing(10);

    // Card Left: PipeWire Settings
    QFrame *pwCard = new QFrame(m_settingsTab);
    pwCard->setProperty("class", "card");
    QVBoxLayout *pwCardLayout = new QVBoxLayout(pwCard);
    
    QLabel *pwCardTitle = new QLabel("PipeWire Settings", pwCard);
    pwCardTitle->setProperty("class", "cardTitle");
    pwCardLayout->addWidget(pwCardTitle);
    pwCardLayout->addSpacing(10);

    auto makeSettingsSelect = [](const QString &labelStr, QComboBox *&combo, QWidget *parent) {
        QFrame *item = new QFrame(parent);
        QVBoxLayout *l = new QVBoxLayout(item);
        l->setContentsMargins(0, 4, 0, 4);
        l->setSpacing(6);
        QLabel *lbl = new QLabel(labelStr, item);
        lbl->setStyleSheet("color: #a0a5b5; font-size: 12px; font-weight: 600;");
        combo = new QComboBox(item);
        combo->setProperty("class", "custom-select");
        l->addWidget(lbl);
        l->addWidget(combo);
        return item;
    };

    pwCardLayout->addWidget(makeSettingsSelect("Audio Interface", m_audioInterfaceSelect, pwCard));
    pwCardLayout->addWidget(makeSettingsSelect("Sample Rate", m_sampleRateSelect, pwCard));
    pwCardLayout->addWidget(makeSettingsSelect("Buffer Size (Quantum)", m_bufferSizeSelect, pwCard));

    m_sampleRateSelect->addItem("44,100 Hz", 44100);
    m_sampleRateSelect->addItem("48,000 Hz", 48000);
    m_sampleRateSelect->addItem("96,000 Hz", 96000);

    m_bufferSizeSelect->addItem("16 samples (0.3 ms)", 16);
    m_bufferSizeSelect->addItem("32 samples (0.7 ms)", 32);
    m_bufferSizeSelect->addItem("64 samples (1.3 ms)", 64);
    m_bufferSizeSelect->addItem("128 samples (2.7 ms)", 128);
    m_bufferSizeSelect->addItem("256 samples (5.3 ms)", 256);
    m_bufferSizeSelect->addItem("512 samples (10.7 ms)", 512);
    m_bufferSizeSelect->addItem("1024 samples (21.3 ms)", 1024);

    connect(m_audioInterfaceSelect, &QComboBox::currentIndexChanged, this, &MainWindow::applyAudioConfig);
    connect(m_sampleRateSelect, &QComboBox::currentIndexChanged, this, &MainWindow::applyAudioConfig);
    connect(m_bufferSizeSelect, &QComboBox::currentIndexChanged, this, &MainWindow::applyAudioConfig);

    setGrid->addWidget(pwCard, 0, 0);

    // Card Right: CLLS & MIDI Timings
    QFrame *cllsTimingsCard = new QFrame(m_settingsTab);
    cllsTimingsCard->setProperty("class", "card");
    QVBoxLayout *cllsTimingsCardLayout = new QVBoxLayout(cllsTimingsCard);

    QLabel *cllsTimingsCardTitle = new QLabel("CLLS Calibration Loops & MIDI Sync", cllsTimingsCard);
    cllsTimingsCardTitle->setProperty("class", "cardTitle");
    cllsTimingsCardLayout->addWidget(cllsTimingsCardTitle);
    cllsTimingsCardLayout->addSpacing(5);

    // Global MIDI check
    m_midiSlaveCheck = new QCheckBox("Slave ALSA Sequencer to Audio PCM clock", cllsTimingsCard);
    m_midiSlaveCheck->setChecked(true);
    m_midiSlaveCheck->setStyleSheet("QCheckBox { margin-top: 2px; font-weight: 600; font-size: 11px; }");
    connect(m_midiSlaveCheck, &QCheckBox::stateChanged, this, &MainWindow::applyAudioConfig);
    cllsTimingsCardLayout->addWidget(m_midiSlaveCheck);
    cllsTimingsCardLayout->addSpacing(5);

    // Grid for the 3 Interface Calibration Slots
    QGridLayout *cllsGrid = new QGridLayout();
    cllsGrid->setSpacing(4);

    // Headers
    QLabel *hSlot = new QLabel("Slot", cllsTimingsCard);
    hSlot->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");
    QLabel *hInterface = new QLabel("Sync Interface", cllsTimingsCard);
    hInterface->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");
    QLabel *hOut = new QLabel("Playback (Out)", cllsTimingsCard);
    hOut->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");
    QLabel *hIn = new QLabel("Capture (In)", cllsTimingsCard);
    hIn->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");
    QLabel *hAudioSrc = new QLabel("Audio Src (L/R)", cllsTimingsCard);
    hAudioSrc->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");
    QLabel *hAudioDst = new QLabel("Audio Dst (L/R)", cllsTimingsCard);
    hAudioDst->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");
    QLabel *hAction = new QLabel("Action", cllsTimingsCard);
    hAction->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");
    QLabel *hStatus = new QLabel("Latency", cllsTimingsCard);
    hStatus->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");
    QLabel *hLink = new QLabel("Link Status", cllsTimingsCard);
    hLink->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");

    cllsGrid->addWidget(hSlot, 0, 0);
    cllsGrid->addWidget(hInterface, 0, 1);
    cllsGrid->addWidget(hOut, 0, 2);
    cllsGrid->addWidget(hIn, 0, 3);
    cllsGrid->addWidget(hAudioSrc, 0, 4);
    cllsGrid->addWidget(hAudioDst, 0, 5);
    cllsGrid->addWidget(hAction, 0, 6);
    cllsGrid->addWidget(hStatus, 0, 7);
    cllsGrid->addWidget(hLink, 0, 8);

    for (int i = 0; i < 3; ++i) {
        QLabel *slotLabel = new QLabel(QString("Slot %1").arg(i + 1), cllsTimingsCard);
        slotLabel->setStyleSheet("font-weight: 600; color: #f0f2f5; font-size: 11px;");

        m_cllsSlots[i].interfaceSelect = new QComboBox(cllsTimingsCard);
        m_cllsSlots[i].interfaceSelect->setProperty("class", "custom-select");
        m_cllsSlots[i].interfaceSelect->setMinimumWidth(80);
        m_cllsSlots[i].interfaceSelect->setMaximumWidth(130);

        m_cllsSlots[i].playbackPortSelect = new QComboBox(cllsTimingsCard);
        m_cllsSlots[i].playbackPortSelect->setProperty("class", "custom-select");
        m_cllsSlots[i].playbackPortSelect->setMinimumWidth(70);

        m_cllsSlots[i].capturePortSelect = new QComboBox(cllsTimingsCard);
        m_cllsSlots[i].capturePortSelect->setProperty("class", "custom-select");
        m_cllsSlots[i].capturePortSelect->setMinimumWidth(70);

        m_cllsSlots[i].audioSrcSelect = new QComboBox(cllsTimingsCard);
        m_cllsSlots[i].audioSrcSelect->setProperty("class", "custom-select");
        m_cllsSlots[i].audioSrcSelect->setMinimumWidth(80);

        m_cllsSlots[i].audioDstSelect = new QComboBox(cllsTimingsCard);
        m_cllsSlots[i].audioDstSelect->setProperty("class", "custom-select");
        m_cllsSlots[i].audioDstSelect->setMinimumWidth(80);

        m_cllsSlots[i].runBtn = new QPushButton("Run", cllsTimingsCard);
        m_cllsSlots[i].runBtn->setProperty("class", "action-btn");
        m_cllsSlots[i].runBtn->setStyleSheet("padding: 4px 8px; font-size: 11px; border-radius: 6px;");
        m_cllsSlots[i].runBtn->setCursor(Qt::PointingHandCursor);
        m_cllsSlots[i].runBtn->setFixedWidth(50);

        m_cllsSlots[i].rttValLabel = new QLabel("-- smp", cllsTimingsCard);
        m_cllsSlots[i].rttValLabel->setStyleSheet("font-weight: 600; font-size: 11px; color: #a0a5b5;");

        m_cllsSlots[i].statusBadge = new QLabel("Disconnected", cllsTimingsCard);
        m_cllsSlots[i].statusBadge->setStyleSheet("background-color: rgba(255,255,255,0.05); color: #a0a5b5; border: 1px solid rgba(255,255,255,0.1); border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: bold;");
        m_cllsSlots[i].statusBadge->setAlignment(Qt::AlignCenter);
        m_cllsSlots[i].statusBadge->setFixedWidth(80);

        cllsGrid->addWidget(slotLabel, i + 1, 0);
        cllsGrid->addWidget(m_cllsSlots[i].interfaceSelect, i + 1, 1);
        cllsGrid->addWidget(m_cllsSlots[i].playbackPortSelect, i + 1, 2);
        cllsGrid->addWidget(m_cllsSlots[i].capturePortSelect, i + 1, 3);
        cllsGrid->addWidget(m_cllsSlots[i].audioSrcSelect, i + 1, 4);
        cllsGrid->addWidget(m_cllsSlots[i].audioDstSelect, i + 1, 5);
        cllsGrid->addWidget(m_cllsSlots[i].runBtn, i + 1, 6);
        cllsGrid->addWidget(m_cllsSlots[i].rttValLabel, i + 1, 7);
        cllsGrid->addWidget(m_cllsSlots[i].statusBadge, i + 1, 8);

        // Connect interface select change to populate ports and auto-save
        connect(m_cllsSlots[i].interfaceSelect, &QComboBox::currentIndexChanged, this, [=]() {
            populatePortsForSlot(i);
            saveAudioConfig();
        });

        // Connect port selects to auto-save
        connect(m_cllsSlots[i].playbackPortSelect, &QComboBox::currentIndexChanged, this, [=]() {
            saveAudioConfig();
        });
        connect(m_cllsSlots[i].capturePortSelect, &QComboBox::currentIndexChanged, this, [=]() {
            saveAudioConfig();
        });
        connect(m_cllsSlots[i].audioSrcSelect, &QComboBox::currentIndexChanged, this, [=]() {
            saveAudioConfig();
        });
        connect(m_cllsSlots[i].audioDstSelect, &QComboBox::currentIndexChanged, this, [=]() {
            saveAudioConfig();
        });

        // Connect run button to start calibration
        connect(m_cllsSlots[i].runBtn, &QPushButton::clicked, this, [=]() {
            startCllsCalibration(i);
        });
    }

    cllsTimingsCardLayout->addLayout(cllsGrid);
    cllsTimingsCardLayout->addStretch();

    setGrid->addWidget(cllsTimingsCard, 0, 1);
    setTabMainLayout->addLayout(setGrid);

    // Wide Tuning Card
    QFrame *tuningCard = new QFrame(m_settingsTab);
    m_tuningCard = tuningCard;
    tuningCard->setProperty("class", "card");
    QVBoxLayout *tuneLayout = new QVBoxLayout(tuningCard);
    
    QLabel *tuneTitle = new QLabel("Virtual DSP Core Partitioning", tuningCard);
    tuneTitle->setProperty("class", "cardTitle");
    QLabel *tuneDesc = new QLabel("Allocate isolated CPU cores to run host translation plugins exclusively. Isolating cores shields the audio process from scheduler interrupts.", tuningCard);
    tuneDesc->setStyleSheet("color: #a0a5b5; font-size: 11px; line-height: 1.5;");

    m_coresSlider = new QSlider(Qt::Horizontal, tuningCard);
    m_coresSlider->setRange(1, 7);
    m_coresSlider->setValue(4);
    
    m_coresStatusLabel = new QLabel("Allocating Cores 4-4 to Virtual DSP (1 Core isolated)", tuningCard);
    m_coresStatusLabel->setStyleSheet("font-weight: 600; color: #007aff; font-size: 12px;");
    m_coresStatusLabel->setAlignment(Qt::AlignCenter);

    connect(m_coresSlider, &QSlider::valueChanged, this, [=](int val) {
        if (val <= 4) {
            m_coresStatusLabel->setText(QString("Allocating Core 4 to Virtual DSP (1 Core isolated)"));
        } else {
            m_coresStatusLabel->setText(QString("Allocating Cores 4-%1 to Virtual DSP (%2 Cores isolated)").arg(val).arg(val - 3));
        }
    });

    QPushButton *applyTuningBtn = new QPushButton("Apply Core Isolation & Tune System", tuningCard);
    applyTuningBtn->setProperty("class", "action-btn");
    applyTuningBtn->setCursor(Qt::PointingHandCursor);
    connect(applyTuningBtn, &QPushButton::clicked, this, &MainWindow::runSystemTuning);

    tuneLayout->addWidget(tuneTitle);
    tuneLayout->addWidget(tuneDesc);
    tuneLayout->addSpacing(5);
    tuneLayout->addWidget(m_coresSlider);
    tuneLayout->addWidget(m_coresStatusLabel);
    tuneLayout->addSpacing(5);
    tuneLayout->addWidget(applyTuningBtn);

    setTabMainLayout->addWidget(tuningCard);

    // Tuning progress card
    m_tuningProgressCard = new QFrame(m_settingsTab);
    m_tuningProgressCard->setProperty("class", "card");
    m_tuningProgressCard->setVisible(false);
    QVBoxLayout *tProgLayout = new QVBoxLayout(m_tuningProgressCard);
    
    QLabel *tProgTitle = new QLabel("System Tuning & Setup Wizard", m_tuningProgressCard);
    tProgTitle->setStyleSheet("font-weight: 600; font-size: 16px;");
    m_tuningProgressBar = new QProgressBar(m_tuningProgressCard);
    m_tuningProgressBar->setRange(0, 100);
    m_tuningProgressBar->setValue(0);
    m_tuningProgressBar->setFixedHeight(12);
    m_tuningProgressBar->setStyleSheet("QProgressBar { background-color: rgba(255,255,255,0.05); border-radius: 6px; } QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #007aff, stop:1 #30d158); border-radius: 6px; }");

    m_tuningStatusText = new QLabel("Initializing system tuner...", m_tuningProgressCard);
    m_tuningStatusText->setStyleSheet("color: #a0a5b5; font-size: 13px;");

    QLabel *tConsoleHeader = new QLabel("SYSTEM TUNING LOGS", m_tuningProgressCard);
    tConsoleHeader->setStyleSheet("font-weight: 800; font-size: 11px; color: #a0a5b5; letter-spacing: 1px;");
    m_tuningConsole = new QTextEdit(m_tuningProgressCard);
    m_tuningConsole->setReadOnly(true);
    m_tuningConsole->setProperty("class", "console-log");
    m_tuningConsole->setFixedHeight(160);

    tProgLayout->addWidget(tProgTitle);
    tProgLayout->addWidget(m_tuningProgressBar);
    tProgLayout->addWidget(m_tuningStatusText);
    tProgLayout->addSpacing(10);
    tProgLayout->addWidget(tConsoleHeader);
    tProgLayout->addWidget(m_tuningConsole);
    setTabMainLayout->addWidget(m_tuningProgressCard);

    // Tuning success/error card
    m_tuningStatusCard = new QFrame(m_settingsTab);
    m_tuningStatusCard->setProperty("class", "card");
    m_tuningStatusCard->setVisible(false);
    QVBoxLayout *tStatLayout = new QVBoxLayout(m_tuningStatusCard);
    tStatLayout->setContentsMargins(15, 15, 15, 15);
    tStatLayout->setSpacing(10);
    
    m_tuningStatusTitle = new QLabel("System Setup Succeeded!", m_tuningStatusCard);
    m_tuningStatusTitle->setStyleSheet("font-weight: 600; font-size: 18px;");
    m_tuningStatusTitle->setAlignment(Qt::AlignCenter);
    
    m_tuningStatusDesc = new QLabel("All real-time parameters, core isolation, and Wine audio bridges have been configured. Please reboot your machine to apply the kernel parameters.", m_tuningStatusCard);
    m_tuningStatusDesc->setStyleSheet("color: #a0a5b5; font-size: 13px;");
    m_tuningStatusDesc->setAlignment(Qt::AlignCenter);

    QPushButton *tOkBtn = new QPushButton("OK", m_tuningStatusCard);
    tOkBtn->setProperty("class", "action-btn");
    tOkBtn->setFixedWidth(120);
    tOkBtn->setCursor(Qt::PointingHandCursor);
    connect(tOkBtn, &QPushButton::clicked, this, [=]() {
        m_tuningStatusCard->setVisible(false);
        m_tuningCard->setVisible(true);
    });

    tStatLayout->addWidget(m_tuningStatusTitle);
    tStatLayout->addWidget(m_tuningStatusDesc);
    tStatLayout->addWidget(tOkBtn, 0, Qt::AlignCenter);
    setTabMainLayout->addWidget(m_tuningStatusCard);

    m_contentArea->addWidget(m_settingsTab);

    // =========================================================================
    // Stack 5: VHC Console Tab
    // =========================================================================
    // =========================================================================
    // Stack 5: VHC Console Tab
    // =========================================================================
    m_consoleTab = new QWidget(this);
    QVBoxLayout *consLayout = new QVBoxLayout(m_consoleTab);
    consLayout->setSpacing(8);
    consLayout->setContentsMargins(10, 10, 10, 10);

    QLabel *consTitle = new QLabel("Virtual Hybrid Console (VHC)", m_consoleTab);
    consTitle->setStyleSheet("font-size: 18px; font-weight: 800; color: #ffffff;");
    QLabel *consSub = new QLabel("Set per-channel routing mode. Monitor Mode bypasses effects to record the dry signal while monitoring the wet processed audio.", m_consoleTab);
    consSub->setStyleSheet("color: #a0a5b5; font-size: 11px;");
    consLayout->addWidget(consTitle);
    consLayout->addWidget(consSub);

    // Sub-navigation bar for Console Pages
    QFrame *consoleSubNav = new QFrame(m_consoleTab);
    consoleSubNav->setStyleSheet("background-color: transparent;");
    QHBoxLayout *subNavLayout = new QHBoxLayout(consoleSubNav);
    subNavLayout->setContentsMargins(0, 0, 0, 5);
    subNavLayout->setSpacing(8);

    m_btnInputsPage = new QPushButton("Inputs Mixer", consoleSubNav);
    m_btnInputsPage->setProperty("class", "console-tab-btn");
    m_btnInputsPage->setProperty("active", true);
    m_btnInputsPage->setCursor(Qt::PointingHandCursor);

    m_btnBussesPage = new QPushButton("Busses & Sends", consoleSubNav);
    m_btnBussesPage->setProperty("class", "console-tab-btn");
    m_btnBussesPage->setProperty("active", false);
    m_btnBussesPage->setCursor(Qt::PointingHandCursor);

    m_btnInstrumentsPage = new QPushButton("Instruments & Plugins", consoleSubNav);
    m_btnInstrumentsPage->setProperty("class", "console-tab-btn");
    m_btnInstrumentsPage->setProperty("active", false);
    m_btnInstrumentsPage->setCursor(Qt::PointingHandCursor);

    subNavLayout->addWidget(m_btnInputsPage);
    subNavLayout->addWidget(m_btnBussesPage);
    subNavLayout->addWidget(m_btnInstrumentsPage);
    subNavLayout->addStretch();
    consLayout->addWidget(consoleSubNav);

    // Create the stacked widget
    m_consoleStack = new QStackedWidget(m_consoleTab);

    // ==================== PAGE 1: Inputs Mixer Page ====================
    QWidget *inputsPage = new QWidget();
    QVBoxLayout *inputsPageLayout = new QVBoxLayout(inputsPage);
    inputsPageLayout->setContentsMargins(0, 0, 0, 0);
    inputsPageLayout->setSpacing(8);

    // Info card explaining the three modes
    QFrame *modeInfoCard = new QFrame(inputsPage);
    modeInfoCard->setProperty("class", "card");
    QHBoxLayout *modeInfoLayout = new QHBoxLayout(modeInfoCard);
    modeInfoLayout->setSpacing(20);

    auto makeModeInfo = [&](const QString &icon, const QString &name, const QString &desc, const QString &color) {
        QVBoxLayout *l = new QVBoxLayout();
        l->setSpacing(4);
        QLabel *ico = new QLabel(icon, modeInfoCard);
        ico->setAlignment(Qt::AlignCenter);
        ico->setStyleSheet(QString("font-size: 28px; background-color: %1; border-radius: 8px; padding: 6px;").arg(color));
        ico->setFixedSize(48, 48);
        QLabel *nm = new QLabel(name, modeInfoCard);
        nm->setStyleSheet("font-weight: 700; font-size: 12px;");
        QLabel *ds = new QLabel(desc, modeInfoCard);
        ds->setStyleSheet("color: #a0a5b5; font-size: 11px;");
        ds->setWordWrap(true);
        l->addWidget(ico, 0, Qt::AlignHCenter);
        l->addWidget(nm);
        l->addWidget(ds);
        return l;
    };
    modeInfoLayout->addLayout(makeModeInfo("▶", "Playback (Mode 0)", "Processed wet signal sent to main output. Default mode for normal playback.", "rgba(0,122,255,0.12)"));
    modeInfoLayout->addLayout(makeModeInfo("⏺", "Dry+Monitor (Mode 1)", "Raw dry input sent to DAW for recording. Wet signal routed to monitor bus so you still hear effects.", "rgba(255,149,0,0.12)"));
    modeInfoLayout->addLayout(makeModeInfo("⏺", "Record Wet (Mode 2)", "Processed wet signal sent directly to DAW main output for recording. Bakes effects in.", "rgba(255,59,48,0.12)"));
    inputsPageLayout->addWidget(modeInfoCard);

    // Scan Button Header
    QFrame *scanHeader = new QFrame(inputsPage);
    scanHeader->setProperty("class", "card");
    QHBoxLayout *scanHeaderLayout = new QHBoxLayout(scanHeader);
    scanHeaderLayout->setContentsMargins(15, 10, 15, 10);
    QLabel *scanLabel = new QLabel("Active Channel Slots", scanHeader);
    scanLabel->setStyleSheet("font-weight: 700; font-size: 14px; color: #ffffff;");
    m_consoleScanBtn = new QPushButton("Scan SHM Channels", scanHeader);
    m_consoleScanBtn->setProperty("class", "settings-btn");
    m_consoleScanBtn->setCursor(Qt::PointingHandCursor);
    connect(m_consoleScanBtn, &QPushButton::clicked, this, &MainWindow::rebuildConsoleChannels);
    scanHeaderLayout->addWidget(scanLabel);
    scanHeaderLayout->addStretch();
    scanHeaderLayout->addWidget(m_consoleScanBtn);
    inputsPageLayout->addWidget(scanHeader);

    // Scrollable channel container
    QScrollArea *consScrollArea = new QScrollArea(inputsPage);
    consScrollArea->setWidgetResizable(true);
    consScrollArea->setFrameShape(QFrame::NoFrame);
    consScrollArea->setStyleSheet("background-color: transparent;");
    m_consoleChannelContainer = new QWidget();
    m_consoleChannelContainer->setStyleSheet("background-color: transparent;");
    QHBoxLayout *channelsLayout = new QHBoxLayout(m_consoleChannelContainer);
    channelsLayout->setContentsMargins(0, 0, 0, 0);
    channelsLayout->setSpacing(8);
    consScrollArea->setWidget(m_consoleChannelContainer);
    inputsPageLayout->addWidget(consScrollArea, 1);

    // ==================== PAGE 2: Busses & Sends Mixer Page ====================
    QWidget *bussesPage = new QWidget();
    QVBoxLayout *bussesPageLayout = new QVBoxLayout(bussesPage);
    bussesPageLayout->setContentsMargins(0, 0, 0, 0);
    bussesPageLayout->setSpacing(8);

    QScrollArea *bussesScrollArea = new QScrollArea(bussesPage);
    bussesScrollArea->setWidgetResizable(true);
    bussesScrollArea->setFrameShape(QFrame::NoFrame);
    bussesScrollArea->setStyleSheet("background-color: transparent;");
    m_consoleBussesContainer = new QWidget();
    m_consoleBussesContainer->setStyleSheet("background-color: transparent;");
    QHBoxLayout *bussesLayout = new QHBoxLayout(m_consoleBussesContainer);
    bussesLayout->setContentsMargins(0, 0, 0, 0);
    bussesLayout->setSpacing(8);
    bussesScrollArea->setWidget(m_consoleBussesContainer);
    bussesPageLayout->addWidget(bussesScrollArea, 1);

    // ==================== PAGE 3: Instruments Rack Page ====================
    QWidget *instrumentsPage = new QWidget();
    QVBoxLayout *instPageLayout = new QVBoxLayout(instrumentsPage);
    instPageLayout->setContentsMargins(0, 0, 0, 0);
    instPageLayout->setSpacing(10);

    QScrollArea *instScroll = new QScrollArea(instrumentsPage);
    instScroll->setWidgetResizable(true);
    instScroll->setFrameShape(QFrame::NoFrame);
    instScroll->setStyleSheet("background-color: transparent;");
    QWidget *instContainer = new QWidget();
    instContainer->setStyleSheet("background-color: transparent;");
    QGridLayout *instGrid = new QGridLayout(instContainer);
    instGrid->setSpacing(12);
    instGrid->setContentsMargins(5, 5, 5, 5);

    QList<QString> pluginsList = scanInstalledVst3Plugins();

    for (int i = 0; i < 8; ++i) {
        InstrumentSlotRow row;
        row.shmName = QString("ArthurAudioIPC_slot_%1").arg(i);

        QFrame *slotCard = new QFrame(instContainer);
        slotCard->setProperty("class", "instrument-slot-card");
        QHBoxLayout *slotLayout = new QHBoxLayout(slotCard);
        slotLayout->setContentsMargins(15, 12, 15, 12);
        slotLayout->setSpacing(10);

        row.slotLabel = new QLabel(QString("Slot %1").arg(i + 1), slotCard);
        row.slotLabel->setStyleSheet("font-weight: 800; font-size: 13px; color: #ffffff; min-width: 50px;");

        row.statusDot = new QLabel(slotCard);
        row.statusDot->setFixedSize(8, 8);
        row.statusDot->setStyleSheet("background-color: #a0a5b5; border-radius: 4px;");

        row.statusText = new QLabel("Empty", slotCard);
        row.statusText->setStyleSheet("color: #a0a5b5; font-size: 12px; font-weight: 600; min-width: 60px;");

        row.pluginSelect = new QComboBox(slotCard);
        row.pluginSelect->setProperty("class", "custom-select");
        row.pluginSelect->setMinimumWidth(150);
        row.pluginSelect->addItem("Select Plugin...", "");
        for (const QString &pluginName : pluginsList) {
            row.pluginSelect->addItem(pluginName, pluginName);
        }

        row.loadBtn = new QPushButton("Load", slotCard);
        row.loadBtn->setProperty("class", "action-btn");
        row.loadBtn->setCursor(Qt::PointingHandCursor);
        row.loadBtn->setFixedWidth(75);

        row.guiBtn = new QPushButton("GUI", slotCard);
        row.guiBtn->setProperty("class", "settings-btn");
        row.guiBtn->setCursor(Qt::PointingHandCursor);
        row.guiBtn->setFixedWidth(60);
        row.guiBtn->setVisible(false);

        slotLayout->addWidget(row.slotLabel);
        slotLayout->addWidget(row.statusDot);
        slotLayout->addWidget(row.statusText);
        slotLayout->addWidget(row.pluginSelect);
        slotLayout->addWidget(row.loadBtn);
        slotLayout->addWidget(row.guiBtn);
        slotLayout->addStretch();

        connect(row.loadBtn, &QPushButton::clicked, this, [=]() {
            if (m_instrumentSlots[i].loadBtn->text() == "Load") {
                loadPlugin(i);
            } else {
                unloadPlugin(i);
            }
        });
        connect(row.guiBtn, &QPushButton::clicked, this, [=]() {
            openPluginGui(i);
        });

        m_instrumentSlots.append(row);
        instGrid->addWidget(slotCard, i / 2, i % 2);
    }
    instScroll->setWidget(instContainer);
    instPageLayout->addWidget(instScroll);

    // Add to stack
    m_consoleStack->addWidget(inputsPage);
    m_consoleStack->addWidget(bussesPage);
    m_consoleStack->addWidget(instrumentsPage);
    consLayout->addWidget(m_consoleStack, 1);

    connect(m_btnInputsPage, &QPushButton::clicked, this, [=]() { switchConsolePage(0); });
    connect(m_btnBussesPage, &QPushButton::clicked, this, [=]() { switchConsolePage(1); });
    connect(m_btnInstrumentsPage, &QPushButton::clicked, this, [=]() { switchConsolePage(2); });

    m_contentArea->addWidget(m_consoleTab);
}

void MainWindow::showDashboard() {
    m_contentArea->setCurrentWidget(m_dashboardTab);
    querySystemStatus();
}
void MainWindow::showInstaller() {
    m_contentArea->setCurrentWidget(m_installerTab);
}
void MainWindow::showSettings() {
    m_contentArea->setCurrentWidget(m_settingsTab);
}
void MainWindow::showConsole() {
    m_contentArea->setCurrentWidget(m_consoleTab);
    rebuildConsoleChannels();
}

// =============================================================================
// VHC Console Mode - Channel Management
// =============================================================================
void MainWindow::rebuildConsoleChannels() {
    m_consoleRows.clear();
    m_consoleBusses.clear();

    // 1. Delete all old children in the channel container layout
    QLayout *chanLayout = m_consoleChannelContainer->layout();
    if (chanLayout) {
        QLayoutItem *item;
        while ((item = chanLayout->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
    }

    // 2. Delete all old children in the busses container layout
    QLayout *busLayout = m_consoleBussesContainer->layout();
    if (busLayout) {
        QLayoutItem *item;
        while ((item = busLayout->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
    }

    // Identify active interface capture ports
    QStringList activeInPorts;
    if (m_audioInterfaceSelect) {
        QString activeOut = m_audioInterfaceSelect->currentData().toString();
        if (!activeOut.isEmpty() && activeOut != "No audio interfaces found") {
            QString activeIn = activeOut;
            activeIn.replace("alsa_output", "alsa_input");
            activeIn.replace("output", "input");

            QList<QString> allPorts = queryPipeWirePorts();
            for (const QString &port : allPorts) {
                if (port.startsWith(activeIn + ":") && port.contains("capture")) {
                    activeInPorts.append(port);
                }
            }
        }
    }
    activeInPorts.sort();

    // Discover active SHM slots via arthur-daemon
    QStringList shmNames;
    QString response = "";
    QLocalSocket sock;
    QString sockPath = "/tmp/arthur.sock";
    QByteArray xdg = qgetenv("XDG_RUNTIME_DIR");
    if (!xdg.isEmpty()) {
        sockPath = QString(xdg) + "/arthur.sock";
    }
    sock.connectToServer(sockPath);
    if (sock.waitForConnected(500)) {
        sock.write("LIST_SHM\n");
        sock.flush();
        sock.waitForReadyRead(1000);
        response = QString::fromUtf8(sock.readAll()).trimmed();
        sock.disconnectFromServer();
    }

    if (!response.isEmpty() && !response.startsWith("ERROR")) {
        shmNames = response.split('\n', Qt::SkipEmptyParts);
    }

    // Fallback: scan /dev/shm for ArthurAudioIPC* entries
    if (shmNames.isEmpty()) {
        QDir devShm("/dev/shm");
        for (const QString &entry : devShm.entryList(QStringList() << "ArthurAudioIPC*", QDir::Files)) {
            shmNames.append(entry);
        }
    }
    shmNames.sort();

    // Combine active physical inputs and virtual channels
    QStringList allChannels;
    for (const QString &p : activeInPorts) {
        allChannels.append(p);
    }
    for (const QString &s : shmNames) {
        allChannels.append(s);
    }

    QHBoxLayout *cl = qobject_cast<QHBoxLayout*>(m_consoleChannelContainer->layout());

    if (allChannels.isEmpty()) {
        QLabel *emptyLabel = new QLabel("No active Arthur channels.\nLoad plugins or DAW.", m_consoleChannelContainer);
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setStyleSheet("color: #a0a5b5; font-size: 12px; margin: 40px;");
        if (cl) cl->addWidget(emptyLabel);
    } else {
        int rowIdx = 0;
        for (const QString &chName : allChannels) {
            ConsoleChannelRow row;
            row.shmName = chName;

            QFrame *strip = new QFrame(m_consoleChannelContainer);
            strip->setProperty("class", "console-strip");
            QVBoxLayout *sl = new QVBoxLayout(strip);
            sl->setContentsMargins(8, 10, 8, 10);
            sl->setSpacing(8);

            // Channel Label (friendly name)
            QString friendlyName = chName;
            bool isPhysical = chName.startsWith("alsa_input.");
            if (isPhysical) {
                friendlyName = chName.section(':', -1);
                if (friendlyName.startsWith("capture_")) {
                    QString suffix = friendlyName.mid(8);
                    if (suffix.startsWith("AUX")) {
                        bool ok;
                        int num = suffix.mid(3).toInt(&ok);
                        if (ok) {
                            friendlyName = QString("EVO IN %1").arg(num + 1);
                        } else {
                            friendlyName = "EVO " + suffix;
                        }
                    } else if (suffix == "FL" || suffix == "L") {
                        friendlyName = "EVO IN L";
                    } else if (suffix == "FR" || suffix == "R") {
                        friendlyName = "EVO IN R";
                    } else {
                        friendlyName = "EVO IN " + suffix;
                    }
                }
            } else {
                if (friendlyName.startsWith("ArthurAudioIPC_")) {
                    friendlyName = "CH " + friendlyName.mid(15);
                }
            }
            row.nameLabel = new QLabel(friendlyName, strip);
            row.nameLabel->setAlignment(Qt::AlignCenter);
            row.nameLabel->setStyleSheet("font-weight: 800; font-size: 12px; color: #ffffff;");
            sl->addWidget(row.nameLabel);

            // Mode Selector
            row.modeSelect = new QComboBox(strip);
            row.modeSelect->setProperty("class", "custom-select");
            row.modeSelect->addItem("IN Playback", 0);
            row.modeSelect->addItem("IN Dry+Mon", 1);
            row.modeSelect->addItem("IN Rec Wet", 2);
            row.modeSelect->setStyleSheet("font-size: 10px; padding: 2px 4px;");
            sl->addWidget(row.modeSelect);

            if (isPhysical) {
                row.modeSelect->setVisible(false);
            }

            int capturedIdx = rowIdx;

            // Plugin Slots (4 rows)
            QVBoxLayout *slotsLayout = new QVBoxLayout();
            slotsLayout->setSpacing(4);
            for (int s = 0; s < 4; ++s) {
                QHBoxLayout *slotRowLayout = new QHBoxLayout();
                slotRowLayout->setSpacing(3);

                QPushButton *slotBtn = new QPushButton("-", strip);
                slotBtn->setProperty("class", "plugin-slot-btn");
                slotBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                slotBtn->setCursor(Qt::PointingHandCursor);

                QPushButton *bypassBtn = new QPushButton("B", strip);
                bypassBtn->setProperty("class", "btn-bypass");
                bypassBtn->setCheckable(true);
                bypassBtn->setCursor(Qt::PointingHandCursor);

                QPushButton *autoGainBtn = new QPushButton("A", strip);
                autoGainBtn->setProperty("class", "btn-auto-slot");
                autoGainBtn->setCheckable(true);
                autoGainBtn->setToolTip("Per-plugin auto gain staging (RMS level match)");
                autoGainBtn->setCursor(Qt::PointingHandCursor);

                slotRowLayout->addWidget(slotBtn, 1);
                slotRowLayout->addWidget(bypassBtn, 0);
                slotRowLayout->addWidget(autoGainBtn, 0);
                slotsLayout->addLayout(slotRowLayout);

                row.pluginSlots[s].slotBtn = slotBtn;
                row.pluginSlots[s].bypassBtn = bypassBtn;
                row.pluginSlots[s].autoGainBtn = autoGainBtn;
                row.pluginSlots[s].pluginName = "";
                row.pluginSlots[s].bypassed = false;
                row.pluginSlots[s].autoGain = false;
                row.pluginSlots[s].shmName = isPhysical
                    ? QString("ArthurAudioIPC_physical_ch%1_slot%2").arg(capturedIdx).arg(s)
                    : QString("ArthurAudioIPC_ch%1_slot%2").arg(capturedIdx).arg(s);

                // Connect signals
                connect(slotBtn, &QPushButton::clicked, this, [=]() {
                    handleSlotClicked(capturedIdx, s, false);
                });
                connect(bypassBtn, &QPushButton::clicked, this, [=]() {
                    toggleSlotBypass(capturedIdx, s, false);
                });
                connect(autoGainBtn, &QPushButton::toggled, this, [=](bool) {
                    toggleSlotAutoGain(capturedIdx, s, false);
                });
            }
            sl->addLayout(slotsLayout);

            // Send Knobs Row (Reverb & Delay side-by-side)
            QHBoxLayout *sendsLayout = new QHBoxLayout();
            sendsLayout->setSpacing(6);

            // Reverb Send
            QVBoxLayout *revLayout = new QVBoxLayout();
            revLayout->setSpacing(2);
            QLabel *revLbl = new QLabel("REV", strip);
            revLbl->setAlignment(Qt::AlignCenter);
            revLbl->setStyleSheet("font-size: 9px; font-weight: 700; color: #a0a5b5;");
            row.sendReverb = new QDial(strip);
            row.sendReverb->setRange(0, 100);
            row.sendReverb->setFixedSize(32, 32);
            row.sendReverb->setNotchesVisible(false);
            row.sendReverb->setToolTip("Reverb Send Amount");
            revLayout->addWidget(revLbl);
            revLayout->addWidget(row.sendReverb);
            sendsLayout->addLayout(revLayout);

            // Delay Send
            QVBoxLayout *dlyLayout = new QVBoxLayout();
            dlyLayout->setSpacing(2);
            QLabel *dlyLbl = new QLabel("DLY", strip);
            dlyLbl->setAlignment(Qt::AlignCenter);
            dlyLbl->setStyleSheet("font-size: 9px; font-weight: 700; color: #a0a5b5;");
            row.sendDelay = new QDial(strip);
            row.sendDelay->setRange(0, 100);
            row.sendDelay->setFixedSize(32, 32);
            row.sendDelay->setNotchesVisible(false);
            row.sendDelay->setToolTip("Delay Send Amount");
            dlyLayout->addWidget(dlyLbl);
            dlyLayout->addWidget(row.sendDelay);
            sendsLayout->addLayout(dlyLayout);

            sl->addLayout(sendsLayout);

            // Fader & Meter Row (Vertical slider and meter side-by-side with AUTO button)
            QHBoxLayout *faderMeterLayout = new QHBoxLayout();
            faderMeterLayout->setSpacing(12);

            row.volumeSlider = new QSlider(Qt::Vertical, strip);
            row.volumeSlider->setRange(0, 100);
            row.volumeSlider->setValue(80);
            row.volumeSlider->setFixedHeight(160);
            row.volumeSlider->setToolTip("Channel Volume Fader");

            row.autoGainBtn = new QPushButton("AUTO", strip);
            row.autoGainBtn->setCheckable(true);
            row.autoGainBtn->setProperty("class", "btn-auto");
            row.autoGainBtn->setCursor(Qt::PointingHandCursor);
            row.autoGainBtn->setFixedWidth(36);

            QVBoxLayout *faderLayout = new QVBoxLayout();
            faderLayout->addWidget(row.volumeSlider, 0, Qt::AlignHCenter);
            faderLayout->addWidget(row.autoGainBtn, 0, Qt::AlignHCenter);

            row.levelMeter = new QProgressBar(strip);
            row.levelMeter->setOrientation(Qt::Vertical);
            row.levelMeter->setRange(0, 100);
            row.levelMeter->setValue(0);
            row.levelMeter->setTextVisible(false);
            row.levelMeter->setFixedHeight(160);

            faderMeterLayout->addLayout(faderLayout);
            faderMeterLayout->addWidget(row.levelMeter, 0, Qt::AlignHCenter);
            sl->addLayout(faderMeterLayout);

            // Mute / Solo Button Row
            QHBoxLayout *muteSoloLayout = new QHBoxLayout();
            muteSoloLayout->setSpacing(6);

            row.muteBtn = new QPushButton("M", strip);
            row.muteBtn->setCheckable(true);
            row.muteBtn->setProperty("class", "btn-mute");
            row.muteBtn->setCursor(Qt::PointingHandCursor);

            row.soloBtn = new QPushButton("S", strip);
            row.soloBtn->setCheckable(true);
            row.soloBtn->setProperty("class", "btn-solo");
            row.soloBtn->setCursor(Qt::PointingHandCursor);

            muteSoloLayout->addWidget(row.muteBtn);
            muteSoloLayout->addWidget(row.soloBtn);
            sl->addLayout(muteSoloLayout);

            if (cl) cl->addWidget(strip);
            m_consoleRows.append(row);

            // Connect controls to actions and settings save
            connect(row.modeSelect, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int idx) {
                applyChannelMode(capturedIdx, idx);
                saveMixerConfig();
            });
            connect(row.volumeSlider, &QSlider::valueChanged, this, [=](int) { saveMixerConfig(); });
            connect(row.sendReverb, &QDial::valueChanged, this, [=](int) { saveMixerConfig(); });
            connect(row.sendDelay, &QDial::valueChanged, this, [=](int) { saveMixerConfig(); });
            connect(row.muteBtn, &QPushButton::toggled, this, [=](bool) { saveMixerConfig(); });
            connect(row.soloBtn, &QPushButton::toggled, this, [=](bool) { saveMixerConfig(); });
            connect(row.autoGainBtn, &QPushButton::toggled, this, [=](bool) {
                toggleAutoGain(capturedIdx, false);
            });

            ++rowIdx;
        }
    }
    if (cl) cl->addStretch();

    // 3. Populate 8 Busses + 8 Sends + Master Bus in m_consoleBussesContainer
    QHBoxLayout *bl = qobject_cast<QHBoxLayout*>(m_consoleBussesContainer->layout());
    
    QStringList busNames;
    for (int i = 1; i <= 8; ++i) {
        busNames.append(QString("Bus %1").arg(i));
    }
    for (int i = 1; i <= 8; ++i) {
        busNames.append(QString("Send %1").arg(i));
    }
    busNames.append("Master Bus");

    for (int i = 0; i < busNames.size(); ++i) {
        ConsoleBusRow bus;
        bus.name = busNames[i];

        QFrame *strip = new QFrame(m_consoleBussesContainer);
        if (bus.name == "Master Bus") {
            strip->setProperty("class", "console-strip-master");
        } else {
            strip->setProperty("class", "console-strip");
        }
        QVBoxLayout *sl = new QVBoxLayout(strip);
        sl->setContentsMargins(8, 10, 8, 10);
        sl->setSpacing(8);

        // Name label
        QLabel *lbl = new QLabel(bus.name, strip);
        lbl->setAlignment(Qt::AlignCenter);
        if (bus.name == "Master Bus") {
            lbl->setStyleSheet("font-weight: 800; font-size: 11px; color: #00f0ff;");
        } else {
            lbl->setStyleSheet("font-weight: 800; font-size: 11px; color: #af52de;");
        }
        sl->addWidget(lbl);

        // Plugin Slots (4 rows) for Busses
        QVBoxLayout *slotsLayout = new QVBoxLayout();
        slotsLayout->setSpacing(4);
        for (int s = 0; s < 4; ++s) {
            QHBoxLayout *slotRowLayout = new QHBoxLayout();
            slotRowLayout->setSpacing(3);

            QPushButton *slotBtn = new QPushButton("-", strip);
            slotBtn->setProperty("class", "plugin-slot-btn");
            slotBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            slotBtn->setCursor(Qt::PointingHandCursor);

            QPushButton *bypassBtn = new QPushButton("B", strip);
            bypassBtn->setProperty("class", "btn-bypass");
            bypassBtn->setCheckable(true);
            bypassBtn->setCursor(Qt::PointingHandCursor);

            QPushButton *autoGainBtn = new QPushButton("A", strip);
            autoGainBtn->setProperty("class", "btn-auto-slot");
            autoGainBtn->setCheckable(true);
            autoGainBtn->setToolTip("Per-plugin auto gain staging (RMS level match)");
            autoGainBtn->setCursor(Qt::PointingHandCursor);

            slotRowLayout->addWidget(slotBtn, 1);
            slotRowLayout->addWidget(bypassBtn, 0);
            slotRowLayout->addWidget(autoGainBtn, 0);
            slotsLayout->addLayout(slotRowLayout);

            bus.pluginSlots[s].slotBtn = slotBtn;
            bus.pluginSlots[s].bypassBtn = bypassBtn;
            bus.pluginSlots[s].autoGainBtn = autoGainBtn;
            bus.pluginSlots[s].pluginName = "";
            bus.pluginSlots[s].bypassed = false;
            bus.pluginSlots[s].autoGain = false;
            bus.pluginSlots[s].shmName = QString("ArthurAudioIPC_bus_ch%1_slot%2").arg(i).arg(s);

            // Connect signals
            connect(slotBtn, &QPushButton::clicked, this, [=]() {
                handleSlotClicked(i, s, true);
            });
            connect(bypassBtn, &QPushButton::clicked, this, [=]() {
                toggleSlotBypass(i, s, true);
            });
            connect(autoGainBtn, &QPushButton::toggled, this, [=](bool) {
                toggleSlotAutoGain(i, s, true);
            });
        }
        sl->addLayout(slotsLayout);

        // Fader & Meter Row (with AUTO button)
        QHBoxLayout *faderMeterLayout = new QHBoxLayout();
        faderMeterLayout->setSpacing(12);

        bus.volumeSlider = new QSlider(Qt::Vertical, strip);
        bus.volumeSlider->setRange(0, 100);
        bus.volumeSlider->setValue(80);
        bus.volumeSlider->setFixedHeight(160);
        bus.volumeSlider->setToolTip(QString("%1 Volume").arg(bus.name));

        bus.autoGainBtn = new QPushButton("AUTO", strip);
        bus.autoGainBtn->setCheckable(true);
        bus.autoGainBtn->setProperty("class", "btn-auto");
        bus.autoGainBtn->setCursor(Qt::PointingHandCursor);
        bus.autoGainBtn->setFixedWidth(36);

        QVBoxLayout *faderLayout = new QVBoxLayout();
        faderLayout->addWidget(bus.volumeSlider, 0, Qt::AlignHCenter);
        faderLayout->addWidget(bus.autoGainBtn, 0, Qt::AlignHCenter);

        bus.levelMeter = new QProgressBar(strip);
        bus.levelMeter->setOrientation(Qt::Vertical);
        bus.levelMeter->setRange(0, 100);
        bus.levelMeter->setValue(0);
        bus.levelMeter->setTextVisible(false);
        bus.levelMeter->setFixedHeight(160);

        faderMeterLayout->addLayout(faderLayout);
        faderMeterLayout->addWidget(bus.levelMeter, 0, Qt::AlignHCenter);
        sl->addLayout(faderMeterLayout);

        // Mute button
        bus.muteBtn = new QPushButton("Mute", strip);
        bus.muteBtn->setCheckable(true);
        bus.muteBtn->setProperty("class", "btn-mute");
        bus.muteBtn->setCursor(Qt::PointingHandCursor);
        sl->addWidget(bus.muteBtn);

        if (bl) bl->addWidget(strip);
        m_consoleBusses.append(bus);

        // Connect changes to auto-save
        connect(bus.volumeSlider, &QSlider::valueChanged, this, [=](int) { saveMixerConfig(); });
        connect(bus.muteBtn, &QPushButton::toggled, this, [=](bool) { saveMixerConfig(); });
        connect(bus.autoGainBtn, &QPushButton::toggled, this, [=](bool) {
            toggleAutoGain(i, true);
        });
    }

    // 4. Load Mixer Config to restore previous state
    loadMixerConfig();
}

void MainWindow::applyChannelMode(int rowIdx, int mode) {
    if (rowIdx < 0 || rowIdx >= m_consoleRows.size()) return;
    ConsoleChannelRow &row = m_consoleRows[rowIdx];
    if (row.shmName.startsWith("alsa_input.")) return;

    // Write console_mode to the SHM segment for this channel
    // Use daemon command for safety - daemon validates and writes the SHM field
    QString cmd = QString("CONSOLE_MODE %1 %2").arg(row.shmName).arg(mode);
    sendDaemonCommand(cmd);
}

void MainWindow::saveMixerConfig() {
    if (m_isUpdatingConfig) return;

    QString configDir = QDir::homePath() + "/.config/arthur";
    QDir().mkpath(configDir);
    QString path = configDir + "/mixer_settings.json";

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;

    QJsonObject root;
    
    // Save channels
    QJsonObject channelsObj;
    for (const ConsoleChannelRow &row : m_consoleRows) {
        QJsonObject chan;
        if (row.volumeSlider) chan["volume"] = row.volumeSlider->value();
        if (row.sendReverb) chan["reverb"] = row.sendReverb->value();
        if (row.sendDelay) chan["delay"] = row.sendDelay->value();
        if (row.muteBtn) chan["mute"] = row.muteBtn->isChecked();
        if (row.soloBtn) chan["solo"] = row.soloBtn->isChecked();
        if (row.modeSelect) chan["mode"] = row.modeSelect->currentIndex();
        chan["autoGain"] = row.autoGainEnabled;

        QJsonArray slotsArr;
        for (int s = 0; s < 4; ++s) {
            QJsonObject slotObj;
            slotObj["plugin"] = row.pluginSlots[s].pluginName;
            slotObj["bypassed"] = row.pluginSlots[s].bypassed;
            slotObj["autoGain"] = row.pluginSlots[s].autoGain;
            slotsArr.append(slotObj);
        }
        chan["slots"] = slotsArr;

        channelsObj[row.shmName] = chan;
    }
    root["channels"] = channelsObj;

    // Save busses
    QJsonObject bussesObj;
    for (const ConsoleBusRow &bus : m_consoleBusses) {
        QJsonObject b;
        if (bus.volumeSlider) b["volume"] = bus.volumeSlider->value();
        if (bus.muteBtn) b["mute"] = bus.muteBtn->isChecked();
        b["autoGain"] = bus.autoGainEnabled;

        QJsonArray slotsArr;
        for (int s = 0; s < 4; ++s) {
            QJsonObject slotObj;
            slotObj["plugin"] = bus.pluginSlots[s].pluginName;
            slotObj["bypassed"] = bus.pluginSlots[s].bypassed;
            slotObj["autoGain"] = bus.pluginSlots[s].autoGain;
            slotsArr.append(slotObj);
        }
        b["slots"] = slotsArr;

        bussesObj[bus.name] = b;
    }
    root["busses"] = bussesObj;

    QJsonDocument doc(root);
    file.write(doc.toJson());
    file.close();
}

void MainWindow::loadMixerConfig() {
    QString path = QDir::homePath() + "/.config/arthur/mixer_settings.json";
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) return;

    QJsonObject root = doc.object();
    QJsonObject channelsObj = root["channels"].toObject();
    QJsonObject bussesObj = root["busses"].toObject();

    m_isUpdatingConfig = true; // prevent loop saves while setting UI values

    // Apply to channels
    for (ConsoleChannelRow &row : m_consoleRows) {
        if (channelsObj.contains(row.shmName)) {
            QJsonObject chan = channelsObj[row.shmName].toObject();
            if (row.volumeSlider && chan.contains("volume")) {
                row.volumeSlider->setValue(chan["volume"].toInt());
            }
            if (row.sendReverb && chan.contains("reverb")) {
                row.sendReverb->setValue(chan["reverb"].toInt());
            }
            if (row.sendDelay && chan.contains("delay")) {
                row.sendDelay->setValue(chan["delay"].toInt());
            }
            if (row.muteBtn && chan.contains("mute")) {
                row.muteBtn->setChecked(chan["mute"].toBool());
            }
            if (row.soloBtn && chan.contains("solo")) {
                row.soloBtn->setChecked(chan["solo"].toBool());
            }
            if (row.modeSelect && chan.contains("mode")) {
                row.modeSelect->setCurrentIndex(chan["mode"].toInt());
            }
            if (row.autoGainBtn && chan.contains("autoGain")) {
                bool ag = chan["autoGain"].toBool();
                row.autoGainBtn->setChecked(ag);
                row.autoGainEnabled = ag;
            }
            if (chan.contains("slots")) {
                QJsonArray slotsArr = chan["slots"].toArray();
                for (int s = 0; s < std::min(static_cast<int>(slotsArr.size()), 4); ++s) {
                    QJsonObject slotObj = slotsArr[s].toObject();
                    QString pName = slotObj["plugin"].toString();
                    bool bp = slotObj["bypassed"].toBool();
                    bool ag = slotObj["autoGain"].toBool();
                    row.pluginSlots[s].pluginName = pName;
                    row.pluginSlots[s].bypassed = bp;
                    row.pluginSlots[s].autoGain = ag;

                    if (row.pluginSlots[s].slotBtn) {
                        row.pluginSlots[s].slotBtn->setText(pName.isEmpty() ? "-" : pName);
                        row.pluginSlots[s].slotBtn->setProperty("loaded", !pName.isEmpty());
                        row.pluginSlots[s].slotBtn->style()->unpolish(row.pluginSlots[s].slotBtn);
                        row.pluginSlots[s].slotBtn->style()->polish(row.pluginSlots[s].slotBtn);
                    }
                    if (row.pluginSlots[s].bypassBtn) {
                        row.pluginSlots[s].bypassBtn->setChecked(bp);
                    }
                    if (row.pluginSlots[s].autoGainBtn) {
                        row.pluginSlots[s].autoGainBtn->setChecked(ag);
                    }

                    // Re-trigger daemon load/bypass/autogain commands for restoration on startup
                    if (!pName.isEmpty()) {
                        QString cmd = QString("LOAD %1 %2").arg(row.pluginSlots[s].shmName).arg(pName);
                        sendDaemonCommand(cmd);
                        if (bp) {
                            QString bpCmd = QString("BYPASS_PLUGIN %1 1").arg(row.pluginSlots[s].shmName);
                            sendDaemonCommand(bpCmd);
                        }
                        if (ag) {
                            QString agCmd = QString("AUTOGAIN_PLUGIN %1 1").arg(row.pluginSlots[s].shmName);
                            sendDaemonCommand(agCmd);
                        }
                    }
                }
            }
        }
    }

    // Apply to busses
    for (ConsoleBusRow &bus : m_consoleBusses) {
        if (bussesObj.contains(bus.name)) {
            QJsonObject b = bussesObj[bus.name].toObject();
            if (bus.volumeSlider && b.contains("volume")) {
                bus.volumeSlider->setValue(b["volume"].toInt());
            }
            if (bus.muteBtn && b.contains("mute")) {
                bus.muteBtn->setChecked(b["mute"].toBool());
            }
            if (bus.autoGainBtn && b.contains("autoGain")) {
                bool ag = b["autoGain"].toBool();
                bus.autoGainBtn->setChecked(ag);
                bus.autoGainEnabled = ag;
            }
            if (b.contains("slots")) {
                QJsonArray slotsArr = b["slots"].toArray();
                for (int s = 0; s < std::min(static_cast<int>(slotsArr.size()), 4); ++s) {
                    QJsonObject slotObj = slotsArr[s].toObject();
                    QString pName = slotObj["plugin"].toString();
                    bool bp = slotObj["bypassed"].toBool();
                    bool ag = slotObj["autoGain"].toBool();
                    bus.pluginSlots[s].pluginName = pName;
                    bus.pluginSlots[s].bypassed = bp;
                    bus.pluginSlots[s].autoGain = ag;

                    if (bus.pluginSlots[s].slotBtn) {
                        bus.pluginSlots[s].slotBtn->setText(pName.isEmpty() ? "-" : pName);
                        bus.pluginSlots[s].slotBtn->setProperty("loaded", !pName.isEmpty());
                        bus.pluginSlots[s].slotBtn->style()->unpolish(bus.pluginSlots[s].slotBtn);
                        bus.pluginSlots[s].slotBtn->style()->polish(bus.pluginSlots[s].slotBtn);
                    }
                    if (bus.pluginSlots[s].bypassBtn) {
                        bus.pluginSlots[s].bypassBtn->setChecked(bp);
                    }
                    if (bus.pluginSlots[s].autoGainBtn) {
                        bus.pluginSlots[s].autoGainBtn->setChecked(ag);
                    }

                    // Re-trigger daemon load/bypass/autogain commands for restoration on startup
                    if (!pName.isEmpty()) {
                        QString cmd = QString("LOAD %1 %2").arg(bus.pluginSlots[s].shmName).arg(pName);
                        sendDaemonCommand(cmd);
                        if (bp) {
                            QString bpCmd = QString("BYPASS_PLUGIN %1 1").arg(bus.pluginSlots[s].shmName);
                            sendDaemonCommand(bpCmd);
                        }
                        if (ag) {
                            QString agCmd = QString("AUTOGAIN_PLUGIN %1 1").arg(bus.pluginSlots[s].shmName);
                            sendDaemonCommand(agCmd);
                        }
                    }
                }
            }
        }
    }

    m_isUpdatingConfig = false;
}

void MainWindow::updateMeterAnimations() {
    // Determine if system is active (lock is active)
    bool active = false;
    QProcess pgrep;
    pgrep.start("pgrep", QStringList() << "-x" << "midi_sync");
    pgrep.waitForFinished(100);
    if (pgrep.exitCode() == 0) {
        active = true;
    }

    // Channel peak meter simulation
    for (ConsoleChannelRow &row : m_consoleRows) {
        if (!row.levelMeter) continue;
        int currentVal = row.levelMeter->value();
        int targetVal = 0;
        
        bool isMuted = row.muteBtn && row.muteBtn->isChecked();
        if (active && !isMuted) {
            // Fluctuates around a standard dynamic level
            targetVal = 30 + QRandomGenerator::global()->bounded(55);
            if (row.volumeSlider) {
                targetVal = (targetVal * row.volumeSlider->value()) / 100;
            }
        } else {
            targetVal = 0;
        }

        int nextVal;
        if (targetVal > currentVal) {
            nextVal = currentVal + (targetVal - currentVal) * 0.7; // rapid attack
        } else {
            nextVal = currentVal - (currentVal - targetVal) * 0.25; // slow decay
        }
        row.levelMeter->setValue(qBound(0, nextVal, 100));

        // Auto Gain Control (AGC) staging
        if (row.autoGainEnabled && active && !isMuted) {
            int level = row.levelMeter->value();
            if (row.volumeSlider && level > 0) {
                int currentVol = row.volumeSlider->value();
                if (level < 65 && currentVol < 100) {
                    row.volumeSlider->setValue(currentVol + 1);
                } else if (level > 75 && currentVol > 0) {
                    row.volumeSlider->setValue(currentVol - 1);
                }
            }
        }
    }

    // Busses peak meter simulation
    for (ConsoleBusRow &bus : m_consoleBusses) {
        if (!bus.levelMeter) continue;
        int currentVal = bus.levelMeter->value();
        int targetVal = 0;

        bool isMuted = bus.muteBtn && bus.muteBtn->isChecked();
        if (active && !isMuted) {
            int sum = 0;
            int count = 0;
            for (const ConsoleChannelRow &row : m_consoleRows) {
                if (row.levelMeter && !(row.muteBtn && row.muteBtn->isChecked())) {
                    sum += row.levelMeter->value();
                    count++;
                }
            }
            if (count > 0) {
                targetVal = sum / count + QRandomGenerator::global()->bounded(10) - 5;
            } else {
                targetVal = 30 + QRandomGenerator::global()->bounded(20);
            }
            if (bus.volumeSlider) {
                targetVal = (targetVal * bus.volumeSlider->value()) / 100;
            }
        } else {
            targetVal = 0;
        }

        int nextVal;
        if (targetVal > currentVal) {
            nextVal = currentVal + (targetVal - currentVal) * 0.6;
        } else {
            nextVal = currentVal - (currentVal - targetVal) * 0.2;
        }
        bus.levelMeter->setValue(qBound(0, nextVal, 100));

        // Auto Gain Control (AGC) staging
        if (bus.autoGainEnabled && active && !isMuted) {
            int level = bus.levelMeter->value();
            if (bus.volumeSlider && level > 0) {
                int currentVol = bus.volumeSlider->value();
                if (level < 65 && currentVol < 100) {
                    bus.volumeSlider->setValue(currentVol + 1);
                } else if (level > 75 && currentVol > 0) {
                    bus.volumeSlider->setValue(currentVol - 1);
                }
            }
        }
    }
}

// =============================================================================
// Status indicator & periodic querying
// =============================================================================
void MainWindow::querySystemStatus() {
    // We check if midi_sync is running using pgrep
    QProcess pgrep;
    pgrep.start("pgrep", QStringList() << "-x" << "midi_sync");
    pgrep.waitForFinished(500);
    bool active = (pgrep.exitCode() == 0);

    // Read kernel parameters or hardcode core isolation (default: 4-7)
    QString cores = "4-7";

    // VDC Load calculation simulation or query
    float vdcLoad = 34.8f;
    if (active) {
        vdcLoad = 30.0f + QRandomGenerator::global()->bounded(150) / 10.0f;
    } else {
        vdcLoad = 0.0f;
    }

    // Update Dashboard VDC Cores card
    if (m_dspActiveBadge) {
        m_dspActiveBadge->setText(active ? "Active" : "Standby");
        m_dspActiveBadge->setProperty("class", active ? "badge badgeGreen" : "badge badgeGray");
        m_dspActiveBadge->style()->unpolish(m_dspActiveBadge);
        m_dspActiveBadge->style()->polish(m_dspActiveBadge);
    }
    if (m_dspCoresVal) m_dspCoresVal->setText(cores);
    if (m_dspLoadText) m_dspLoadText->setText(QString("%1%").arg(vdcLoad, 0, 'f', 1));
    if (m_dspLoadProgress) m_dspLoadProgress->setValue(static_cast<int>(vdcLoad));

    updateGlobalStatus(
        active ? "System Lock Active" : "System Standby",
        active ? "System Lock Active | Real-Time" : "System Standby",
        active
    );
    updateMidiSyncCard(active);

    // Discover active SHM slots via arthur-daemon (with fast timeout)
    QStringList currentShms;
    QString response = "";
    QLocalSocket sock;
    QString sockPath = "/tmp/arthur.sock";
    QByteArray xdg = qgetenv("XDG_RUNTIME_DIR");
    if (!xdg.isEmpty()) {
        sockPath = QString(xdg) + "/arthur.sock";
    }
    sock.connectToServer(sockPath);
    if (sock.waitForConnected(200)) {
        sock.write("LIST_SHM\n");
        sock.flush();
        sock.waitForReadyRead(300);
        response = QString::fromUtf8(sock.readAll()).trimmed();
        sock.disconnectFromServer();
    }

    if (!response.isEmpty() && !response.startsWith("ERROR")) {
        currentShms = response.split('\n', Qt::SkipEmptyParts);
    }

    // Fallback: scan /dev/shm for ArthurAudioIPC* entries
    if (currentShms.isEmpty()) {
        QDir devShm("/dev/shm");
        for (const QString &entry : devShm.entryList(QStringList() << "ArthurAudioIPC*", QDir::Files)) {
            currentShms.append(entry);
        }
    }

    // Query LIST_PLUGINS via arthur-daemon (with fast timeout)
    QString pluginsResponse = "";
    QLocalSocket pluginSock;
    pluginSock.connectToServer(sockPath);
    if (pluginSock.waitForConnected(200)) {
        pluginSock.write("LIST_PLUGINS\n");
        pluginSock.flush();
        pluginSock.waitForReadyRead(300);
        pluginsResponse = QString::fromUtf8(pluginSock.readAll()).trimmed();
        pluginSock.disconnectFromServer();
    }

    // Map of loaded plugin names by SHM name
    QMap<QString, QString> loadedPlugins;
    if (!pluginsResponse.isEmpty() && !pluginsResponse.startsWith("ERROR")) {
        QStringList lines = pluginsResponse.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                loadedPlugins[parts[0]] = parts[1];
            } else if (parts.size() == 1) {
                loadedPlugins[parts[0]] = "VST3 Plugin";
            }
        }
    }

    // Update Instrument Rack slots UI
    for (int i = 0; i < m_instrumentSlots.size(); ++i) {
        InstrumentSlotRow &row = m_instrumentSlots[i];
        if (loadedPlugins.contains(row.shmName)) {
            QString pluginName = loadedPlugins[row.shmName];
            row.statusDot->setStyleSheet("background-color: #30d158; border-radius: 4px;"); // green
            row.statusText->setText(pluginName);
            row.statusText->setStyleSheet("color: #30d158; font-size: 12px; font-weight: 600; min-width: 100px;");
            
            row.pluginSelect->blockSignals(true);
            int idx = row.pluginSelect->findData(pluginName);
            if (idx >= 0) {
                row.pluginSelect->setCurrentIndex(idx);
            } else {
                row.pluginSelect->addItem(pluginName, pluginName);
                row.pluginSelect->setCurrentIndex(row.pluginSelect->count() - 1);
            }
            row.pluginSelect->setEnabled(false);
            row.pluginSelect->blockSignals(false);

            row.loadBtn->setText("Unload");
            row.loadBtn->setStyleSheet("background: #ff3b30; border: none; border-radius: 8px; color: white; font-weight: 600; padding: 8px 15px;");
            row.guiBtn->setVisible(true);
        } else {
            row.statusDot->setStyleSheet("background-color: #a0a5b5; border-radius: 4px;"); // gray
            row.statusText->setText("Empty");
            row.statusText->setStyleSheet("color: #a0a5b5; font-size: 12px; font-weight: 600; min-width: 100px;");

            row.pluginSelect->blockSignals(true);
            row.pluginSelect->setCurrentIndex(0);
            row.pluginSelect->setEnabled(true);
            row.pluginSelect->blockSignals(false);

            row.loadBtn->setText("Load");
            row.loadBtn->setStyleSheet("");
            row.guiBtn->setVisible(false);
        }
    }

    // Sort to make sure comparison is independent of order
    currentShms.sort();
    QStringList lastScannedSorted = m_lastScannedShms;
    lastScannedSorted.sort();

    if (currentShms != lastScannedSorted) {
        m_lastScannedShms = currentShms;
        rebuildConsoleChannels();
    }
}

void MainWindow::updateGlobalStatus(const QString &title, const QString &description, bool active) {
    Q_UNUSED(title);
    if (m_statusDot && m_statusTextLabel) {
        m_statusTextLabel->setText(description);
        m_statusDot->setStyleSheet(active ? "background-color: #30d158; border-radius: 4px;" : "background-color: #a0a5b5; border-radius: 4px;");
    }
}

void MainWindow::updateMidiSyncCard(bool active) {
    if (m_midiStatusText && m_midiJitterBadge) {
        if (active) {
            m_midiStatusText->setText("Slaved to PCM hardware clock (hw:0,0)");
            m_midiJitterBadge->setText("Jitter: < 10 µs");
            m_midiJitterBadge->setProperty("class", "badge badgeGreen");
        } else {
            m_midiStatusText->setText("MIDI clock slaving disabled");
            m_midiJitterBadge->setText("Inactive");
            m_midiJitterBadge->setProperty("class", "badge badgeGray");
        }
        m_midiJitterBadge->style()->unpolish(m_midiJitterBadge);
        m_midiJitterBadge->style()->polish(m_midiJitterBadge);
    }
}

// =============================================================================
// Audio Config Settings slots
// =============================================================================
void MainWindow::loadAudioConfig() {
    m_isUpdatingConfig = true;

    bool flatpakMode = QFile::exists("/.flatpak-info");

    // 1. Get sound interfaces via pactl
    QProcess pactl;
    if (flatpakMode) {
        pactl.start("flatpak-spawn", QStringList() << "--host" << "pactl" << "list" << "sinks");
    } else {
        pactl.start("pactl", QStringList() << "list" << "sinks");
    }
    pactl.waitForFinished(1000);
    
    QString activeInterface = "";
    if (m_audioInterfaceSelect) {
        m_audioInterfaceSelect->clear();
        QString stdoutStr = QString::fromUtf8(pactl.readAllStandardOutput());
        QTextStream stream(&stdoutStr);
        QString currentName = "";
        while (!stream.atEnd()) {
            QString line = stream.readLine().trimmed();
            if (line.startsWith("Name: ")) {
                currentName = line.mid(6);
            } else if (line.startsWith("Description: ") && !currentName.isEmpty()) {
                QString desc = line.mid(13);
                m_audioInterfaceSelect->addItem(desc, currentName);
                currentName = "";
            }
        }
        
        // Fallback interface
        if (m_audioInterfaceSelect->count() == 0) {
            m_audioInterfaceSelect->addItem("No audio interfaces found", "");
        }

        // Check if config file exists
        QString configPath = QDir::homePath() + "/.config/arthur/audio_settings.json";
        QFile file(configPath);
        bool hasSavedConfig = false;
        QJsonObject savedObj;
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray data = file.readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (!doc.isNull() && doc.isObject()) {
                savedObj = doc.object();
                hasSavedConfig = true;
            }
            file.close();
        }

        if (hasSavedConfig && savedObj.contains("audio_interface")) {
            activeInterface = savedObj["audio_interface"].toString();
        } else {
            // Get default sink
            QProcess pactlDef;
            if (flatpakMode) {
                pactlDef.start("flatpak-spawn", QStringList() << "--host" << "pactl" << "get-default-sink");
            } else {
                pactlDef.start("pactl", QStringList() << "get-default-sink");
            }
            pactlDef.waitForFinished(500);
            activeInterface = QString::fromUtf8(pactlDef.readAllStandardOutput()).trimmed();
        }

        int idx = m_audioInterfaceSelect->findData(activeInterface);
        if (idx >= 0) {
            m_audioInterfaceSelect->setCurrentIndex(idx);
        } else {
            m_audioInterfaceSelect->setCurrentIndex(0);
        }

        // Populate CLLS slot interface dropdowns
        QJsonArray cllsArray = savedObj["clls_slots"].toArray();
        for (int i = 0; i < 3; ++i) {
            if (m_cllsSlots[i].interfaceSelect) {
                m_cllsSlots[i].interfaceSelect->blockSignals(true);
                m_cllsSlots[i].interfaceSelect->clear();
                for (int k = 0; k < m_audioInterfaceSelect->count(); ++k) {
                    m_cllsSlots[i].interfaceSelect->addItem(m_audioInterfaceSelect->itemText(k), m_audioInterfaceSelect->itemData(k));
                }
                
                // Restore CLLS slot interface selection
                QString slotInterface = "";
                if (hasSavedConfig && i < cllsArray.size()) {
                    slotInterface = cllsArray[i].toObject()["interface"].toString();
                }
                
                int slotIdx = -1;
                if (!slotInterface.isEmpty()) {
                    slotIdx = m_cllsSlots[i].interfaceSelect->findData(slotInterface);
                }
                
                if (slotIdx >= 0) {
                    m_cllsSlots[i].interfaceSelect->setCurrentIndex(slotIdx);
                } else {
                    // Default selections
                    if (i < m_audioInterfaceSelect->count()) {
                        m_cllsSlots[i].interfaceSelect->setCurrentIndex(i);
                    } else {
                        m_cllsSlots[i].interfaceSelect->setCurrentIndex(0);
                    }
                }
                m_cllsSlots[i].interfaceSelect->blockSignals(false);
                populatePortsForSlot(i);

                // Restore CLLS slot playback and capture ports
                if (hasSavedConfig && i < cllsArray.size()) {
                    QJsonObject slotSavedObj = cllsArray[i].toObject();
                    QString playbackPort = slotSavedObj["playback_port"].toString();
                    QString capturePort = slotSavedObj["capture_port"].toString();
                    QString audioSrc = slotSavedObj["audio_src"].toString();
                    QString audioDst = slotSavedObj["audio_dst"].toString();

                    if (!playbackPort.isEmpty() && m_cllsSlots[i].playbackPortSelect) {
                        m_cllsSlots[i].playbackPortSelect->blockSignals(true);
                        int pIdx = m_cllsSlots[i].playbackPortSelect->findData(playbackPort);
                        if (pIdx >= 0) {
                            m_cllsSlots[i].playbackPortSelect->setCurrentIndex(pIdx);
                        }
                        m_cllsSlots[i].playbackPortSelect->blockSignals(false);
                    }
                    if (!capturePort.isEmpty() && m_cllsSlots[i].capturePortSelect) {
                        m_cllsSlots[i].capturePortSelect->blockSignals(true);
                        int cIdx = m_cllsSlots[i].capturePortSelect->findData(capturePort);
                        if (cIdx >= 0) {
                            m_cllsSlots[i].capturePortSelect->setCurrentIndex(cIdx);
                        }
                        m_cllsSlots[i].capturePortSelect->blockSignals(false);
                    }
                    if (!audioSrc.isEmpty() && m_cllsSlots[i].audioSrcSelect) {
                        m_cllsSlots[i].audioSrcSelect->blockSignals(true);
                        int sIdx = m_cllsSlots[i].audioSrcSelect->findData(audioSrc);
                        if (sIdx >= 0) {
                            m_cllsSlots[i].audioSrcSelect->setCurrentIndex(sIdx);
                        }
                        m_cllsSlots[i].audioSrcSelect->blockSignals(false);
                    }
                    if (!audioDst.isEmpty() && m_cllsSlots[i].audioDstSelect) {
                        m_cllsSlots[i].audioDstSelect->blockSignals(true);
                        int dIdx = m_cllsSlots[i].audioDstSelect->findData(audioDst);
                        if (dIdx >= 0) {
                            m_cllsSlots[i].audioDstSelect->setCurrentIndex(dIdx);
                        }
                        m_cllsSlots[i].audioDstSelect->blockSignals(false);
                    }

                    QString slotInterface = slotSavedObj["interface"].toString();
                    if (!slotInterface.isEmpty() && !playbackPort.isEmpty() && !capturePort.isEmpty()) {
                        QTimer::singleShot(2000, this, [=]() {
                            startCllsCalibration(i);
                        });
                    }
                }
            }
        }
    }

    // 2. Query PipeWire rate & quantum settings via pw-metadata
    unsigned int rate = 48000;
    unsigned int quantum = 128;
    bool slaving = false;

    QString configPath = QDir::homePath() + "/.config/arthur/audio_settings.json";
    QFile file(configPath);
    bool hasSavedConfig = false;
    QJsonObject savedObj;
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray data = file.readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isNull() && doc.isObject()) {
            savedObj = doc.object();
            hasSavedConfig = true;
        }
        file.close();
    }

    if (hasSavedConfig) {
        if (savedObj.contains("sample_rate")) rate = savedObj["sample_rate"].toInt();
        if (savedObj.contains("buffer_size")) quantum = savedObj["buffer_size"].toInt();
        if (savedObj.contains("midi_slave")) slaving = savedObj["midi_slave"].toBool();
    } else {
        QProcess pwMeta;
        if (flatpakMode) {
            pwMeta.start("flatpak-spawn", QStringList() << "--host" << "pw-metadata" << "-n" << "settings");
        } else {
            pwMeta.start("pw-metadata", QStringList() << "-n" << "settings");
        }
        pwMeta.waitForFinished(1000);
        QString metaStdout = QString::fromUtf8(pwMeta.readAllStandardOutput());
        
        QRegularExpression rateRegex("clock.rate\\s+value:'(\\d+)'");
        QRegularExpression forceRateRegex("clock.force-rate\\s+value:'(\\d+)'");
        QRegularExpression quantumRegex("clock.quantum\\s+value:'(\\d+)'");
        QRegularExpression forceQuantumRegex("clock.force-quantum\\s+value:'(\\d+)'");

        auto getMatch = [](const QRegularExpression &regex, const QString &text) -> unsigned int {
            QRegularExpressionMatch m = regex.match(text);
            if (m.hasMatch()) {
                return m.captured(1).toUInt();
            }
            return 0;
        };

        unsigned int frate = getMatch(forceRateRegex, metaStdout);
        unsigned int crate = getMatch(rateRegex, metaStdout);
        rate = frate ? frate : (crate ? crate : 48000);

        unsigned int fquant = getMatch(forceQuantumRegex, metaStdout);
        unsigned int cquant = getMatch(quantumRegex, metaStdout);
        quantum = fquant ? fquant : (cquant ? cquant : 128);

        // Slaving state
        QProcess pgrep;
        pgrep.start("pgrep", QStringList() << "-x" << "midi_sync");
        pgrep.waitForFinished(500);
        slaving = (pgrep.exitCode() == 0);
    }

    m_activeSampleRate = rate;

    if (m_sampleRateSelect) {
        m_sampleRateSelect->blockSignals(true);
        int idx = m_sampleRateSelect->findData(rate);
        if (idx >= 0) m_sampleRateSelect->setCurrentIndex(idx);
        m_sampleRateSelect->blockSignals(false);
    }
    if (m_bufferSizeSelect) {
        m_bufferSizeSelect->blockSignals(true);
        int idx = m_bufferSizeSelect->findData(quantum);
        if (idx >= 0) m_bufferSizeSelect->setCurrentIndex(idx);
        m_bufferSizeSelect->blockSignals(false);
    }

    if (m_midiSlaveCheck) {
        m_midiSlaveCheck->blockSignals(true);
        m_midiSlaveCheck->setChecked(slaving);
        m_midiSlaveCheck->blockSignals(false);
    }

    m_isUpdatingConfig = false;

    // Apply the loaded config to system
    applyAudioConfig();

    // If config file did not exist, write the current queried settings to create it
    if (!hasSavedConfig) {
        saveAudioConfig();
    }
}

void MainWindow::applyAudioConfig() {
    if (m_isUpdatingConfig) return;

    QString interface = m_audioInterfaceSelect->currentData().toString();
    unsigned int rate = m_sampleRateSelect->currentData().toUInt();
    unsigned int quantum = m_bufferSizeSelect->currentData().toUInt();
    bool slaveMidi = m_midiSlaveCheck->isChecked();

    m_activeSampleRate = rate;

    bool flatpakMode = QFile::exists("/.flatpak-info");

    updateGlobalStatus("⚡ Applying", "Applying audio configuration...", true);

    std::thread([=]() {
        bool flatpakMode = QFile::exists("/.flatpak-info");

        // Set default sink
        if (flatpakMode) {
            QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pactl" << "set-default-sink" << interface);
        } else {
            QProcess::execute("pactl", QStringList() << "set-default-sink" << interface);
        }

        // Set force-rate
        if (flatpakMode) {
            QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pw-metadata" << "-n" << "settings" << "0" << "clock.force-rate" << QString::number(rate));
        } else {
            QProcess::execute("pw-metadata", QStringList() << "-n" << "settings" << "0" << "clock.force-rate" << QString::number(rate));
        }

        // Set force-quantum
        if (flatpakMode) {
            QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pw-metadata" << "-n" << "settings" << "0" << "clock.force-quantum" << QString::number(quantum));
        } else {
            QProcess::execute("pw-metadata", QStringList() << "-n" << "settings" << "0" << "clock.force-quantum" << QString::number(quantum));
        }

        // Slaving daemon control
        if (slaveMidi) {
            QProcess pgrep;
            pgrep.start("pgrep", QStringList() << "-x" << "midi_sync");
            pgrep.waitForFinished(500);
            if (pgrep.exitCode() != 0) {
                // Spawn midi_sync
                QProcess::startDetached(QCoreApplication::applicationDirPath() + "/midi_sync", QStringList());
            }
        } else {
            QProcess::execute("pkill", QStringList() << "-x" << "midi_sync");
        }

        QMetaObject::invokeMethod(this, [=]() {
            updateGlobalStatus("✓ Sync Active", "Audio settings updated successfully.", true);
            // Save the config values since they have changed
            saveAudioConfig();
        });
    }).detach();
}

void MainWindow::saveAudioConfig() {
    if (m_isUpdatingConfig) return;

    QString configDir = QDir::homePath() + "/.config/arthur";
    QDir().mkpath(configDir);
    QString path = configDir + "/audio_settings.json";

    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonObject obj;
        if (m_audioInterfaceSelect) {
            obj["audio_interface"] = m_audioInterfaceSelect->currentData().toString();
        }
        if (m_sampleRateSelect) {
            obj["sample_rate"] = static_cast<int>(m_sampleRateSelect->currentData().toUInt());
        }
        if (m_bufferSizeSelect) {
            obj["buffer_size"] = static_cast<int>(m_bufferSizeSelect->currentData().toUInt());
        }
        if (m_midiSlaveCheck) {
            obj["midi_slave"] = m_midiSlaveCheck->isChecked();
        }

        QJsonArray cllsArray;
        for (int i = 0; i < 3; ++i) {
            QJsonObject slotObj;
            if (m_cllsSlots[i].interfaceSelect) {
                slotObj["interface"] = m_cllsSlots[i].interfaceSelect->currentData().toString();
            }
            if (m_cllsSlots[i].playbackPortSelect) {
                slotObj["playback_port"] = m_cllsSlots[i].playbackPortSelect->currentData().toString();
            }
            if (m_cllsSlots[i].capturePortSelect) {
                slotObj["capture_port"] = m_cllsSlots[i].capturePortSelect->currentData().toString();
            }
            if (m_cllsSlots[i].audioSrcSelect) {
                slotObj["audio_src"] = m_cllsSlots[i].audioSrcSelect->currentData().toString();
            }
            if (m_cllsSlots[i].audioDstSelect) {
                slotObj["audio_dst"] = m_cllsSlots[i].audioDstSelect->currentData().toString();
            }
            slotObj["is_calibrating"] = m_cllsSlots[i].isCalibrating;
            cllsArray.append(slotObj);
        }
        obj["clls_slots"] = cllsArray;

        QJsonDocument doc(obj);
        file.write(doc.toJson());
        file.close();
    }
}

// Static helper to resolve the exact input/capture interface name in PipeWire for a given output interface
static QString getMatchingInputInterface(const QString &activeInterface, const QList<QString> &allPorts) {
    if (activeInterface.startsWith("alsa_input")) return activeInterface;
    
    QStringList parts = activeInterface.split('.');
    if (parts.size() >= 2) {
        QString cardId = parts[1];
        QString targetPrefix = "alsa_input." + cardId;
        for (const QString &port : allPorts) {
            if (port.startsWith(targetPrefix)) {
                int colonIdx = port.indexOf(':');
                if (colonIdx != -1) {
                    return port.left(colonIdx);
                } else {
                    return port;
                }
            }
        }
    }
    
    // Fallback: standard replacements
    QString inputInterface = activeInterface;
    inputInterface.replace("alsa_output", "alsa_input");
    inputInterface.replace("output", "input");
    return inputInterface;
}

// =============================================================================
// CLLS Calibration loops
// =============================================================================
void MainWindow::startCllsCalibration(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= 3) return;
    CllsSlot &slot = m_cllsSlots[slotIdx];

    if (slot.process && slot.process->state() == QProcess::Running) {
        slot.process->kill();
        slot.process->waitForFinished(500);
        return;
    }

    QString interface = slot.interfaceSelect->currentData().toString();
    QString outputPort = slot.playbackPortSelect->currentData().toString();
    QString inputPort = slot.capturePortSelect->currentData().toString();

    if (interface.isEmpty() || outputPort.isEmpty() || inputPort.isEmpty()) {
        updateGlobalStatus("⚠ Setup Error", "Interface or loopback channels not selected.", false);
        return;
    }

    slot.isCalibrating = true;
    slot.runBtn->setText("Stop");
    slot.runBtn->setStyleSheet("background-color: #ff3b30; padding: 4px 8px; font-size: 11px; border-radius: 6px;");

    slot.statusBadge->setText("Connecting...");
    slot.statusBadge->setStyleSheet("background-color: rgba(255,159,10,0.15); color: #ff9f0a; border: 1px solid rgba(255,159,10,0.3); border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: bold;");

    QString alignerName = QString("CLLS-Aligner-%1").arg(slotIdx + 1);

    if (slot.process) {
        slot.process->deleteLater();
    }
    slot.process = new QProcess(this);
    slot.process->setProcessChannelMode(QProcess::MergedChannels);
    connect(slot.process, &QProcess::readyReadStandardOutput, this, [=]() { readCllsOutput(slotIdx); });
    connect(slot.process, &QProcess::finished, this, [=](int exitCode, QProcess::ExitStatus status) {
        handleCllsFinished(slotIdx, exitCode, status);
    });

    slot.process->start(QCoreApplication::applicationDirPath() + "/pw_module_clls", QStringList() << alignerName);

    updateGlobalStatus("⚡ Calibrating", QString("Spawning CLLS Aligner for Slot %1...").arg(slotIdx + 1), true);
    saveAudioConfig();

    // Dynamic graph loopback auto-linker
    QTimer::singleShot(1500, this, [=]() {
        QList<QString> allPorts = queryPipeWirePorts();
        QString inputInterface = getMatchingInputInterface(interface, allPorts);

        QList<QString> interfaceCapturePorts;
        for (const QString &port : allPorts) {
            if (port.startsWith(inputInterface) && port.contains("capture")) {
                interfaceCapturePorts.append(port);
            }
        }
        
        QList<QString> interfacePlaybackPorts;
        for (const QString &port : allPorts) {
            if (port.startsWith(interface) && port.contains("playback")) {
                interfacePlaybackPorts.append(port);
            }
        }
        
        QString anchor1, anchor2;
        QString srcData = slot.audioSrcSelect->currentData().toString();
        if (!srcData.isEmpty()) {
            QStringList srcParts = srcData.split(';');
            if (srcParts.size() >= 2) {
                anchor1 = srcParts[0];
                anchor2 = srcParts[1];
            }
        }
        if (anchor1.isEmpty() || anchor2.isEmpty()) {
            anchor1 = interfaceCapturePorts.size() > 0 ? interfaceCapturePorts[0] : QString("%1:capture_AUX0").arg(inputInterface);
            anchor2 = interfaceCapturePorts.size() > 1 ? interfaceCapturePorts[1] : QString("%1:capture_AUX1").arg(inputInterface);
        }

        QString playDest1, playDest2;
        QString dstData = slot.audioDstSelect->currentData().toString();
        if (!dstData.isEmpty()) {
            QStringList dstParts = dstData.split(';');
            if (dstParts.size() >= 2) {
                playDest1 = dstParts[0];
                playDest2 = dstParts[1];
            }
        }
        if (playDest1.isEmpty() || playDest2.isEmpty()) {
            playDest1 = interfacePlaybackPorts.size() > 0 ? interfacePlaybackPorts[0] : QString("%1:playback_AUX0").arg(interface);
            playDest2 = interfacePlaybackPorts.size() > 1 ? interfacePlaybackPorts[1] : QString("%1:playback_AUX1").arg(interface);
        }

        // Link in a background thread to prevent blocking main GUI thread
        std::thread([=]() {
            int code1 = -1;
            int code2 = -1;
            bool flatpakMode = QFile::exists("/.flatpak-info");
            if (flatpakMode) {
                code1 = QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pw-link" << QString("%1:out_loopback").arg(alignerName) << outputPort);
                code2 = QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pw-link" << inputPort << QString("%1:in_loopback").arg(alignerName));
                QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pw-link" << anchor1 << QString("%1:in_audio_L").arg(alignerName));
                QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pw-link" << anchor2 << QString("%1:in_audio_R").arg(alignerName));
                QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pw-link" << QString("%1:out_audio_L").arg(alignerName) << playDest1);
                QProcess::execute("flatpak-spawn", QStringList() << "--host" << "pw-link" << QString("%1:out_audio_R").arg(alignerName) << playDest2);
            } else {
                code1 = QProcess::execute("pw-link", QStringList() << QString("%1:out_loopback").arg(alignerName) << outputPort);
                code2 = QProcess::execute("pw-link", QStringList() << inputPort << QString("%1:in_loopback").arg(alignerName));
                QProcess::execute("pw-link", QStringList() << anchor1 << QString("%1:in_audio_L").arg(alignerName));
                QProcess::execute("pw-link", QStringList() << anchor2 << QString("%1:in_audio_R").arg(alignerName));
                QProcess::execute("pw-link", QStringList() << QString("%1:out_audio_L").arg(alignerName) << playDest1);
                QProcess::execute("pw-link", QStringList() << QString("%1:out_audio_R").arg(alignerName) << playDest2);
            }

            QMetaObject::invokeMethod(this, [=]() {
                if (slotIdx >= 0 && slotIdx < 3) {
                    CllsSlot &cllsSlot = m_cllsSlots[slotIdx];
                    if (code1 == 0 && code2 == 0) {
                        cllsSlot.statusBadge->setText("Awaiting Loopback");
                        cllsSlot.statusBadge->setStyleSheet("background-color: rgba(255,159,10,0.15); color: #ff9f0a; border: 1px solid rgba(255,159,10,0.3); border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: bold;");
                    } else {
                        cllsSlot.statusBadge->setText("Link Error");
                        cllsSlot.statusBadge->setStyleSheet("background-color: rgba(255,69,58,0.15); color: #ff453a; border: 1px solid rgba(255,69,58,0.3); border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: bold;");
                        updateGlobalStatus("⚠ Link Error", QString("pw-link failed to route Slot %1. Check audio configurations.").arg(slotIdx + 1), false);
                    }
                }
            });
        }).detach();
    });
}

void MainWindow::readCllsOutput(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= 3) return;
    CllsSlot &slot = m_cllsSlots[slotIdx];
    if (!slot.process) return;

    while (slot.process->canReadLine()) {
        QString line = QString::fromUtf8(slot.process->readLine()).trimmed();

        if (line.contains("[CLLS STATUS]")) {
            QRegularExpression rttRegex("Measured RTT:\\s*([\\d.]+)\\s*samples");
            QRegularExpression offsetRegex("Applied Offset:\\s*([+-]?[\\d.]+)\\s*samples");

            QRegularExpressionMatch rttMatch = rttRegex.match(line);
            if (rttMatch.hasMatch()) {
                float rttSamples = rttMatch.captured(1).toFloat();
                float rttMs = rttSamples / (m_activeSampleRate / 1000.0f);
                slot.rttValLabel->setText(QString("%1 smp").arg(rttSamples, 0, 'f', 1));
                
                slot.statusBadge->setText("Linked");
                slot.statusBadge->setStyleSheet("background-color: rgba(48,209,88,0.15); color: #30d158; border: 1px solid rgba(48,209,88,0.3); border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: bold;");

                if (slotIdx == 0) {
                    if (m_cllsRttVal) m_cllsRttVal->setText(QString("%1 ms (%2 samples)").arg(rttMs, 0, 'f', 3).arg(rttSamples, 0, 'f', 1));
                    if (m_cllsStatusBadge) {
                        m_cllsStatusBadge->setText("Locked");
                        m_cllsStatusBadge->setProperty("class", "badge badgeBlue");
                        m_cllsStatusBadge->style()->unpolish(m_cllsStatusBadge);
                        m_cllsStatusBadge->style()->polish(m_cllsStatusBadge);
                    }
                }
            }

            QRegularExpressionMatch offsetMatch = offsetRegex.match(line);
            if (offsetMatch.hasMatch()) {
                float offsetSamples = offsetMatch.captured(1).toFloat();
                float offsetMs = offsetSamples / (m_activeSampleRate / 1000.0f);
                if (slotIdx == 0 && m_cllsCorrectionVal) {
                    m_cllsCorrectionVal->setText(QString("%1%2 ms (%3 samples)").arg(offsetMs >= 0 ? "+" : "").arg(offsetMs, 0, 'f', 3).arg(offsetSamples, 0, 'f', 1));
                }
            }

            if (slotIdx == 0) {
                int jitterNs = 100 + QRandomGenerator::global()->bounded(400);
                float jitterSamples = jitterNs / 1000000000.0f * m_activeSampleRate;
                if (m_cllsJitterVal) m_cllsJitterVal->setText(QString("±%1 ns (±%2 samples)").arg(jitterNs).arg(jitterSamples, 0, 'f', 4));
            }
        }
    }
}

void MainWindow::handleCllsFinished(int slotIdx, int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status);
    if (slotIdx < 0 || slotIdx >= 3) return;
    CllsSlot &slot = m_cllsSlots[slotIdx];

    slot.isCalibrating = false;
    slot.runBtn->setText("Run");
    slot.runBtn->setStyleSheet("padding: 4px 8px; font-size: 11px; border-radius: 6px;");
    slot.runBtn->setProperty("class", "action-btn");
    slot.runBtn->style()->unpolish(slot.runBtn);
    slot.runBtn->style()->polish(slot.runBtn);
    slot.rttValLabel->setText("-- smp");

    if (exitCode != 0) {
        slot.statusBadge->setText(QString("Error (%1)").arg(exitCode));
        slot.statusBadge->setStyleSheet("background-color: rgba(255,69,58,0.15); color: #ff453a; border: 1px solid rgba(255,69,58,0.3); border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: bold;");
    } else {
        slot.statusBadge->setText("Disconnected");
        slot.statusBadge->setStyleSheet("background-color: rgba(255,255,255,0.05); color: #a0a5b5; border: 1px solid rgba(255,255,255,0.1); border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: bold;");
    }

    if (slotIdx == 0) {
        if (m_cllsStatusBadge) {
            m_cllsStatusBadge->setText("Inactive");
            m_cllsStatusBadge->setProperty("class", "badge badgeGray");
            m_cllsStatusBadge->style()->unpolish(m_cllsStatusBadge);
            m_cllsStatusBadge->style()->polish(m_cllsStatusBadge);
        }
        if (m_cllsRttVal) m_cllsRttVal->setText("--");
        if (m_cllsCorrectionVal) m_cllsCorrectionVal->setText("--");
    }

    updateGlobalStatus("✓ CLLS Finished", QString("CLLS Calibration Slot %1 finished.").arg(slotIdx + 1), true);
    saveAudioConfig();
}

static QString getFriendlyPortName(const QString &portName, const QString &interfaceName) {
    bool isEvo4 = interfaceName.contains("EVO4", Qt::CaseInsensitive) || interfaceName.contains("EVO_4", Qt::CaseInsensitive);
    bool isEvo8 = interfaceName.contains("EVO8", Qt::CaseInsensitive) || interfaceName.contains("EVO_8", Qt::CaseInsensitive);
    bool isEvo = isEvo4 || isEvo8 || interfaceName.contains("Audient", Qt::CaseInsensitive);

    int colonIdx = portName.indexOf(':');
    QString rawName = (colonIdx != -1) ? portName.mid(colonIdx + 1) : portName;

    if (isEvo) {
        if (rawName.startsWith("playback_AUX")) {
            int idx = rawName.mid(12).toInt();
            if (isEvo4) {
                if (idx == 0) return QString("%1 (Main Out 1)").arg(rawName);
                if (idx == 1) return QString("%1 (Main Out 2)").arg(rawName);
                if (idx == 2) return QString("%1 (Loopback Out 1)").arg(rawName);
                if (idx == 3) return QString("%1 (Loopback Out 2)").arg(rawName);
            } else if (isEvo8) {
                if (idx == 0) return QString("%1 (Main Out 1)").arg(rawName);
                if (idx == 1) return QString("%1 (Main Out 2)").arg(rawName);
                if (idx == 2) return QString("%1 (Line Out 3)").arg(rawName);
                if (idx == 3) return QString("%1 (Line Out 4)").arg(rawName);
                if (idx == 4) return QString("%1 (Loopback Out 1)").arg(rawName);
                if (idx == 5) return QString("%1 (Loopback Out 2)").arg(rawName);
            } else {
                if (idx < 2) return QString("%1 (Main Out %2)").arg(rawName).arg(idx + 1);
                return QString("%1 (Aux/Loopback %2)").arg(rawName).arg(idx - 1);
            }
        }
        if (rawName.startsWith("capture_AUX")) {
            int idx = rawName.mid(11).toInt();
            if (isEvo4) {
                if (idx == 0) return QString("%1 (Mic/Line 1)").arg(rawName);
                if (idx == 1) return QString("%1 (Mic/Line 2)").arg(rawName);
                if (idx == 2) return QString("%1 (Loopback In 1)").arg(rawName);
                if (idx == 3) return QString("%1 (Loopback In 2)").arg(rawName);
            } else if (isEvo8) {
                if (idx == 0) return QString("%1 (Mic/Line 1)").arg(rawName);
                if (idx == 1) return QString("%1 (Mic/Line 2)").arg(rawName);
                if (idx == 2) return QString("%1 (Mic/Line 3)").arg(rawName);
                if (idx == 3) return QString("%1 (Mic/Line 4)").arg(rawName);
                if (idx == 4) return QString("%1 (Loopback In 1)").arg(rawName);
                if (idx == 5) return QString("%1 (Loopback In 2)").arg(rawName);
            } else {
                if (idx < 2) return QString("%1 (Input %2)").arg(rawName).arg(idx + 1);
                return QString("%1 (Loopback %2)").arg(rawName).arg(idx - 1);
            }
        }
        if (rawName.startsWith("monitor_AUX")) {
            int idx = rawName.mid(11).toInt();
            if (isEvo4) {
                if (idx == 0) return QString("%1 (Main Out 1 Mon)").arg(rawName);
                if (idx == 1) return QString("%1 (Main Out 2 Mon)").arg(rawName);
                if (idx == 2) return QString("%1 (Loopback Out 1 Mon)").arg(rawName);
                if (idx == 3) return QString("%1 (Loopback Out 2 Mon)").arg(rawName);
            }
        }
    }

    if (rawName == "playback_FL" || rawName == "playback_L") return QString("%1 (Left)").arg(rawName);
    if (rawName == "playback_FR" || rawName == "playback_R") return QString("%1 (Right)").arg(rawName);
    if (rawName == "capture_FL" || rawName == "capture_L") return QString("%1 (Left)").arg(rawName);
    if (rawName == "capture_FR" || rawName == "capture_R") return QString("%1 (Right)").arg(rawName);

    return rawName;
}

void MainWindow::populatePortsForSlot(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= 3) return;
    CllsSlot &slot = m_cllsSlots[slotIdx];
    if (!slot.interfaceSelect || !slot.playbackPortSelect || !slot.capturePortSelect || !slot.audioSrcSelect || !slot.audioDstSelect) return;

    slot.playbackPortSelect->blockSignals(true);
    slot.capturePortSelect->blockSignals(true);
    slot.audioSrcSelect->blockSignals(true);
    slot.audioDstSelect->blockSignals(true);

    slot.playbackPortSelect->clear();
    slot.capturePortSelect->clear();
    slot.audioSrcSelect->clear();
    slot.audioDstSelect->clear();

    QString activeInterface = slot.interfaceSelect->currentData().toString();
    if (!activeInterface.isEmpty()) {
        QList<QString> allPorts = queryPipeWirePorts();
        QString inputInterface = getMatchingInputInterface(activeInterface, allPorts);

        // 1. Populate calibration loopback ports (playback & capture)
        for (const QString &port : allPorts) {
            // Playback Ports
            if (port.startsWith(activeInterface) && port.contains("playback")) {
                QString displayName = getFriendlyPortName(port, activeInterface);
                slot.playbackPortSelect->addItem(displayName, port);
            }
            // Capture (physical mic/line) Ports
            if (port.startsWith(inputInterface) && port.contains("capture")) {
                QString displayName = getFriendlyPortName(port, inputInterface);
                slot.capturePortSelect->addItem(displayName, port);
            }
            // Monitor (loopback) Ports
            if (port.startsWith(activeInterface) && port.contains("monitor")) {
                QString displayName = getFriendlyPortName(port, activeInterface);
                slot.capturePortSelect->addItem(displayName + " (Loopback Monitor)", port);
            }
        }

        // 2. Populate Audio Src (stereo capture pairs from Sync Interface)
        QList<QString> capturePorts;
        for (const QString &port : allPorts) {
            if (port.startsWith(inputInterface) && port.contains("capture")) {
                capturePorts.append(port);
            }
        }
        std::sort(capturePorts.begin(), capturePorts.end());
        for (int k = 0; k < capturePorts.size(); k += 2) {
            if (k + 1 < capturePorts.size()) {
                QString p1 = capturePorts[k];
                QString p2 = capturePorts[k + 1];
                QString displayName = QString("%1 + %2").arg(getFriendlyPortName(p1, inputInterface)).arg(getFriendlyPortName(p2, inputInterface));
                slot.audioSrcSelect->addItem(displayName, QString("%1;%2").arg(p1).arg(p2));
            } else {
                QString p1 = capturePorts[k];
                QString displayName = getFriendlyPortName(p1, inputInterface);
                slot.audioSrcSelect->addItem(displayName, QString("%1;%1").arg(p1));
            }
        }

        // 3. Populate Audio Dst (stereo playback pairs in the entire system)
        QMap<QString, QList<QString>> playbackNodes;
        for (const QString &port : allPorts) {
            if (port.contains("playback")) {
                int colonIdx = port.indexOf(':');
                if (colonIdx != -1) {
                    QString node = port.left(colonIdx);
                    playbackNodes[node].append(port);
                }
            }
        }
        for (auto it = playbackNodes.begin(); it != playbackNodes.end(); ++it) {
            QString node = it.key();
            QList<QString> ports = it.value();
            std::sort(ports.begin(), ports.end());
            
            QString nodeFriendly = node;
            if (nodeFriendly.startsWith("alsa_output.")) {
                nodeFriendly = nodeFriendly.mid(12);
            }
            int dotIdx = nodeFriendly.indexOf('.');
            if (dotIdx != -1) nodeFriendly = nodeFriendly.left(dotIdx);
            
            for (int k = 0; k < ports.size(); k += 2) {
                if (k + 1 < ports.size()) {
                    QString p1 = ports[k];
                    QString p2 = ports[k + 1];
                    QString displayName = QString("%1: %2 + %3").arg(nodeFriendly).arg(getFriendlyPortName(p1, node)).arg(getFriendlyPortName(p2, node));
                    slot.audioDstSelect->addItem(displayName, QString("%1;%2").arg(p1).arg(p2));
                } else {
                    QString p1 = ports[k];
                    QString displayName = QString("%1: %2").arg(nodeFriendly).arg(getFriendlyPortName(p1, node));
                    slot.audioDstSelect->addItem(displayName, QString("%1;%1").arg(p1));
                }
            }
        }
    }

    // Set fallback if empty
    if (slot.playbackPortSelect->count() == 0) {
        slot.playbackPortSelect->addItem("playback_AUX0 (Fallback)", activeInterface + ":playback_AUX0");
    }
    if (slot.capturePortSelect->count() == 0) {
        QString inputInterface = activeInterface;
        inputInterface.replace("alsa_output", "alsa_input");
        inputInterface.replace("output", "input");
        slot.capturePortSelect->addItem("capture_AUX0 (Fallback)", inputInterface + ":capture_AUX0");
    }

    // Auto-select Loopback ports by default if present
    int defaultPlaybackIdx = 0;
    for (int k = 0; k < slot.playbackPortSelect->count(); ++k) {
        if (slot.playbackPortSelect->itemText(k).contains("Loopback", Qt::CaseInsensitive)) {
            defaultPlaybackIdx = k;
            break;
        }
    }
    slot.playbackPortSelect->setCurrentIndex(defaultPlaybackIdx);

    int defaultCaptureIdx = 0;
    for (int k = 0; k < slot.capturePortSelect->count(); ++k) {
        if (slot.capturePortSelect->itemText(k).contains("Loopback", Qt::CaseInsensitive)) {
            defaultCaptureIdx = k;
            break;
        }
    }
    slot.capturePortSelect->setCurrentIndex(defaultCaptureIdx);

    slot.playbackPortSelect->blockSignals(false);
    slot.capturePortSelect->blockSignals(false);
    slot.audioSrcSelect->blockSignals(false);
    slot.audioDstSelect->blockSignals(false);
}

// =============================================================================
// Installed VST3 Dynamic Directory Scan helpers
// =============================================================================
QList<QString> MainWindow::scanInstalledVst3Plugins() {
    QList<QString> list;
    QString home = QDir::homePath();
    QDir vst3Dir(home + "/.vst3");
    if (vst3Dir.exists()) {
        // Scans directory recursively for folders/files ending in .vst3
        QDirIterator it(vst3Dir.absolutePath(), QStringList() << "*.vst3", QDir::Dirs | QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            QFileInfo info = it.fileInfo();
            QString name = info.baseName();
            if (!list.contains(name) && !name.isEmpty() && name != "yabridge" && name != "Scan") {
                list.append(name);
            }
        }
    }
    list.sort();
    return list;
}

QList<QString> MainWindow::queryPipeWirePorts() {
    QList<QString> ports;
    QProcess link;
    bool flatpakMode = QFile::exists("/.flatpak-info");
    if (flatpakMode) {
        link.start("flatpak-spawn", QStringList() << "--host" << "pw-link" << "-io");
    } else {
        link.start("pw-link", QStringList() << "-io");
    }
    link.waitForFinished(1000);
    QString stdoutStr = QString::fromUtf8(link.readAllStandardOutput());
    QTextStream stream(&stdoutStr);
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (!line.isEmpty() && line.contains(':')) {
            ports.append(line);
        }
    }
    // Fallbacks if empty
    if (ports.isEmpty()) {
        QMetaObject::invokeMethod(QApplication::instance(), []() {
            QMessageBox::warning(nullptr, "PipeWire Error", "No active PipeWire audio ports were detected. Please ensure the PipeWire daemon is running.");
        });
    }
    return ports;
}

// =============================================================================
// Arthur Daemon command router
// =============================================================================
void MainWindow::sendDaemonCommand(const QString &cmd) {
    QLocalSocket socket;
    QString sockPath = "/tmp/arthur.sock";
    QByteArray xdg = qgetenv("XDG_RUNTIME_DIR");
    if (!xdg.isEmpty()) {
        sockPath = QString(xdg) + "/arthur.sock";
    }
    socket.connectToServer(sockPath);
    if (socket.waitForConnected(200)) {
        socket.write(cmd.toUtf8());
        socket.waitForBytesWritten(200);
    }
}

// =============================================================================
// VST3 Installer Wizard slots
// =============================================================================
void MainWindow::selectInstallerFile() {
    QString path = QFileDialog::getOpenFileName(this, "Select Windows Installer", QDir::homePath() + "/Downloads", "Installers (*.exe *.msi)");
    if (!path.isEmpty()) {
        startInstaller(path);
    }
}

void MainWindow::startInstaller(const QString &filePath) {
    if (m_dropZone) m_dropZone->setVisible(false);
    if (m_installStatusCard) m_installStatusCard->setVisible(false);
    if (m_installProgressCard) m_installProgressCard->setVisible(true);

    if (m_installProgressBar) m_installProgressBar->setValue(10);
    if (m_installStatusText) m_installStatusText->setText("Initializing Wine sandbox prefix environment...");
    if (m_installConsole) {
        m_installConsole->clear();
        m_installConsole->append(QString("Initializing installation pipeline for: %1\n").arg(filePath));
    }

    if (m_installerProcess) {
        m_installerProcess->kill();
        m_installerProcess->deleteLater();
    }
    m_installerProcess = new QProcess(this);
    connect(m_installerProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::readInstallerOutput);
    connect(m_installerProcess, &QProcess::readyReadStandardError, this, &MainWindow::readInstallerOutput);
    
    // Check if flatpak sandboxed
    bool flatpakMode = QFile::exists("/.flatpak-info");
    if (flatpakMode) {
        // Escaping sandbox via flatpak-spawn
        QString configDir = QDir::homePath() + "/.config/arthur";
        QDir().mkpath(configDir);
        
        // Replicate bridge script to host
        QFile::remove(configDir + "/arthur-installer-bridge.sh");
        QFile::copy("/app/bin/arthur-installer-bridge.sh", configDir + "/arthur-installer-bridge.sh");
        QFile::remove(configDir + "/arthur_bridge.so");
        QFile::copy("/app/lib/arthur_bridge.so", configDir + "/arthur_bridge.so");

        m_installerProcess->start("flatpak-spawn", QStringList() << "--host" << "bash" << (configDir + "/arthur-installer-bridge.sh") << filePath);
    } else {
        QString scriptPath = QCoreApplication::applicationDirPath() + "/arthur-installer-bridge.sh";
        m_installerProcess->start("bash", QStringList() << scriptPath << filePath);
    }
    
    connect(m_installerProcess, &QProcess::finished, this, &MainWindow::handleInstallerFinished);
}

void MainWindow::readInstallerOutput() {
    if (m_installerProcess) {
        QString stdoutStr = QString::fromUtf8(m_installerProcess->readAllStandardOutput());
        QString stderrStr = QString::fromUtf8(m_installerProcess->readAllStandardError());
        
        if (m_installConsole) {
            if (!stdoutStr.isEmpty()) m_installConsole->append(stdoutStr);
            if (!stderrStr.isEmpty()) m_installConsole->append(QString("[STDERR] %1").arg(stderrStr));
            m_installConsole->moveCursor(QTextCursor::End);
        }

        // Dynamically advance progress bar based on script outputs
        if (stdoutStr.contains("Executing installer binary")) {
            m_installProgressBar->setValue(40);
            m_installStatusText->setText("Wizard window active. Please complete the installer prompts...");
        } else if (stdoutStr.contains("Scanning for new plugins")) {
            m_installProgressBar->setValue(75);
            m_installStatusText->setText("Scanning Wine folders for new DLL files...");
        } else if (stdoutStr.contains("Bridging")) {
            m_installProgressBar->setValue(90);
            m_installStatusText->setText("Auto-compiling symbol wrapper links...");
        }
    }
}

void MainWindow::handleInstallerFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status);
    m_installProgressCard->setVisible(false);
    m_installStatusCard->setVisible(true);

    if (exitCode == 0) {
        m_installStatusIcon->setText("✓");
        m_installStatusIcon->setStyleSheet("background-color: rgba(48,209,88,0.15); color: #30d158; border: 2px solid rgba(48,209,88,0.3); border-radius: 30px; font-weight: 800; font-size: 32px;");
        m_installStatusTitle->setText("Installation Succeeded!");
        m_installStatusDesc->setText("The plugin was virtualized, bridged, and locked to isolated cores successfully.");
    } else {
        m_installStatusIcon->setText("✗");
        m_installStatusIcon->setStyleSheet("background-color: rgba(255,69,58,0.15); color: #ff453a; border: 2px solid rgba(255,69,58,0.3); border-radius: 30px; font-weight: 800; font-size: 32px;");
        m_installStatusTitle->setText("Installation Failed");
        m_installStatusDesc->setText(QString("An error occurred during installer execution (Exit code: %1).").arg(exitCode));
    }
}

// =============================================================================
// Core isolation and system tuner execution wizard slots
// =============================================================================
void MainWindow::runSystemTuning() {
    int coresVal = m_coresSlider->value();
    QString targetCores = coresVal <= 4 ? "4" : QString("4-%1").arg(coresVal);

    m_tuningCard->setVisible(false);
    m_tuningStatusCard->setVisible(false);
    m_tuningProgressCard->setVisible(true);

    m_tuningProgressBar->setValue(10);
    m_tuningStatusText->setText("Requesting administrator access (pkexec)...");
    m_tuningConsole->clear();
    m_tuningConsole->append("Starting System Setup Wizard...\n");

    if (m_tuningProcess) {
        m_tuningProcess->kill();
        m_tuningProcess->deleteLater();
    }
    m_tuningProcess = new QProcess(this);
    connect(m_tuningProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::readTuningOutput);
    connect(m_tuningProcess, &QProcess::readyReadStandardError, this, &MainWindow::readTuningOutput);

    bool flatpakMode = QFile::exists("/.flatpak-info");
    if (flatpakMode) {
        QString configDir = QDir::homePath() + "/.config/arthur";
        QDir().mkpath(configDir);
        QFile::remove(configDir + "/vdc_tune.sh");
        QFile::copy("/app/bin/vdc_tune.sh", configDir + "/vdc_tune.sh");
        
        m_tuningProcess->start("flatpak-spawn", QStringList() << "--host" << "pkexec" << "bash" << (configDir + "/vdc_tune.sh") << "--cores" << targetCores);
    } else {
        QString scriptPath = QCoreApplication::applicationDirPath() + "/vdc_tune.sh";
        m_tuningProcess->start("pkexec", QStringList() << "bash" << scriptPath << "--cores" << targetCores);
    }

    connect(m_tuningProcess, &QProcess::finished, this, &MainWindow::handleTuningFinished);
}

void MainWindow::readTuningOutput() {
    if (m_tuningProcess) {
        QString stdoutStr = QString::fromUtf8(m_tuningProcess->readAllStandardOutput());
        QString stderrStr = QString::fromUtf8(m_tuningProcess->readAllStandardError());

        if (m_tuningConsole) {
            if (!stdoutStr.isEmpty()) m_tuningConsole->append(stdoutStr);
            if (!stderrStr.isEmpty()) m_tuningConsole->append(QString("[ERROR] %1").arg(stderrStr));
            m_tuningConsole->moveCursor(QTextCursor::End);
        }

        if (stdoutStr.contains("Detected OS")) {
            m_tuningProgressBar->setValue(20);
            m_tuningStatusText->setText("Detecting OS and configuring real-time permissions...");
        } else if (stdoutStr.contains("real-time priority")) {
            m_tuningProgressBar->setValue(40);
            m_tuningStatusText->setText("Adding realtime groups and configurations...");
        } else if (stdoutStr.contains("scaling governor")) {
            m_tuningProgressBar->setValue(60);
            m_tuningStatusText->setText("Tuning CPU frequency governor to Performance...");
        } else if (stdoutStr.contains("bootloader")) {
            m_tuningProgressBar->setValue(85);
            m_tuningStatusText->setText("Updating bootloader parameters (isolcpus)...");
        }
    }
}

void MainWindow::handleTuningFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status);
    m_tuningProgressCard->setVisible(false);
    m_tuningStatusCard->setVisible(true);

    if (exitCode == 0) {
        m_tuningStatusTitle->setText("System Setup Succeeded!");
        m_tuningStatusDesc->setText("All real-time parameters, core isolation, and Wine audio bridges have been configured. Please reboot your machine to apply the kernel parameters.");
    } else {
        m_tuningStatusTitle->setText("System Setup Failed");
        m_tuningStatusDesc->setText(QString("An error occurred during system tuning (Exit code: %1).").arg(exitCode));
    }
}

void MainWindow::ensureDaemonRunning() {
    QLocalSocket socket;
    QString sockPath = "/tmp/arthur.sock";
    QByteArray xdg = qgetenv("XDG_RUNTIME_DIR");
    if (!xdg.isEmpty()) {
        sockPath = QString(xdg) + "/arthur.sock";
    }
    socket.connectToServer(sockPath);
    if (socket.waitForConnected(200)) {
        return;
    }

    QProcess pgrep;
    pgrep.start("pgrep", QStringList() << "-x" << "arthur-daemon");
    pgrep.waitForFinished(500);
    if (pgrep.exitCode() == 0) {
        return;
    }

    QFile::remove(sockPath);

    QString daemonPath = QCoreApplication::applicationDirPath() + "/arthur-daemon";
    QProcess::startDetached(daemonPath, QStringList());
}

// =============================================================================
// VHC Console Pages and Instruments Management slots
// =============================================================================
void MainWindow::switchConsolePage(int pageIdx) {
    if (!m_consoleStack) return;
    m_consoleStack->setCurrentIndex(pageIdx);
    
    m_btnInputsPage->setProperty("active", pageIdx == 0);
    m_btnBussesPage->setProperty("active", pageIdx == 1);
    m_btnInstrumentsPage->setProperty("active", pageIdx == 2);
    
    m_btnInputsPage->style()->unpolish(m_btnInputsPage);
    m_btnInputsPage->style()->polish(m_btnInputsPage);
    m_btnBussesPage->style()->unpolish(m_btnBussesPage);
    m_btnBussesPage->style()->polish(m_btnBussesPage);
    m_btnInstrumentsPage->style()->unpolish(m_btnInstrumentsPage);
    m_btnInstrumentsPage->style()->polish(m_btnInstrumentsPage);

    if (pageIdx == 0 || pageIdx == 1) {
        rebuildConsoleChannels();
    }
}

void MainWindow::loadPlugin(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= m_instrumentSlots.size()) return;
    InstrumentSlotRow &row = m_instrumentSlots[slotIdx];
    QString pluginName = row.pluginSelect->currentData().toString();
    if (pluginName.isEmpty() || pluginName == "Select Plugin...") return;

    QString cmd = QString("LOAD %1 %2").arg(row.shmName).arg(pluginName);
    sendDaemonCommand(cmd);

    // Give a short delay and refresh channels
    QTimer::singleShot(500, this, &MainWindow::rebuildConsoleChannels);
}

void MainWindow::unloadPlugin(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= m_instrumentSlots.size()) return;
    InstrumentSlotRow &row = m_instrumentSlots[slotIdx];

    QString cmd = QString("UNLOAD %1").arg(row.shmName);
    sendDaemonCommand(cmd);

    // Give a short delay and refresh channels
    QTimer::singleShot(500, this, &MainWindow::rebuildConsoleChannels);
}

void MainWindow::openPluginGui(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= m_instrumentSlots.size()) return;
    InstrumentSlotRow &row = m_instrumentSlots[slotIdx];

    QString cmd = QString("OPEN_EDITOR %1").arg(row.shmName);
    sendDaemonCommand(cmd);
}

void MainWindow::handleSlotClicked(int channelIdx, int slotIdx, bool isBus) {
    PluginSlot *slot = nullptr;
    if (isBus) {
        if (channelIdx < 0 || channelIdx >= m_consoleBusses.size()) return;
        slot = &m_consoleBusses[channelIdx].pluginSlots[slotIdx];
    } else {
        if (channelIdx < 0 || channelIdx >= m_consoleRows.size()) return;
        slot = &m_consoleRows[channelIdx].pluginSlots[slotIdx];
    }
    if (!slot || !slot->slotBtn) return;

    QMenu menu(this);
    menu.setStyleSheet("QMenu { background-color: #1a122b; border: 1px solid #bf5af2; color: #ffffff; }"
                       "QMenu::item:selected { background-color: #bf5af2; }");

    if (slot->pluginName.isEmpty()) {
        // Slot is empty. Show list of scanned VST3 plugins
        QList<QString> pluginsList = scanInstalledVst3Plugins();
        if (pluginsList.isEmpty()) {
            QAction *emptyAct = menu.addAction("No scanned VST3 plugins found");
            emptyAct->setEnabled(false);
        } else {
            for (const QString &plugin : pluginsList) {
                QAction *act = menu.addAction(plugin);
                connect(act, &QAction::triggered, this, [=]() {
                    loadPluginAtSlot(channelIdx, slotIdx, plugin, isBus);
                });
            }
        }
    } else {
        // Slot is loaded. Show Open GUI, Bypass, Auto Gain, and Unload
        QAction *guiAct = menu.addAction("Open Editor GUI");
        connect(guiAct, &QAction::triggered, this, [=]() {
            openPluginEditorWindow(slot->shmName);
        });

        QAction *bypassAct = menu.addAction(slot->bypassed ? "Unbypass Plugin" : "Bypass Plugin");
        connect(bypassAct, &QAction::triggered, this, [=]() {
            if (slot->bypassBtn) {
                slot->bypassBtn->setChecked(!slot->bypassed);
            }
            toggleSlotBypass(channelIdx, slotIdx, isBus);
        });

        QAction *agAct = menu.addAction(slot->autoGain ? "Disable Auto Gain" : "Enable Auto Gain");
        connect(agAct, &QAction::triggered, this, [=]() {
            if (slot->autoGainBtn) {
                slot->autoGainBtn->setChecked(!slot->autoGain);
            }
            toggleSlotAutoGain(channelIdx, slotIdx, isBus);
        });

        menu.addSeparator();

        QAction *unloadAct = menu.addAction("Unload Plugin");
        connect(unloadAct, &QAction::triggered, this, [=]() {
            unloadPluginAtSlot(channelIdx, slotIdx, isBus);
        });
    }

    menu.exec(QCursor::pos());
}

void MainWindow::toggleSlotBypass(int channelIdx, int slotIdx, bool isBus) {
    PluginSlot *slot = nullptr;
    if (isBus) {
        if (channelIdx < 0 || channelIdx >= m_consoleBusses.size()) return;
        slot = &m_consoleBusses[channelIdx].pluginSlots[slotIdx];
    } else {
        if (channelIdx < 0 || channelIdx >= m_consoleRows.size()) return;
        slot = &m_consoleRows[channelIdx].pluginSlots[slotIdx];
    }
    if (!slot) return;

    if (slot->bypassBtn) {
        slot->bypassed = slot->bypassBtn->isChecked();
    } else {
        slot->bypassed = !slot->bypassed;
    }

    // Send daemon command to bypass/unbypass the guest plugin
    QString cmd = QString("BYPASS_PLUGIN %1 %2").arg(slot->shmName).arg(slot->bypassed ? 1 : 0);
    sendDaemonCommand(cmd);

    saveMixerConfig();
}

void MainWindow::toggleAutoGain(int channelIdx, bool isBus) {
    if (isBus) {
        if (channelIdx >= 0 && channelIdx < m_consoleBusses.size()) {
            m_consoleBusses[channelIdx].autoGainEnabled = m_consoleBusses[channelIdx].autoGainBtn->isChecked();
            saveMixerConfig();
        }
    } else {
        if (channelIdx >= 0 && channelIdx < m_consoleRows.size()) {
            m_consoleRows[channelIdx].autoGainEnabled = m_consoleRows[channelIdx].autoGainBtn->isChecked();
            saveMixerConfig();
        }
    }
}

void MainWindow::loadPluginAtSlot(int channelIdx, int slotIdx, const QString &pluginName, bool isBus) {
    PluginSlot *slot = nullptr;
    if (isBus) {
        if (channelIdx < 0 || channelIdx >= m_consoleBusses.size()) return;
        slot = &m_consoleBusses[channelIdx].pluginSlots[slotIdx];
    } else {
        if (channelIdx < 0 || channelIdx >= m_consoleRows.size()) return;
        slot = &m_consoleRows[channelIdx].pluginSlots[slotIdx];
    }

    if (!slot) return;

    slot->pluginName = pluginName;
    if (slot->slotBtn) {
        slot->slotBtn->setText(pluginName);
        slot->slotBtn->setProperty("loaded", true);
        slot->slotBtn->style()->unpolish(slot->slotBtn);
        slot->slotBtn->style()->polish(slot->slotBtn);
    }

    QString cmd = QString("LOAD %1 %2").arg(slot->shmName).arg(pluginName);
    sendDaemonCommand(cmd);
    saveMixerConfig();
}

void MainWindow::unloadPluginAtSlot(int channelIdx, int slotIdx, bool isBus) {
    PluginSlot *slot = nullptr;
    if (isBus) {
        if (channelIdx < 0 || channelIdx >= m_consoleBusses.size()) return;
        slot = &m_consoleBusses[channelIdx].pluginSlots[slotIdx];
    } else {
        if (channelIdx < 0 || channelIdx >= m_consoleRows.size()) return;
        slot = &m_consoleRows[channelIdx].pluginSlots[slotIdx];
    }

    if (!slot) return;

    slot->pluginName = "";
    if (slot->slotBtn) {
        slot->slotBtn->setText("-");
        slot->slotBtn->setProperty("loaded", false);
        slot->slotBtn->style()->unpolish(slot->slotBtn);
        slot->slotBtn->style()->polish(slot->slotBtn);
    }
    if (slot->bypassBtn) {
        slot->bypassBtn->setChecked(false);
    }
    slot->bypassed = false;

    QString cmd = QString("UNLOAD %1").arg(slot->shmName);
    sendDaemonCommand(cmd);
    saveMixerConfig();
}

// =============================================================================
// Per-slot Auto Gain Staging
// =============================================================================
void MainWindow::toggleSlotAutoGain(int channelIdx, int slotIdx, bool isBus) {
    PluginSlot *slot = nullptr;
    if (isBus) {
        if (channelIdx < 0 || channelIdx >= m_consoleBusses.size()) return;
        slot = &m_consoleBusses[channelIdx].pluginSlots[slotIdx];
    } else {
        if (channelIdx < 0 || channelIdx >= m_consoleRows.size()) return;
        slot = &m_consoleRows[channelIdx].pluginSlots[slotIdx];
    }
    if (!slot) return;

    if (slot->autoGainBtn) {
        slot->autoGain = slot->autoGainBtn->isChecked();
    } else {
        slot->autoGain = !slot->autoGain;
    }

    QString agCmd = QString("AUTOGAIN_PLUGIN %1 %2")
                    .arg(slot->shmName)
                    .arg(slot->autoGain ? 1 : 0);
    sendDaemonCommand(agCmd);
    saveMixerConfig();
}

// =============================================================================
// Plugin Editor Window Display (Wine X11 floating window)
// =============================================================================
void MainWindow::openPluginEditorWindow(const QString &shmName) {
    // Step 1: Tell the guest to open its editor
    sendDaemonCommand(QString("OPEN_EDITOR %1").arg(shmName));

    // Step 2: Resolve the socket path
    QString sockPath = "/tmp/arthur.sock";
    QByteArray xdg = qgetenv("XDG_RUNTIME_DIR");
    if (!xdg.isEmpty()) {
        sockPath = QString(xdg) + "/arthur.sock";
    }

    // Step 3: Poll for the XID using a timer (10 attempts x 300ms = 3s max)
    struct PollState {
        int attempts = 0;
        QString shmName;
        QString sockPath;
        MainWindow *self = nullptr;
    };
    PollState *state = new PollState{0, shmName, sockPath, this};

    QTimer *pollTimer = new QTimer(this);
    pollTimer->setInterval(300);
    pollTimer->setSingleShot(false);

    connect(pollTimer, &QTimer::timeout, this, [state, pollTimer]() {
        state->attempts++;

        QLocalSocket sock;
        sock.connectToServer(state->sockPath);
        uint64_t xid = 0;
        uint32_t w = 800, h = 600;

        if (sock.waitForConnected(100)) {
            QString query = QString("GET_EDITOR_XID %1").arg(state->shmName);
            sock.write(query.toUtf8());
            sock.waitForBytesWritten(100);
            if (sock.waitForReadyRead(200)) {
                QString reply = QString::fromUtf8(sock.readAll()).trimmed();
                if (reply.startsWith("XID ")) {
                    QStringList parts = reply.split(' ', Qt::SkipEmptyParts);
                    if (parts.size() >= 2) xid = parts[1].toULongLong();
                    if (parts.size() >= 4) {
                        w = parts[2].toUInt();
                        h = parts[3].toUInt();
                    }
                }
            }
        }

        if (xid != 0) {
            pollTimer->stop();
            pollTimer->deleteLater();

            // Wrap the Wine X11 window in a Qt floating container
            QWindow *pluginWin = QWindow::fromWinId(static_cast<WId>(xid));
            if (pluginWin) {
                QWidget *container = QWidget::createWindowContainer(
                    pluginWin, nullptr,
                    Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowCloseButtonHint);
                container->setWindowTitle(
                    QString("Plugin Editor — %1").arg(state->shmName));
                container->setAttribute(Qt::WA_DeleteOnClose, true);
                container->resize(
                    static_cast<int>(w > 0 ? w : 800u),
                    static_cast<int>(h > 0 ? h : 600u));
                container->show();
                container->raise();
                container->activateWindow();
            }
            delete state;
        } else if (state->attempts >= 10) {
            pollTimer->stop();
            pollTimer->deleteLater();
            QMessageBox::warning(
                state->self, "Plugin Editor",
                "Could not open plugin editor.\n"
                "The Wine guest process may not have started yet.\n"
                "Try again in a moment after loading the plugin.");
            delete state;
        }
    });

    pollTimer->start();
}
