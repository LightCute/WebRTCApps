#pragma once
#include <QMainWindow>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QJsonObject>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QTimer>
#include <QAction>
#include <QToolBar>
#include <QStatusBar>
#include "webrtc_process_manager.h"
#include "control_channel.h"
#include "voice_chat_client.h"
#include "voice_chat_manager.h"
#include <QFile>
#include <QTextStream>
#include "shm_video_source.h"
#include "ui_mainwindow.h"

class GlVideoWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onProcessStateChanged(WebRtcProcessManager::State state);
    void onControlConnected();
    void onControlDisconnected();
    void onPeerListReceived(QVector<PeerInfo> peers);
    void onPeerDoubleClicked(QTreeWidgetItem* item, int column);

private:
    void initUi();
    void initConnections();
    void log(const QString& msg);
    void chatLog(const QString& msg);
    void startVideoSources();
    void stopVideoSources();
    void switchToListMode();
    void switchToCallMode();
    void doCall(int peer_id);
    void doSendMessage();
    void onUpdateStats();
    void toggleVoiceChat();
    void stopVoiceChat();
    void updateVoiceChatEnabled();
    void toggleAi(const QString& type);
    void resetAllAiButtons();
    void triggerDangerWarning(const QString& label);
    void checkDangerLabels(const QVector<struct Detection>& detections);

    // UI (from .ui file — remote_video_ is the only video widget still
    // auto-generated; it gets reparented into remote_container_ at startup)
    Ui::MainWindow ui;

    // --- Runtime pointers (found from .ui or created in initUi) ---
    QWidget* remote_container_ = nullptr;   // from .ui (findChild)
    GlVideoWidget* local_video_ = nullptr;  // C++ created (PIP overlay)
    QLabel* stats_label_ = nullptr;         // C++ created (bottom overlay)

    // Toolbar (managed in C++ — dynamic separators + embedded QLineEdit)
    QToolBar* toolbar_;
    QLineEdit* peer_input_;
    QAction* action_connect_;
    QAction* action_disconnect_;
    QAction* action_call_;
    QAction* action_hangup_;
    QAction* action_ai_ = nullptr;
    QAction* action_fall_ = nullptr;
    QAction* action_fire_ = nullptr;
    QAction* action_voice_chat_ = nullptr;

    // Components
    WebRtcProcessManager* proc_mgr_;
    ControlChannel* channel_;
    VoiceChatManager* voice_mgr_ = nullptr;
    VoiceChatClient* voice_client_ = nullptr;
    bool voice_chat_running_ = false;
    bool voice_chat_startup_complete_ = false;
    QString ai_active_type_;
    ShmVideoSource* local_source_ = nullptr;
    ShmVideoSource* remote_source_ = nullptr;

    // Log
    QFile* log_file_ = nullptr;

    // State
    QString server_addr_ = "120.79.210.6";
    int server_port_ = 8888;
    QVector<PeerInfo> peers_;
    bool is_call_active_ = false;
    bool first_connect_ = true;
    QTimer* key_timer_ = nullptr;
    int held_key_ = 0;

    // Danger warning overlay
    QWidget* danger_overlay_ = nullptr;
    QLabel* danger_label_ = nullptr;
    QTimer* danger_timer_ = nullptr;
    int danger_blinks_ = 0;

    // Stats
    QTimer* stats_timer_;
    int remote_width_ = 0, remote_height_ = 0;
    int local_width_ = 0, local_height_ = 0;
    void onStatsReceived(const QJsonObject& stats);
    bool stats_monitoring_ = false;
    uint64_t remote_frame_count_ = 0;
    uint64_t local_frame_count_ = 0;
};
