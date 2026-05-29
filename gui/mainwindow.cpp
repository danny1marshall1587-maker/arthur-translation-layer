#include "mainwindow.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QStackedWidget>
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
    , m_cllsProcess(nullptr)
    , m_installerProcess(nullptr)
    , m_tuningProcess(nullptr)
{
    initUi();
    setupGlobalStylesheet();

    // Start background status query loop
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::querySystemStatus);
    m_statusTimer->start(2000);

    // Initial config query
    QTimer::singleShot(200, this, &MainWindow::loadAudioConfig);
    QTimer::singleShot(500, this, &MainWindow::loadVdcProfiles);
}

MainWindow::~MainWindow() {
    if (m_cllsProcess) {
        m_cllsProcess->kill();
        m_cllsProcess->waitForFinished(500);
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
            font-size: 14px;
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
            padding: 12px 20px;
            font-weight: 600;
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
            font-size: 15px;
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
            padding: 8px 12px;
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
            padding: 8px 12px;
            color: #f0f2f5;
        }
        /* Buttons */
        .action-btn {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #007aff, stop:1 #af52de);
            border: none;
            border-radius: 8px;
            color: white;
            font-weight: 600;
            padding: 10px 20px;
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
            padding: 10px 20px;
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
    m_sidebar->setFixedWidth(240);
    QVBoxLayout *sidebarLayout = new QVBoxLayout(m_sidebar);
    sidebarLayout->setContentsMargins(20, 30, 20, 20);

    // Brand Header
    QLabel *logoLabel = new QLabel("A", m_sidebar);
    logoLabel->setFixedSize(32, 32);
    logoLabel->setAlignment(Qt::AlignCenter);
    logoLabel->setStyleSheet("background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #007aff, stop:1 #af52de); border-radius: 8px; font-weight: 800; font-size: 16px;");

    QLabel *brandText = new QLabel("ARTHUR", m_sidebar);
    brandText->setStyleSheet("font-size: 20px; font-weight: 800; letter-spacing: 1.5px; color: #ffffff;");
    
    QHBoxLayout *brandLayout = new QHBoxLayout();
    brandLayout->addWidget(logoLabel);
    brandLayout->addWidget(brandText);
    brandLayout->addStretch();
    sidebarLayout->addLayout(brandLayout);
    sidebarLayout->addSpacing(30);

    // Nav Buttons
    QPushButton *btnDashboard = new QPushButton("  Dashboard", m_sidebar);
    btnDashboard->setProperty("active", true);
    btnDashboard->setIcon(QApplication::style()->standardIcon(QStyle::SP_ComputerIcon));
    btnDashboard->setCursor(Qt::PointingHandCursor);
    btnDashboard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btnDashboard->setFixedHeight(45);
    btnDashboard->setProperty("class", "navBtn");
    
    QPushButton *btnInstaller = new QPushButton("  Install Plugins", m_sidebar);
    btnInstaller->setIcon(QApplication::style()->standardIcon(QStyle::SP_DriveHDIcon));
    btnInstaller->setCursor(Qt::PointingHandCursor);
    btnInstaller->setFixedHeight(45);
    btnInstaller->setProperty("class", "navBtn");

    QPushButton *btnRack = new QPushButton("  Virtual DSP Rack", m_sidebar);
    btnRack->setIcon(QApplication::style()->standardIcon(QStyle::SP_FileDialogListView));
    btnRack->setCursor(Qt::PointingHandCursor);
    btnRack->setFixedHeight(45);
    btnRack->setProperty("class", "navBtn");

    QPushButton *btnSettings = new QPushButton("  Audio Setup", m_sidebar);
    btnSettings->setIcon(QApplication::style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    btnSettings->setCursor(Qt::PointingHandCursor);
    btnSettings->setFixedHeight(45);
    btnSettings->setProperty("class", "navBtn");

    sidebarLayout->addWidget(btnDashboard);
    sidebarLayout->addWidget(btnInstaller);
    sidebarLayout->addWidget(btnRack);
    sidebarLayout->addWidget(btnSettings);
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
    m_contentArea->setStyleSheet("background-color: #0f121e; padding: 30px;");
    mainLayout->addWidget(m_contentArea);

    // Connect Navigation Button Clicks to Stack switches
    connect(btnDashboard, &QPushButton::clicked, this, [=]() {
        btnDashboard->setProperty("active", true); btnInstaller->setProperty("active", false); btnRack->setProperty("active", false); btnSettings->setProperty("active", false);
        btnDashboard->style()->unpolish(btnDashboard); btnDashboard->style()->polish(btnDashboard);
        btnInstaller->style()->unpolish(btnInstaller); btnInstaller->style()->polish(btnInstaller);
        btnRack->style()->unpolish(btnRack); btnRack->style()->polish(btnRack);
        btnSettings->style()->unpolish(btnSettings); btnSettings->style()->polish(btnSettings);
        showDashboard();
    });
    connect(btnInstaller, &QPushButton::clicked, this, [=]() {
        btnDashboard->setProperty("active", false); btnInstaller->setProperty("active", true); btnRack->setProperty("active", false); btnSettings->setProperty("active", false);
        btnDashboard->style()->unpolish(btnDashboard); btnDashboard->style()->polish(btnDashboard);
        btnInstaller->style()->unpolish(btnInstaller); btnInstaller->style()->polish(btnInstaller);
        btnRack->style()->unpolish(btnRack); btnRack->style()->polish(btnRack);
        btnSettings->style()->unpolish(btnSettings); btnSettings->style()->polish(btnSettings);
        showInstaller();
    });
    connect(btnRack, &QPushButton::clicked, this, [=]() {
        btnDashboard->setProperty("active", false); btnInstaller->setProperty("active", false); btnRack->setProperty("active", true); btnSettings->setProperty("active", false);
        btnDashboard->style()->unpolish(btnDashboard); btnDashboard->style()->polish(btnDashboard);
        btnInstaller->style()->unpolish(btnInstaller); btnInstaller->style()->polish(btnInstaller);
        btnRack->style()->unpolish(btnRack); btnRack->style()->polish(btnRack);
        btnSettings->style()->unpolish(btnSettings); btnSettings->style()->polish(btnSettings);
        showRack();
    });
    connect(btnSettings, &QPushButton::clicked, this, [=]() {
        btnDashboard->setProperty("active", false); btnInstaller->setProperty("active", false); btnRack->setProperty("active", false); btnSettings->setProperty("active", true);
        btnDashboard->style()->unpolish(btnDashboard); btnDashboard->style()->polish(btnDashboard);
        btnInstaller->style()->unpolish(btnInstaller); btnInstaller->style()->polish(btnInstaller);
        btnRack->style()->unpolish(btnRack); btnRack->style()->polish(btnRack);
        btnSettings->style()->unpolish(btnSettings); btnSettings->style()->polish(btnSettings);
        showSettings();
    });

    // =========================================================================
    // Stack 1: Dashboard Tab
    // =========================================================================
    m_dashboardTab = new QWidget(this);
    QVBoxLayout *dashLayout = new QVBoxLayout(m_dashboardTab);
    dashLayout->setContentsMargins(10, 10, 10, 10);
    dashLayout->setSpacing(20);

    QLabel *dashTitle = new QLabel("Studio OS Dashboard", m_dashboardTab);
    dashTitle->setStyleSheet("font-size: 26px; font-weight: 800; color: #ffffff;");
    QLabel *dashSub = new QLabel("Dynamic monitoring of translation and clock alignment layers.", m_dashboardTab);
    dashSub->setStyleSheet("color: #a0a5b5; font-size: 13px;");

    dashLayout->addWidget(dashTitle);
    dashLayout->addWidget(dashSub);

    QGridLayout *dashGrid = new QGridLayout();
    dashGrid->setSpacing(20);

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
        bar->setFixedHeight(20 + rand() % 25);
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
                int height = 10 + rand() % 35;
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
    instLayout->setSpacing(20);

    QLabel *instTitle = new QLabel("Drag & Drop VST3 Installer", m_installerTab);
    instTitle->setStyleSheet("font-size: 26px; font-weight: 800; color: #ffffff;");
    QLabel *instSub = new QLabel("Install Windows .exe or .msi plugin installers directly into the Arthur Translation Layer.", m_installerTab);
    instSub->setStyleSheet("color: #a0a5b5; font-size: 13px;");

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
        loadVdcProfiles(); // reload to show newly installed plugins
    });

    statCardLayout->addWidget(m_installStatusIcon, 0, Qt::AlignCenter);
    statCardLayout->addWidget(m_installStatusTitle);
    statCardLayout->addWidget(m_installStatusDesc);
    statCardLayout->addWidget(statusOkBtn, 0, Qt::AlignCenter);
    instLayout->addWidget(m_installStatusCard);

    m_contentArea->addWidget(m_installerTab);

    // =========================================================================
    // Stack 3: Virtual DSP Rack Tab
    // =========================================================================
    m_rackTab = new QWidget(this);
    QVBoxLayout *rackLayout = new QVBoxLayout(m_rackTab);
    rackLayout->setSpacing(20);

    QLabel *rackTitle = new QLabel("Virtual DSP Cores (VDC) Rack", m_rackTab);
    rackTitle->setStyleSheet("font-size: 26px; font-weight: 800; color: #ffffff;");
    QLabel *rackSub = new QLabel("Pre-load plugins into system memory slots locked on isolated real-time CPU cores.", m_rackTab);
    rackSub->setStyleSheet("color: #a0a5b5; font-size: 13px;");

    rackLayout->addWidget(rackTitle);
    rackLayout->addWidget(rackSub);

    QFrame *profHeader = new QFrame(m_rackTab);
    profHeader->setProperty("class", "card");
    QHBoxLayout *profHeaderLayout = new QHBoxLayout(profHeader);
    profHeaderLayout->setContentsMargins(20, 12, 20, 12);
    
    QLabel *profSelectLabel = new QLabel("VDC DSP Profile:", profHeader);
    profSelectLabel->setStyleSheet("font-weight: 600; color: #a0a5b5;");
    m_profileSelect = new QComboBox(profHeader);
    m_profileSelect->setProperty("class", "custom-select");
    m_profileSelect->setMinimumWidth(220);
    connect(m_profileSelect, &QComboBox::currentTextChanged, this, &MainWindow::onProfileChanged);

    QPushButton *saveProfBtn = new QPushButton("Save Rack Configuration", profHeader);
    saveProfBtn->setProperty("class", "action-btn");
    saveProfBtn->setCursor(Qt::PointingHandCursor);
    connect(saveProfBtn, &QPushButton::clicked, this, &MainWindow::saveCurrentProfile);

    profHeaderLayout->addWidget(profSelectLabel);
    profHeaderLayout->addWidget(m_profileSelect);
    profHeaderLayout->addStretch();
    profHeaderLayout->addWidget(saveProfBtn);
    rackLayout->addWidget(profHeader);

    m_rackGridWidget = new QWidget(m_rackTab);
    rackLayout->addWidget(m_rackGridWidget);

    m_contentArea->addWidget(m_rackTab);

    // =========================================================================
    // Stack 4: Audio Setup Tab
    // =========================================================================
    m_settingsTab = new QWidget(this);
    QVBoxLayout *setTabMainLayout = new QVBoxLayout(m_settingsTab);
    setTabMainLayout->setSpacing(20);

    QLabel *setTabTitle = new QLabel("Audio Setup & System Tuner", m_settingsTab);
    setTabTitle->setStyleSheet("font-size: 26px; font-weight: 800; color: #ffffff;");
    QLabel *setTabSub = new QLabel("Configure PipeWire backend latency and lock kernel parameters for core isolation.", m_settingsTab);
    setTabSub->setStyleSheet("color: #a0a5b5; font-size: 13px;");

    setTabMainLayout->addWidget(setTabTitle);
    setTabMainLayout->addWidget(setTabSub);

    QGridLayout *setGrid = new QGridLayout();
    setGrid->setSpacing(20);

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

    QLabel *cllsTimingsCardTitle = new QLabel("CLLS & MIDI Timings", cllsTimingsCard);
    cllsTimingsCardTitle->setProperty("class", "cardTitle");
    cllsTimingsCardLayout->addWidget(cllsTimingsCardTitle);
    cllsTimingsCardLayout->addSpacing(10);

    cllsTimingsCardLayout->addWidget(makeSettingsSelect("CLLS Calibration Channel", m_cllsChannelSelect, cllsTimingsCard));
    m_cllsChannelSelect->addItem("Channel 8 (Loopback Return)", "Channel 8");
    m_cllsChannelSelect->addItem("Channel 2 (Stereo Out L)", "Channel 2");
    m_cllsChannelSelect->addItem("Channel 4 (Stereo Out R)", "Channel 4");

    m_midiSlaveCheck = new QCheckBox("Slave ALSA Sequencer to Audio PCM clock", cllsTimingsCard);
    m_midiSlaveCheck->setChecked(true);
    m_midiSlaveCheck->setStyleSheet("QCheckBox { margin-top: 10px; font-weight: 600; }");
    connect(m_midiSlaveCheck, &QCheckBox::stateChanged, this, &MainWindow::applyAudioConfig);
    cllsTimingsCardLayout->addWidget(m_midiSlaveCheck);
    cllsTimingsCardLayout->addSpacing(15);

    m_runCllsBtn = new QPushButton("Calibrate CLLS Latency", cllsTimingsCard);
    m_runCllsBtn->setProperty("class", "settings-btn");
    m_runCllsBtn->setCursor(Qt::PointingHandCursor);
    connect(m_runCllsBtn, &QPushButton::clicked, this, &MainWindow::startCllsCalibration);
    cllsTimingsCardLayout->addWidget(m_runCllsBtn);
    cllsTimingsCardLayout->addStretch();

    setGrid->addWidget(cllsTimingsCard, 0, 1);
    setTabMainLayout->addLayout(setGrid);

    // Wide Tuning Card
    m_tuningStatusCard = new QFrame(m_settingsTab);
    m_tuningStatusCard->setProperty("class", "card");
    QVBoxLayout *tuneLayout = new QVBoxLayout(m_tuningStatusCard);
    
    QLabel *tuneTitle = new QLabel("Virtual DSP Core Partitioning", m_tuningStatusCard);
    tuneTitle->setProperty("class", "cardTitle");
    QLabel *tuneDesc = new QLabel("Allocate isolated CPU cores to run host translation plugins exclusively. Isolating cores shields the audio process from scheduler interrupts.", m_tuningStatusCard);
    tuneDesc->setStyleSheet("color: #a0a5b5; font-size: 13px; line-height: 1.5;");

    m_coresSlider = new QSlider(Qt::Horizontal, m_tuningStatusCard);
    m_coresSlider->setRange(1, 7);
    m_coresSlider->setValue(4);
    
    m_coresStatusLabel = new QLabel("Allocating Cores 4-4 to Virtual DSP (1 Core isolated)", m_tuningStatusCard);
    m_coresStatusLabel->setStyleSheet("font-weight: 600; color: #007aff; font-size: 13px;");
    m_coresStatusLabel->setAlignment(Qt::AlignCenter);

    connect(m_coresSlider, &QSlider::valueChanged, this, [=](int val) {
        if (val <= 4) {
            m_coresStatusLabel->setText(QString("Allocating Core 4 to Virtual DSP (1 Core isolated)"));
        } else {
            m_coresStatusLabel->setText(QString("Allocating Cores 4-%1 to Virtual DSP (%2 Cores isolated)").arg(val).arg(val - 3));
        }
    });

    QPushButton *applyTuningBtn = new QPushButton("Apply Core Isolation & Tune System", m_tuningStatusCard);
    applyTuningBtn->setProperty("class", "action-btn");
    applyTuningBtn->setCursor(Qt::PointingHandCursor);
    connect(applyTuningBtn, &QPushButton::clicked, this, &MainWindow::runSystemTuning);

    tuneLayout->addWidget(tuneTitle);
    tuneLayout->addWidget(tuneDesc);
    tuneLayout->addSpacing(10);
    tuneLayout->addWidget(m_coresSlider);
    tuneLayout->addWidget(m_coresStatusLabel);
    tuneLayout->addSpacing(10);
    tuneLayout->addWidget(applyTuningBtn);

    setTabMainLayout->addWidget(m_tuningStatusCard);

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
    tStatLayout->setContentsMargins(30, 30, 30, 30);
    tStatLayout->setSpacing(15);
    
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
        m_tuningStatusCard->setVisible(true); // reset
    });

    tStatLayout->addWidget(m_tuningStatusTitle);
    tStatLayout->addWidget(m_tuningStatusDesc);
    tStatLayout->addWidget(tOkBtn, 0, Qt::AlignCenter);
    setTabMainLayout->addWidget(m_tuningStatusCard);

    m_contentArea->addWidget(m_settingsTab);
}

void MainWindow::showDashboard() {
    m_contentArea->setCurrentWidget(m_dashboardTab);
    querySystemStatus();
}
void MainWindow::showInstaller() {
    m_contentArea->setCurrentWidget(m_installerTab);
}
void MainWindow::showRack() {
    m_contentArea->setCurrentWidget(m_rackTab);
    renderRackGrid();
}
void MainWindow::showSettings() {
    m_contentArea->setCurrentWidget(m_settingsTab);
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
        vdcLoad = 30.0f + (rand() % 150) / 10.0f;
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

    // 1. Get sound interfaces via pactl
    QProcess pactl;
    pactl.start("pactl", QStringList() << "list" << "sinks");
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
            m_audioInterfaceSelect->addItem("EVO4 Pro (Fallback)", "alsa_output.usb-Audient_EVO4-00.pro-output-0");
        }

        // Get default sink
        QProcess pactlDef;
        pactlDef.start("pactl", QStringList() << "get-default-sink");
        pactlDef.waitForFinished(500);
        activeInterface = QString::fromUtf8(pactlDef.readAllStandardOutput()).trimmed();
        int idx = m_audioInterfaceSelect->findData(activeInterface);
        if (idx >= 0) {
            m_audioInterfaceSelect->setCurrentIndex(idx);
        } else {
            m_audioInterfaceSelect->setCurrentIndex(0);
        }
    }

    // 2. Query PipeWire rate & quantum settings via pw-metadata
    QProcess pwMeta;
    pwMeta.start("pw-metadata", QStringList() << "-n" << "settings");
    pwMeta.waitForFinished(1000);
    QString metaStdout = QString::fromUtf8(pwMeta.readAllStandardOutput());
    
    unsigned int rate = 48000;
    unsigned int quantum = 128;
    
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

    m_activeSampleRate = rate;

    if (m_sampleRateSelect) {
        int idx = m_sampleRateSelect->findData(rate);
        if (idx >= 0) m_sampleRateSelect->setCurrentIndex(idx);
    }
    if (m_bufferSizeSelect) {
        int idx = m_bufferSizeSelect->findData(quantum);
        if (idx >= 0) m_bufferSizeSelect->setCurrentIndex(idx);
    }

    // 3. Slaving state
    QProcess pgrep;
    pgrep.start("pgrep", QStringList() << "-x" << "midi_sync");
    pgrep.waitForFinished(500);
    bool slaving = (pgrep.exitCode() == 0);
    if (m_midiSlaveCheck) {
        m_midiSlaveCheck->setChecked(slaving);
    }

    m_isUpdatingConfig = false;
}

void MainWindow::applyAudioConfig() {
    if (m_isUpdatingConfig) return;

    QString interface = m_audioInterfaceSelect->currentData().toString();
    unsigned int rate = m_sampleRateSelect->currentData().toUInt();
    unsigned int quantum = m_bufferSizeSelect->currentData().toUInt();
    bool slaveMidi = m_midiSlaveCheck->isChecked();

    m_activeSampleRate = rate;

    // Set default sink
    QProcess::execute("pactl", QStringList() << "set-default-sink" << interface);

    // Set force-rate
    QProcess::execute("pw-metadata", QStringList() << "-n" << "settings" << "0" << "clock.force-rate" << QString::number(rate));

    // Set force-quantum
    QProcess::execute("pw-metadata", QStringList() << "-n" << "settings" << "0" << "clock.force-quantum" << QString::number(quantum));

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

    updateGlobalStatus("✓ Sync Active", "Audio settings updated successfully.", true);
}

// =============================================================================
// CLLS Calibration loops
// =============================================================================
void MainWindow::startCllsCalibration() {
    if (m_cllsProcess && m_cllsProcess->state() == QProcess::Running) {
        m_cllsProcess->kill();
        return;
    }

    m_runCllsBtn->setEnabled(false);
    m_runCllsBtn->setText("Calibrating...");

    QString interface = m_audioInterfaceSelect->currentData().toString();
    QString channel = m_cllsChannelSelect->currentData().toString();

    m_cllsProcess = new QProcess(this);
    connect(m_cllsProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::readCllsOutput);
    connect(m_cllsProcess, &QProcess::finished, this, &MainWindow::handleCllsFinished);
    
    // Spawn pw_module_clls
    m_cllsProcess->start(QCoreApplication::applicationDirPath() + "/pw_module_clls");

    updateGlobalStatus("⚡ Calibrating", "Spawning CLLS aligner...", true);

    // Dynamic graph loopback auto-linker
    QTimer::singleShot(1500, this, [=]() {
        QString inputInterface = interface;
        inputInterface.replace("alsa_output", "alsa_input");

        QString outputInterface = interface;
        outputInterface.replace("alsa_input", "alsa_output");

        QString inputPort = "";
        QString outputPort = "";

        if (channel.contains("Channel 8")) {
            inputPort = QString("%1:capture_AUX7").arg(inputInterface);
            outputPort = QString("%1:playback_AUX7").arg(outputInterface);
        } else if (channel.contains("Channel 2")) {
            inputPort = QString("%1:capture_AUX1").arg(inputInterface);
            outputPort = QString("%1:playback_AUX1").arg(outputInterface);
        } else {
            inputPort = QString("%1:capture_AUX3").arg(inputInterface);
            outputPort = QString("%1:playback_AUX3").arg(outputInterface);
        }

        // Link
        QProcess::execute("pw-link", QStringList() << "CLLS-Aligner:output_0" << outputPort);
        QProcess::execute("pw-link", QStringList() << inputPort << "CLLS-Aligner:input_0");
        QProcess::execute("pw-link", QStringList() << QString("%1:capture_AUX0").arg(inputInterface) << "CLLS-Aligner:input_1");
        QProcess::execute("pw-link", QStringList() << QString("%1:capture_AUX1").arg(inputInterface) << "CLLS-Aligner:input_2");
    });
}

void MainWindow::readCllsOutput() {
    while (m_cllsProcess->canReadLine()) {
        QString line = QString::fromUtf8(m_cllsProcess->readLine()).trimmed();
        
        // Parse [CLLS STATUS] Measured RTT: <rtt> samples | Applied Offset: +<delay> samples
        if (line.contains("[CLLS STATUS]")) {
            QRegularExpression rttRegex("Measured RTT:\\s*([\\d.]+)\\s*samples");
            QRegularExpression offsetRegex("Applied Offset:\\s*([+-]?[\\d.]+)\\s*samples");

            QRegularExpressionMatch rttMatch = rttRegex.match(line);
            if (rttMatch.hasMatch()) {
                float rttSamples = rttMatch.captured(1).toFloat();
                float rttMs = rttSamples / (m_activeSampleRate / 1000.0f);
                if (m_cllsRttVal) m_cllsRttVal->setText(QString("%1 ms (%2 samples)").arg(rttMs, 0, 'f', 3).arg(rttSamples, 0, 'f', 1));
            }

            QRegularExpressionMatch offsetMatch = offsetRegex.match(line);
            if (offsetMatch.hasMatch()) {
                float offsetSamples = offsetMatch.captured(1).toFloat();
                float offsetMs = offsetSamples / (m_activeSampleRate / 1000.0f);
                if (m_cllsCorrectionVal) m_cllsCorrectionVal->setText(QString("%1%2 ms (%3 samples)").arg(offsetMs >= 0 ? "+" : "").arg(offsetMs, 0, 'f', 3).arg(offsetSamples, 0, 'f', 1));
            }

            // Phase jitter simulation
            int jitterNs = 100 + rand() % 400;
            float jitterSamples = jitterNs / 1000000000.0f * m_activeSampleRate;
            if (m_cllsJitterVal) m_cllsJitterVal->setText(QString("±%1 ns (±%2 samples)").arg(jitterNs).arg(jitterSamples, 0, 'f', 4));

            if (m_cllsStatusBadge) {
                m_cllsStatusBadge->setText("Locked");
                m_cllsStatusBadge->setProperty("class", "badge badgeBlue");
                m_cllsStatusBadge->style()->unpolish(m_cllsStatusBadge);
                m_cllsStatusBadge->style()->polish(m_cllsStatusBadge);
            }
        }
    }
}

void MainWindow::handleCllsFinished() {
    m_runCllsBtn->setEnabled(true);
    m_runCllsBtn->setText("Calibrate CLLS Latency");
    updateGlobalStatus("✓ CLLS Finished", "CLLS Loopback Calibration run complete.", true);
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
    link.start("pw-link", QStringList() << "-io");
    link.waitForFinished(1000);
    QString stdoutStr = QString::fromUtf8(link.readAllStandardOutput());
    QTextStream stream(&stdoutStr);
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (!line.isEmpty()) {
            ports.append(line);
        }
    }
    // Fallbacks
    if (ports.isEmpty()) {
        ports.append("alsa_input.usb-Audient_EVO4-00.pro-input-0:capture_AUX0");
        ports.append("alsa_input.usb-Audient_EVO4-00.pro-input-0:capture_AUX1");
        ports.append("alsa_output.usb-Audient_EVO4-00.pro-output-0:playback_AUX0");
        ports.append("alsa_output.usb-Audient_EVO4-00.pro-output-0:playback_AUX1");
    }
    return ports;
}

// =============================================================================
// Virtual DSP Rack profile load/saves
// =============================================================================
void MainWindow::loadVdcProfiles() {
    QString configDir = QDir::homePath() + "/.config/arthur/profiles";
    QDir().mkpath(configDir);

    QDir dir(configDir);
    m_availableProfiles.clear();
    QStringList files = dir.entryList(QStringList() << "*.vdcp", QDir::Files);
    for (const QString &file : files) {
        m_availableProfiles.append(QFileInfo(file).baseName());
    }

    if (m_profileSelect) {
        m_profileSelect->blockSignals(true);
        m_profileSelect->clear();
        for (const QString &prof : m_availableProfiles) {
            m_profileSelect->addItem(prof + ".vdcp", prof);
        }
        m_profileSelect->addItem("Create Empty Profile...", "_create_empty_");
        m_profileSelect->blockSignals(false);
    }

    if (!m_availableProfiles.isEmpty()) {
        loadProfile(m_availableProfiles.first());
    } else {
        // Create default profiles if none exist
        QList<QString> installed = scanInstalledVst3Plugins();
        
        m_currentProfile.profile_name = "Tracking_Session";
        m_currentProfile.cores_allocated = "4-7";
        m_currentProfile.sample_rate = 48000;
        m_currentProfile.buffer_size = 128;
        m_currentProfile.vdc_slots.clear();

        if (!installed.isEmpty()) {
            m_currentProfile.vdc_slots.append({1, "CH 1 INSERTS", installed[0], true, "capture_AUX0", "playback_AUX0"});
            if (installed.size() > 1) {
                m_currentProfile.vdc_slots.append({2, "CH 1 INSERTS", installed[1], true, "capture_AUX0", "playback_AUX0"});
            }
            if (installed.size() > 2) {
                m_currentProfile.vdc_slots.append({1, "CH 2 INSERTS", installed[2], true, "capture_AUX1", "playback_AUX1"});
            }
        } else {
            m_currentProfile.vdc_slots.append({1, "CH 1 INSERTS", "CyberDenoiserPro", true, "capture_AUX0", "playback_AUX0"});
        }

        saveCurrentProfile();

        m_currentProfile.profile_name = "Mixdown_Mastering";
        m_currentProfile.sample_rate = 96000;
        m_currentProfile.buffer_size = 256;
        m_currentProfile.vdc_slots.clear();
        if (!installed.isEmpty()) {
            m_currentProfile.vdc_slots.append({1, "CH 3 INSERTS", installed.first(), true, "capture_AUX2", "playback_AUX2"});
        } else {
            m_currentProfile.vdc_slots.append({1, "CH 3 INSERTS", "THE MIDS ROOM", true, "capture_AUX2", "playback_AUX2"});
        }

        saveCurrentProfile();
        loadVdcProfiles(); // Reload dropdown
    }
}

void MainWindow::loadProfile(const QString &name) {
    QString path = QDir::homePath() + QString("/.config/arthur/profiles/%1.vdcp").arg(name);
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray data = file.readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        m_currentProfile.profile_name = obj["profile_name"].toString();
        m_currentProfile.cores_allocated = obj["cores_allocated"].toString();
        m_currentProfile.sample_rate = obj["sample_rate"].toInt();
        m_currentProfile.buffer_size = obj["buffer_size"].toInt();
        m_currentProfile.vdc_slots.clear();

        QJsonArray slotsArr = obj["slots"].toArray();
        for (int i = 0; i < slotsArr.size(); ++i) {
            QJsonObject slotObj = slotsArr[i].toObject();
            VdcSlot slot;
            slot.slot_id = slotObj["slot_id"].toInt();
            slot.channel_name = slotObj["channel_name"].toString();
            slot.vst3_dll_path = slotObj["vst3_dll_path"].toString();
            slot.active = slotObj["active"].toBool();
            slot.input_source = slotObj["input_source"].toString();
            slot.output_destination = slotObj["output_destination"].toString();
            m_currentProfile.vdc_slots.append(slot);
        }

        m_activeSampleRate = m_currentProfile.sample_rate;
        if (m_sampleRateSelect) {
            int idx = m_sampleRateSelect->findData(m_currentProfile.sample_rate);
            if (idx >= 0) m_sampleRateSelect->setCurrentIndex(idx);
        }
        if (m_bufferSizeSelect) {
            int idx = m_bufferSizeSelect->findData(m_currentProfile.buffer_size);
            if (idx >= 0) m_bufferSizeSelect->setCurrentIndex(idx);
        }
        renderRackGrid();
    }
}

void MainWindow::onProfileChanged(const QString &profileName) {
    QString val = m_profileSelect->currentData().toString();
    if (val == "_create_empty_") {
        // Prompt dialog
        QDialog diag(this);
        diag.setWindowTitle("Create Empty Profile");
        diag.setMinimumWidth(300);
        QVBoxLayout *l = new QVBoxLayout(&diag);
        QLabel *lbl = new QLabel("Enter profile name:", &diag);
        QLineEdit *edit = new QLineEdit(&diag);
        edit->setProperty("class", "custom-input");
        QHBoxLayout *btnLayout = new QHBoxLayout();
        QPushButton *ok = new QPushButton("Create", &diag);
        ok->setProperty("class", "action-btn");
        QPushButton *cancel = new QPushButton("Cancel", &diag);
        cancel->setProperty("class", "settings-btn");
        btnLayout->addWidget(cancel);
        btnLayout->addWidget(ok);
        l->addWidget(lbl);
        l->addWidget(edit);
        l->addLayout(btnLayout);

        connect(ok, &QPushButton::clicked, &diag, &QDialog::accept);
        connect(cancel, &QPushButton::clicked, &diag, &QDialog::reject);

        if (diag.exec() == QDialog::Accepted && !edit->text().trimmed().isEmpty()) {
            QString cleanName = edit->text().trimmed().replace(" ", "_");
            m_currentProfile.profile_name = cleanName;
            m_currentProfile.cores_allocated = "4-7";
            m_currentProfile.sample_rate = m_activeSampleRate;
            m_currentProfile.buffer_size = 128;
            m_currentProfile.vdc_slots.clear();

            saveCurrentProfile();
            loadVdcProfiles();
            // Select newly created profile
            int idx = m_profileSelect->findData(cleanName);
            if (idx >= 0) m_profileSelect->setCurrentIndex(idx);
        } else {
            // Restore previous profile
            int idx = m_profileSelect->findData(m_currentProfile.profile_name);
            if (idx >= 0) {
                m_profileSelect->blockSignals(true);
                m_profileSelect->setCurrentIndex(idx);
                m_profileSelect->blockSignals(false);
            }
        }
    } else if (!val.isEmpty()) {
        loadProfile(val);
    }
}

void MainWindow::saveCurrentProfile() {
    QString path = QDir::homePath() + QString("/.config/arthur/profiles/%1.vdcp").arg(m_currentProfile.profile_name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonObject obj;
        obj["profile_name"] = m_currentProfile.profile_name;
        obj["cores_allocated"] = m_currentProfile.cores_allocated.isEmpty() ? "4-7" : m_currentProfile.cores_allocated;
        obj["sample_rate"] = static_cast<int>(m_currentProfile.sample_rate);
        obj["buffer_size"] = static_cast<int>(m_currentProfile.buffer_size);

        QJsonArray slotsArr;
        for (const VdcSlot &slot : m_currentProfile.vdc_slots) {
            QJsonObject slotObj;
            slotObj["slot_id"] = slot.slot_id;
            slotObj["channel_name"] = slot.channel_name;
            slotObj["vst3_dll_path"] = slot.vst3_dll_path;
            slotObj["active"] = slot.active;
            slotObj["input_source"] = slot.input_source;
            slotObj["output_destination"] = slot.output_destination;
            slotsArr.append(slotObj);

            // Notify arthur-daemon client load for active VST guest slots
            if (slot.active) {
                QString shmName = QString("arthur_%1").arg(slot.vst3_dll_path);
                QString msg = QString("LOAD %1 %2").arg(shmName).arg(slot.vst3_dll_path);
                
                QLocalSocket socket;
                socket.connectToServer("/tmp/arthur.sock");
                if (socket.waitForConnected(200)) {
                    socket.write(msg.toUtf8());
                    socket.waitForBytesWritten(200);
                }
            }
        }
        obj["slots"] = slotsArr;

        QJsonDocument doc(obj);
        file.write(doc.toJson());
        file.close();

        updateGlobalStatus("✓ Saved Profile", QString("Profile '%1' saved and synced successfully.").arg(m_currentProfile.profile_name), true);
    }
}

void MainWindow::renderRackGrid() {
    // Recreate grid
    QLayoutItem *child;
    if (m_rackGridWidget->layout() != nullptr) {
        while ((child = m_rackGridWidget->layout()->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }
        delete m_rackGridWidget->layout();
    }

    QHBoxLayout *gridOuter = new QHBoxLayout(m_rackGridWidget);
    gridOuter->setSpacing(20);
    gridOuter->setContentsMargins(0, 0, 0, 0);

    QStringList channels = {"CH 1 INSERTS", "CH 2 INSERTS", "CH 3 INSERTS"};
    for (const QString &ch : channels) {
        QFrame *chCol = new QFrame(m_rackGridWidget);
        chCol->setProperty("class", "card");
        QVBoxLayout *chColLayout = new QVBoxLayout(chCol);
        chColLayout->setContentsMargins(15, 15, 15, 15);
        chColLayout->setSpacing(12);

        QLabel *chHeader = new QLabel(ch, chCol);
        chHeader->setAlignment(Qt::AlignCenter);
        chHeader->setStyleSheet("font-weight: 800; font-size: 11px; color: #a0a5b5; letter-spacing: 1.5px; border-bottom: 1px solid rgba(255,255,255,0.06); padding-bottom: 8px; margin-bottom: 5px;");
        chColLayout->addWidget(chHeader);

        for (int slotId = 1; slotId <= 3; ++slotId) {
            // Find slot
            VdcSlot matchingSlot;
            bool found = false;
            for (const VdcSlot &s : m_currentProfile.vdc_slots) {
                if (s.channel_name == ch && s.slot_id == slotId) {
                    matchingSlot = s;
                    found = true;
                    break;
                }
            }

            QFrame *slotFrame = new QFrame(chCol);
            QHBoxLayout *slotFrameLayout = new QHBoxLayout(slotFrame);
            slotFrameLayout->setContentsMargins(12, 0, 12, 0);
            slotFrameLayout->setSpacing(10);

            if (found) {
                slotFrame->setProperty("class", "rack-slot");
                
                QLabel *numLabel = new QLabel(QString::number(slotId), slotFrame);
                numLabel->setFixedWidth(20);
                numLabel->setStyleSheet("font-weight: 800; color: #a0a5b5; font-size: 11px;");

                QLabel *nameLabel = new QLabel(matchingSlot.vst3_dll_path, slotFrame);
                nameLabel->setStyleSheet("font-weight: 600;");
                nameLabel->setToolTip(QString("In: %1\nOut: %2").arg(matchingSlot.input_source).arg(matchingSlot.output_destination));

                QPushButton *removeBtn = new QPushButton("✖", slotFrame);
                removeBtn->setFixedSize(20, 20);
                removeBtn->setCursor(Qt::PointingHandCursor);
                removeBtn->setStyleSheet("background: transparent; border: none; color: rgba(255,255,255,0.3); font-size: 10px;");
                removeBtn->setToolTip("Remove VST3 Insert");
                connect(removeBtn, &QPushButton::clicked, this, [=]() {
                    handleRemoveSlotClick(ch, slotId);
                });

                slotFrameLayout->addWidget(numLabel);
                slotFrameLayout->addWidget(nameLabel);
                slotFrameLayout->addStretch();
                slotFrameLayout->addWidget(removeBtn);
            } else {
                slotFrame->setProperty("class", "rack-slot rack-slot-empty");
                
                QLabel *numLabel = new QLabel(QString::number(slotId), slotFrame);
                numLabel->setFixedWidth(20);
                numLabel->setStyleSheet("font-weight: 800; color: rgba(255,255,255,0.2); font-size: 11px;");

                QPushButton *addBtn = new QPushButton("+ Add VST3 Insert", slotFrame);
                addBtn->setCursor(Qt::PointingHandCursor);
                addBtn->setStyleSheet("background: transparent; border: none; color: #a0a5b5; font-weight: 600; text-align: left;");
                connect(addBtn, &QPushButton::clicked, this, [=]() {
                    handleAddSlotClick(ch, slotId);
                });

                slotFrameLayout->addWidget(numLabel);
                slotFrameLayout->addWidget(addBtn);
                slotFrameLayout->addStretch();
            }

            slotFrame->style()->unpolish(slotFrame);
            slotFrame->style()->polish(slotFrame);
            chColLayout->addWidget(slotFrame);
        }
        gridOuter->addWidget(chCol);
    }
}

void MainWindow::handleRemoveSlotClick(const QString &channelName, int slotId) {
    m_currentProfile.vdc_slots.erase(
        std::remove_if(m_currentProfile.vdc_slots.begin(), m_currentProfile.vdc_slots.end(),
                       [=](const VdcSlot &s) { return s.channel_name == channelName && s.slot_id == slotId; }),
        m_currentProfile.vdc_slots.end()
    );
    renderRackGrid();
}

void MainWindow::handleAddSlotClick(const QString &channelName, int slotId) {
    QDialog dialog(this);
    dialog.setWindowTitle("Add VST3 Insert");
    dialog.setMinimumWidth(380);
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setSpacing(15);
    layout->setContentsMargins(20, 20, 20, 20);

    QLabel *header = new QLabel("Insert VST3 into slot", &dialog);
    header->setStyleSheet("font-weight: 800; font-size: 16px;");
    layout->addWidget(header);

    // VST3 List Combo
    QVBoxLayout *vstBox = new QVBoxLayout();
    vstBox->setSpacing(6);
    QLabel *vstLabel = new QLabel("VST3 Plugin:", &dialog);
    vstLabel->setStyleSheet("color: #a0a5b5; font-size: 12px; font-weight: 600;");
    QComboBox *vstCombo = new QComboBox(&dialog);
    vstCombo->setProperty("class", "custom-select");
    
    QList<QString> installed = scanInstalledVst3Plugins();
    for (const QString &plugin : installed) {
        vstCombo->addItem(plugin, plugin);
    }
    if (vstCombo->count() == 0) {
        vstCombo->addItem("CyberDenoiserPro (Mock/Fallback)", "CyberDenoiserPro");
        vstCombo->addItem("THE MIDS ROOM (Mock/Fallback)", "THE MIDS ROOM");
        vstCombo->addItem("Strobe Poly Tuner (Mock/Fallback)", "Strobe Poly Tuner");
    }
    vstBox->addWidget(vstLabel);
    vstBox->addWidget(vstCombo);
    layout->addLayout(vstBox);

    // Ports
    QList<QString> ports = queryPipeWirePorts();
    QList<QString> inputPorts;
    QList<QString> outputPorts;
    for (const QString &port : ports) {
        if (port.toLower().contains("capture") || port.toLower().contains("output")) {
            inputPorts.append(port);
        }
        if (port.toLower().contains("playback") || port.toLower().contains("input")) {
            outputPorts.append(port);
        }
    }
    if (inputPorts.isEmpty()) inputPorts = ports;
    if (outputPorts.isEmpty()) outputPorts = ports;

    QVBoxLayout *inBox = new QVBoxLayout();
    inBox->setSpacing(6);
    QLabel *inLabel = new QLabel("Input Source (PipeWire Port):", &dialog);
    inLabel->setStyleSheet("color: #a0a5b5; font-size: 12px; font-weight: 600;");
    QComboBox *inCombo = new QComboBox(&dialog);
    inCombo->setProperty("class", "custom-select");
    for (const QString &port : inputPorts) {
        inCombo->addItem(port.split(":").last(), port);
    }
    inBox->addWidget(inLabel);
    inBox->addWidget(inCombo);
    layout->addLayout(inBox);

    QVBoxLayout *outBox = new QVBoxLayout();
    outBox->setSpacing(6);
    QLabel *outLabel = new QLabel("Output Destination (PipeWire Port):", &dialog);
    outLabel->setStyleSheet("color: #a0a5b5; font-size: 12px; font-weight: 600;");
    QComboBox *outCombo = new QComboBox(&dialog);
    outCombo->setProperty("class", "custom-select");
    for (const QString &port : outputPorts) {
        outCombo->addItem(port.split(":").last(), port);
    }
    outBox->addWidget(outLabel);
    outBox->addWidget(outCombo);
    layout->addLayout(outBox);

    QHBoxLayout *btns = new QHBoxLayout();
    QPushButton *addBtn = new QPushButton("Add Insert", &dialog);
    addBtn->setProperty("class", "action-btn");
    QPushButton *cancelBtn = new QPushButton("Cancel", &dialog);
    cancelBtn->setProperty("class", "settings-btn");
    btns->addWidget(cancelBtn);
    btns->addWidget(addBtn);
    layout->addLayout(btns);

    connect(addBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        QString vstName = vstCombo->currentData().toString();
        QString inPort = inCombo->currentData().toString();
        QString outPort = outCombo->currentData().toString();

        m_currentProfile.vdc_slots.append({slotId, channelName, vstName, true, inPort, outPort});
        renderRackGrid();
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
        QFile::copy("/app/bin/arthur-installer-bridge.sh", configDir + "/arthur-installer-bridge.sh");
        QFile::copy("/app/lib/arthur_bridge.so", configDir + "/arthur_bridge.so");

        m_installerProcess->start("flatpak-spawn", QStringList() << "--host" << "bash" << (configDir + "/arthur-installer-bridge.sh") << filePath);
    } else {
        m_installerProcess->start("bash", QStringList() << "./arthur-installer-bridge.sh" << filePath);
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

    m_tuningStatusCard->setVisible(false);
    m_tuningStatusCard->setVisible(false);
    m_tuningProgressCard->setVisible(true);

    m_tuningProgressBar->setValue(10);
    m_tuningStatusText->setText("Requesting administrator access (pkexec)...");
    m_tuningConsole->clear();
    m_tuningConsole->append("Starting System Setup Wizard...\n");

    m_tuningProcess = new QProcess(this);
    connect(m_tuningProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::readTuningOutput);
    connect(m_tuningProcess, &QProcess::readyReadStandardError, this, &MainWindow::readTuningOutput);

    bool flatpakMode = QFile::exists("/.flatpak-info");
    if (flatpakMode) {
        QString configDir = QDir::homePath() + "/.config/arthur";
        QDir().mkpath(configDir);
        QFile::copy("/app/bin/vdc_tune.sh", configDir + "/vdc_tune.sh");
        
        m_tuningProcess->start("flatpak-spawn", QStringList() << "--host" << "pkexec" << "bash" << (configDir + "/vdc_tune.sh") << "--cores" << targetCores);
    } else {
        m_tuningProcess->start("pkexec", QStringList() << "bash" << "./vdc_tune.sh" << "--cores" << targetCores);
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
