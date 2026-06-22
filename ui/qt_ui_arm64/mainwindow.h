#pragma once
#include <QMainWindow>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QTimer>
#include <QAction>
#include <QToolBar>
#include <QStatusBar>
#include <QFile>
#include <QTextStream>
#include "webrtc_process_manager.h"
#include "control_channel.h"
#include "dma_buf_video_source.h"
#include "voice_chat_client.h"
#include "voice_chat_manager.h"
#include "ui_mainwindow.h"

class AiReceiver;
class GlVideoWidget;
class SerialWorker;

enum class AiType { None, YoloV5, Fall, Fire };

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

private slots:
    void onProcessStateChanged(WebRtcProcessManager::State state);
    void onControlConnected();
    void onControlDisconnected();
    void onPeerListReceived(QVector<PeerInfo> peers);
    void onPeerDoubleClicked(QTreeWidgetItem* item, int column);

    void onStatsReceived(const QJsonObject& stats);
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
    void handleRemoteKey(const QString& key);
    void sendSerialJson(const QString& json);
    void startKeyRepeat();
    void stopKeyRepeat();
    bool startAi(AiType type);
    void stopAi();
    void resetAllAiButtons();
    void onAiDetections(QVector<struct Detection> detections);

    // UI (from .ui file — central widget tree)
    Ui::MainWindow ui;

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
    DmaBufVideoSource* local_source_ = nullptr;
    DmaBufVideoSource* remote_source_ = nullptr;
    SerialWorker* serial_ = nullptr;

    // AI detection
    AiReceiver* ai_receiver_ = nullptr;
    QProcess* ai_proc_ = nullptr;
    AiType ai_type_ = AiType::None;

    // Log
    QFile* log_file_ = nullptr;

    // Keyboard repeat timer
    QTimer* key_timer_ = nullptr;
    int held_key_ = 0;

    // State
    QString server_addr_ = "120.79.210.6";
    int server_port_ = 8888;
    QVector<PeerInfo> peers_;
    bool is_call_active_ = false;
    bool first_connect_ = true;
    bool voice_chat_running_ = false;
    bool voice_chat_startup_complete_ = false;

    void toggleVoiceChat();
    void stopVoiceChat();
    void updateVoiceChatEnabled();

    // Stats
    QTimer* stats_timer_;
    bool stats_monitoring_ = false;
    int remote_width_ = 0, remote_height_ = 0;
    int local_width_ = 0, local_height_ = 0;
    uint64_t remote_frame_count_ = 0;
    uint64_t local_frame_count_ = 0;
};
