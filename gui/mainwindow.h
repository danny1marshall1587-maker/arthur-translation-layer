#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QProgressBar>
#include <QTextEdit>
#include <QTimer>
#include <QProcess>
#include <QList>
#include <QDialog>
#include <QDial>
#include <QLineEdit>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QStackedWidget>
#include <QMessageBox>
#include <QRandomGenerator>
#include <thread>

// --- Custom Drag and Drop Zone for Windows Installers ---
class DropZoneWidget : public QFrame {
    Q_OBJECT
public:
    explicit DropZoneWidget(QWidget *parent = nullptr);

signals:
    void fileDropped(const QString &filePath);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};

// --- Main Audio Control Window ---


class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // Tab switching
    void showDashboard();
    void showInstaller();
    void showSettings();
    void showConsole();

    // System Status Updates
    void querySystemStatus();

    // Audio Settings
    void loadAudioConfig();
    void applyAudioConfig();
    void startCllsCalibration(int slotIdx);
    void readCllsOutput(int slotIdx);
    void handleCllsFinished(int slotIdx, int exitCode, QProcess::ExitStatus status);
    void populatePortsForSlot(int slotIdx);
    void saveAudioConfig();


    // VST3 Installer Wizard
    void startInstaller(const QString &filePath);
    void readInstallerOutput();
    void handleInstallerFinished(int exitCode, QProcess::ExitStatus status);
    void selectInstallerFile();

    // Tuning Wizard
    void runSystemTuning();
    void readTuningOutput();
    void handleTuningFinished(int exitCode, QProcess::ExitStatus status);
    void updateMeterAnimations();
    void switchConsolePage(int pageIdx);
    void loadPlugin(int slotIdx);
    void unloadPlugin(int slotIdx);
    void openPluginGui(int slotIdx);
    void handleSlotClicked(int channelIdx, int slotIdx, bool isBus);
    void toggleSlotBypass(int channelIdx, int slotIdx, bool isBus);
    void toggleAutoGain(int channelIdx, bool isBus);
    void toggleSlotAutoGain(int channelIdx, int slotIdx, bool isBus);
    void openPluginEditorWindow(const QString &shmName);
    void loadPluginAtSlot(int channelIdx, int slotIdx, const QString &pluginName, bool isBus);
    void unloadPluginAtSlot(int channelIdx, int slotIdx, bool isBus);

private:
    void initUi();
    void setupGlobalStylesheet();
    void ensureDaemonRunning();
    void sendDaemonCommand(const QString &cmd);
    QList<QString> scanInstalledVst3Plugins();
    QList<QString> queryPipeWirePorts();
    void updateMidiSyncCard(bool active);
    void updateGlobalStatus(const QString &title, const QString &description, bool active);

    // --- State variables ---
    QStringList m_activePreloadedShms;
    unsigned int m_activeSampleRate;
    bool m_isUpdatingConfig;

    // --- UI Layout Pointers ---
    QWidget *m_sidebar;
    QStackedWidget *m_contentArea;
    QWidget *m_dashboardTab;
    QWidget *m_installerTab;
    QWidget *m_settingsTab;
    QWidget *m_consoleTab;

    struct PluginSlot {
        QPushButton *slotBtn = nullptr;
        QPushButton *bypassBtn = nullptr;
        QPushButton *autoGainBtn = nullptr;  // per-slot RMS level-match toggle
        QString pluginName;
        bool bypassed = false;
        bool autoGain = false;
        QString shmName;
    };

    struct ConsoleChannelRow {
        QLabel *nameLabel = nullptr;
        QComboBox *modeSelect = nullptr;
        PluginSlot pluginSlots[4];
        QSlider *volumeSlider = nullptr;
        QProgressBar *levelMeter = nullptr;
        QDial *sendReverb = nullptr;
        QDial *sendDelay = nullptr;
        QPushButton *muteBtn = nullptr;
        QPushButton *soloBtn = nullptr;
        QPushButton *autoGainBtn = nullptr;
        bool autoGainEnabled = false;
        QString shmName;
    };
    QList<ConsoleChannelRow> m_consoleRows;

    struct ConsoleBusRow {
        QString name;
        PluginSlot pluginSlots[4];
        QSlider *volumeSlider = nullptr;
        QProgressBar *levelMeter = nullptr;
        QPushButton *muteBtn = nullptr;
        QPushButton *autoGainBtn = nullptr;
        bool autoGainEnabled = false;
    };
    QList<ConsoleBusRow> m_consoleBusses;

    QStackedWidget *m_consoleStack = nullptr;
    QPushButton *m_btnInputsPage = nullptr;
    QPushButton *m_btnBussesPage = nullptr;
    QPushButton *m_btnInstrumentsPage = nullptr;

    QWidget *m_consoleChannelContainer = nullptr;
    QWidget *m_consoleBussesContainer = nullptr;
    QPushButton *m_consoleScanBtn = nullptr;

    struct InstrumentSlotRow {
        QLabel *slotLabel = nullptr;
        QLabel *statusDot = nullptr;
        QLabel *statusText = nullptr;
        QComboBox *pluginSelect = nullptr;
        QPushButton *loadBtn = nullptr;
        QPushButton *guiBtn = nullptr;
        QString shmName;
    };
    QList<InstrumentSlotRow> m_instrumentSlots;

    void rebuildConsoleChannels();
    void applyChannelMode(int rowIdx, int mode);
    void saveMixerConfig();
    void loadMixerConfig();

    // Sidebar status
    QLabel *m_statusDot;
    QLabel *m_statusTextLabel;

    // Dashboard Cards
    QLabel *m_dspActiveBadge;
    QLabel *m_dspCoresVal;
    QLabel *m_dspLoadText;
    QProgressBar *m_dspLoadProgress;
    QLabel *m_cllsStatusBadge;
    QLabel *m_cllsRttVal;
    QLabel *m_cllsCorrectionVal;
    QLabel *m_cllsJitterVal;
    QLabel *m_midiStatusText;
    QLabel *m_midiJitterBadge;
    QList<QWidget*> m_latencyBars;

    // Settings elements
    QComboBox *m_audioInterfaceSelect;
    QComboBox *m_sampleRateSelect;
    QComboBox *m_bufferSizeSelect;
    QCheckBox *m_midiSlaveCheck;
    QSlider *m_coresSlider;
    QLabel *m_coresStatusLabel;

    // CLLS Slots for 3 Audio Interfaces to Sync
    struct CllsSlot {
        QProcess *process = nullptr;
        QComboBox *interfaceSelect = nullptr;
        QComboBox *playbackPortSelect = nullptr;
        QComboBox *capturePortSelect = nullptr;
        QComboBox *audioSrcSelect = nullptr;
        QComboBox *audioDstSelect = nullptr;
        QPushButton *runBtn = nullptr;
        QLabel *statusBadge = nullptr;
        QLabel *rttValLabel = nullptr;
        QLabel *offsetValLabel = nullptr;
        QLabel *jitterValLabel = nullptr;
        bool isCalibrating = false;
    };
    CllsSlot m_cllsSlots[3];

    // Installer Wizard UI
    DropZoneWidget *m_dropZone;
    QWidget *m_installProgressCard;
    QProgressBar *m_installProgressBar;
    QLabel *m_installStatusText;
    QTextEdit *m_installConsole;
    QWidget *m_installStatusCard;
    QLabel *m_installStatusIcon;
    QLabel *m_installStatusTitle;
    QLabel *m_installStatusDesc;

    // Tuning Wizard UI
    QWidget *m_tuningProgressCard;
    QProgressBar *m_tuningProgressBar;
    QLabel *m_tuningStatusText;
    QTextEdit *m_tuningConsole;
    QWidget *m_tuningStatusCard;
    QLabel *m_tuningStatusTitle;
    QLabel *m_tuningStatusDesc;
    QWidget *m_tuningCard;


    // Background System Processes
    QProcess *m_installerProcess;
    QProcess *m_tuningProcess;
    QTimer *m_statusTimer;
    QTimer *m_midiJitterTimer;
    QTimer *m_meterTimer;
    QStringList m_lastScannedShms;
};

#endif // MAINWINDOW_H
