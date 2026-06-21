#include "mainwindow.h"
#include "gl_video_widget.h"
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QCloseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QSplitter>
#include <QDebug>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    initUi();

    QString app_dir = QCoreApplication::applicationDirPath();
    QString runtime_dir = qEnvironmentVariable("WEBRTC_RUNTIME_DIR",
                                               QString("/tmp/webrtc_runtime"));
    QDir().mkpath(runtime_dir);
    log_file_ = new QFile(runtime_dir + "/ui_x64.log", this);
    log_file_->open(QIODevice::Append | QIODevice::Text);

    QString daemon_path = qEnvironmentVariable("WEBRTC_DAEMON_PATH",
        app_dir + "/apps_peerconnection_client");

    proc_mgr_ = new WebRtcProcessManager(daemon_path, this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("WEBRTC_RUNTIME_DIR", runtime_dir);
    proc_mgr_->process()->setProcessEnvironment(env);

    channel_ = new ControlChannel(runtime_dir + "/webrtc_ctrl.sock", this);

    initConnections();

    key_timer_ = new QTimer(this);
    key_timer_->setInterval(100);
    connect(key_timer_, &QTimer::timeout, this, [this]() {
        const char* data = nullptr;
        if (held_key_ == Qt::Key_W)      data = "w";
        else if (held_key_ == Qt::Key_A) data = "a";
        else if (held_key_ == Qt::Key_S) data = "s";
        else if (held_key_ == Qt::Key_D) data = "d";
        else if (held_key_ == Qt::Key_Up)    data = "i";
        else if (held_key_ == Qt::Key_Left)  data = "j";
        else if (held_key_ == Qt::Key_Down)  data = "k";
        else if (held_key_ == Qt::Key_Right) data = "l";
        if (data) channel_->cmdSendData(QString(data));
    });

    // Voice chat manager
    voice_mgr_ = new VoiceChatManager(this);
    connect(voice_mgr_, &VoiceChatManager::daemonLog, this, &MainWindow::log);

    QString vc_socket = runtime_dir + "/voice_chat.sock";
    voice_client_ = new VoiceChatClient(vc_socket, this);
    connect(voice_client_, &VoiceChatClient::asrResult, this,
            [this](const QString& t) { chatLog("You: " + t); });
    connect(voice_client_, &VoiceChatClient::llmResult, this,
            [this](const QString& t) { chatLog("Bot: " + t); });
    connect(voice_client_, &VoiceChatClient::errorOccurred, this,
            [this](const QString& e) { log("VoiceChat error: " + e); });
    // Wait for voice_chat server to signal ready, then start conversation
    connect(voice_client_, &VoiceChatClient::connected, this, [this]() {
        log("VoiceChat socket connected, waiting for ready...");
    });
    connect(voice_client_, &VoiceChatClient::ready, this, [this]() {
        log("VoiceChat ready, starting conversation");
        voice_client_->cmdStart();
    });

    connect(action_ai_, &QAction::triggered, this, [this]() {
        toggleAi("yolov5");
    });
    connect(action_fall_, &QAction::triggered, this, [this]() {
        toggleAi("fall");
    });
    connect(action_fire_, &QAction::triggered, this, [this]() {
        toggleAi("fire");
    });

    connect(action_voice_chat_, &QAction::triggered,
            this, &MainWindow::toggleVoiceChat);

    log("Starting WebRTC daemon...");
    proc_mgr_->start();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
    stopVoiceChat();
    channel_->cmdShutdown();
    proc_mgr_->stop();
    stopVideoSources();
    event->accept();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == remote_container_ && event->type() == QEvent::Resize) {
        int cw = remote_container_->width();
        int ch = remote_container_->height();

        // --- PIP (local_video_) ---
        constexpr double PIP_SCALE = 0.22;
        constexpr int PIP_MIN_W = 160;
        constexpr int PIP_MAX_W = 480;
        constexpr int PIP_MARGIN = 16;

        int pip_w = qBound(PIP_MIN_W, int(cw * PIP_SCALE), PIP_MAX_W);
        int pip_h = pip_w * 3 / 4;
        int pip_x = cw - pip_w - PIP_MARGIN;
        int pip_y = PIP_MARGIN;

        // Only update PIP geometry when it actually changes — during
        // QSplitter drag, Resize events fire at very high frequency;
        // unconditional setGeometry overwhelms the OpenGL render pipeline.
        QRect currentGeo = local_video_->geometry();
        if (currentGeo.x() != pip_x || currentGeo.y() != pip_y ||
            currentGeo.width() != pip_w || currentGeo.height() != pip_h) {
            local_video_->setGeometry(pip_x, pip_y, pip_w, pip_h);
        }

        // --- stats_label_ ---
        constexpr int STATS_W = 420;
        constexpr int STATS_H = 28;
        constexpr int STATS_BOTTOM_MARGIN = 8;
        int stats_x = (cw - STATS_W) / 2;
        int stats_y = ch - STATS_H - STATS_BOTTOM_MARGIN;
        stats_label_->setGeometry(stats_x, stats_y, STATS_W, STATS_H);

        // Keep Z-order correct
        local_video_->raise();
        stats_label_->raise();
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return;
    int k = event->key();
    if (k == Qt::Key_W || k == Qt::Key_A || k == Qt::Key_S || k == Qt::Key_D ||
        k == Qt::Key_Up || k == Qt::Key_Left || k == Qt::Key_Down || k == Qt::Key_Right) {
        held_key_ = k;
        key_timer_->start();
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return;
    if (event->key() == held_key_) {
        held_key_ = 0;
        key_timer_->stop();
    }
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::initUi() {
    // Load widget tree from .ui file (generated by uic).
    // After setupUi: remote_video_ is inside callPage→callLayout;
    // chatRow (chat_input_, send_button_), log_area_, peer_tree_ exist.
    ui.setupUi(this);

    // ---- Replace callPage layout with QSplitter structure ----
    // The .ui callLayout is a QVBoxLayout with only remote_video_ in it.
    // Qt cannot set a new layout while the old one is still installed, so:
    //   1. Remove remote_video_ from old layout (widget survives)
    //   2. Delete old layout (now empty and detached from callPage)
    //   3. Build new layout structure
    QLayout* oldCallLayout = ui.callPage->layout();
    oldCallLayout->removeWidget(ui.remote_video_);
    delete oldCallLayout;

    QHBoxLayout* callRootLayout = new QHBoxLayout(ui.callPage);
    callRootLayout->setContentsMargins(0, 0, 0, 0);
    callRootLayout->setSpacing(0);

    splitter_ = new QSplitter(Qt::Horizontal, ui.callPage);
    splitter_->setHandleWidth(4);
    splitter_->setChildrenCollapsible(false);

    // --- remote_container_ ---
    remote_container_ = new QWidget();
    remote_container_->setObjectName("remoteContainer");
    remote_container_->setMinimumWidth(400);

    QVBoxLayout* remoteLayout = new QVBoxLayout(remote_container_);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(0);

    // Reparent remote_video_ into remote_container_
    ui.remote_video_->setParent(remote_container_);
    remoteLayout->addWidget(ui.remote_video_);

    // --- local_video_ (PIP, NOT in layout — absolute positioned) ---
    local_video_ = new GlVideoWidget(remote_container_);
    local_video_->setObjectName("localVideo");
    local_video_->setMinimumSize(160, 120);
    local_video_->setMaximumSize(480, 360);
    local_video_->setStyleSheet(
        "border: 2px solid rgba(255,255,255,0.25); border-radius: 8px;");
    local_video_->hide();
    // Pass mouse events through to remote_video_ (PIP is free-floating)
    local_video_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // --- stats_label_ (bottom overlay) ---
    stats_label_ = new QLabel(remote_container_);
    stats_label_->setObjectName("statsLabel");
    stats_label_->setText("Remote: -- | Local: --");
    stats_label_->setAlignment(Qt::AlignCenter);
    stats_label_->setStyleSheet(
        "color: white; background: rgba(0,0,0,0.55);"
        "border-radius: 4px; padding: 4px 12px;");
    stats_label_->hide();

    // --- right_panel_ ---
    right_panel_ = new QWidget();
    right_panel_->setObjectName("rightPanel");
    right_panel_->setMinimumWidth(200);

    QVBoxLayout* panelLayout = new QVBoxLayout(right_panel_);
    panelLayout->setContentsMargins(8, 8, 8, 8);
    panelLayout->setSpacing(4);

    // chat_display_ — created in C++
    chat_display_ = new QPlainTextEdit(right_panel_);
    chat_display_->setObjectName("chatDisplay");
    chat_display_->setReadOnly(true);
    chat_display_->setPlaceholderText("DataChannel / 语音对话消息...");
    chat_display_->setMinimumHeight(120);

    // chat_input_row_ — reparent chat_input_ and send_button_ from chatRow
    QWidget* chatInputRow = new QWidget(right_panel_);
    QHBoxLayout* chatInputLayout = new QHBoxLayout(chatInputRow);
    chatInputLayout->setContentsMargins(0, 0, 0, 0);
    chatInputLayout->setSpacing(4);

    // Reparent from chatRow (which is still in mainLayout — remove it)
    QWidget* oldChatRow = ui.chat_input_->parentWidget();
    ui.chat_input_->setParent(chatInputRow);
    ui.send_button_->setParent(chatInputRow);
    chatInputLayout->addWidget(ui.chat_input_);
    chatInputLayout->addWidget(ui.send_button_);
    // Remove old chatRow from mainLayout (it's now empty)
    if (oldChatRow) {
        ui.mainLayout->removeWidget(oldChatRow);
        oldChatRow->deleteLater();
    }

    panelLayout->addWidget(chat_display_, 1);
    panelLayout->addWidget(chatInputRow);

    // --- Assemble splitter ---
    splitter_->addWidget(remote_container_);
    splitter_->addWidget(right_panel_);
    splitter_->setStretchFactor(0, 80);
    splitter_->setStretchFactor(1, 20);

    callRootLayout->addWidget(splitter_);

    // ---- Overlay positioning on remote_container_ resize ----
    remote_container_->installEventFilter(this);

    // ---- Danger warning overlay (child of remote_container_) ----
    danger_overlay_ = new QWidget(remote_container_);
    danger_overlay_->setStyleSheet(
        "background: rgba(200, 0, 0, 60); border: 3px solid rgba(255, 40, 40, 180);");
    danger_overlay_->hide();
    danger_label_ = new QLabel(danger_overlay_);
    danger_label_->setAlignment(Qt::AlignCenter);
    danger_label_->setStyleSheet(
        "background: transparent; color: white; font-size: 28px; font-weight: bold;"
        "border: none;");
    danger_timer_ = new QTimer(this);
    connect(danger_timer_, &QTimer::timeout, this, [this]() {
        if (danger_blinks_ <= 0) {
            danger_timer_->stop();
            danger_overlay_->hide();
            return;
        }
        danger_overlay_->setVisible(!danger_overlay_->isVisible());
        if (danger_overlay_->isVisible()) danger_blinks_--;
    });

    // ---- Configure remaining .ui widgets ----
    ui.peer_tree_->setHeaderLabels({"ID", "名称", "状态"});
    ui.peer_tree_->setMaximumHeight(220);
    ui.send_button_->setObjectName("sendButton");
    ui.log_area_->setObjectName("logArea");

    // ---- Toolbar (unchanged) ----
    toolbar_ = addToolBar("Main");
    toolbar_->setMovable(false);

    action_connect_ = toolbar_->addAction("连接");
    action_disconnect_ = toolbar_->addAction("断开");
    action_disconnect_->setEnabled(false);
    toolbar_->addSeparator();

    peer_input_ = new QLineEdit();
    peer_input_->setPlaceholderText("对端ID或名称");
    peer_input_->setMaximumWidth(180);
    peer_input_->setEnabled(false);
    toolbar_->insertWidget(toolbar_->actions().at(3), peer_input_);

    action_call_ = toolbar_->addAction("呼叫");
    action_call_->setEnabled(false);
    action_hangup_ = toolbar_->addAction("挂断");
    action_hangup_->setEnabled(false);
    toolbar_->addSeparator();

    action_ai_ = toolbar_->addAction("启动AI");
    action_ai_->setEnabled(false);
    action_ai_->setToolTip("切换YOLOv5目标检测");

    action_fall_ = toolbar_->addAction("跌倒检测");
    action_fall_->setEnabled(false);
    action_fall_->setToolTip("切换跌倒检测");

    action_fire_ = toolbar_->addAction("火灾检测");
    action_fire_->setEnabled(false);
    action_fire_->setToolTip("切换火灾/烟雾检测");
    toolbar_->addSeparator();

    action_voice_chat_ = toolbar_->addAction("语音对话");
    action_voice_chat_->setEnabled(false);
    action_voice_chat_->setToolTip("切换语音对话");

    statusBar()->showMessage("Starting...");
}

void MainWindow::initConnections() {
    connect(proc_mgr_, &WebRtcProcessManager::stateChanged,
            this, &MainWindow::onProcessStateChanged);
    connect(proc_mgr_, &WebRtcProcessManager::daemonLog,
            this, &MainWindow::log);

    connect(channel_, &ControlChannel::connected,
            this, &MainWindow::onControlConnected);
    connect(channel_, &ControlChannel::disconnected,
            this, &MainWindow::onControlDisconnected);
    connect(channel_, &ControlChannel::peerListReceived,
            this, &MainWindow::onPeerListReceived);

    connect(channel_, &ControlChannel::serverConnected, this, [this]() {
        log("Server connected");
        first_connect_ = false;
        action_connect_->setEnabled(false);
        action_disconnect_->setEnabled(true);
        statusBar()->showMessage("Connected to signaling server");
        QTimer::singleShot(200, this, [this]() {
            voice_chat_startup_complete_ = true;
            updateVoiceChatEnabled();
        });
    });
    connect(channel_, &ControlChannel::serverDisconnected, this, [this]() {
        log("Server disconnected");
        is_call_active_ = false;
        ai_active_type_.clear();
        resetAllAiButtons();
        action_ai_->setEnabled(false);
        action_fall_->setEnabled(false);
        action_fire_->setEnabled(false);
        action_voice_chat_->setEnabled(false);
        stopVoiceChat();
        if (ui.remote_video_) ui.remote_video_->setDetections({});
        action_connect_->setEnabled(true);
        action_disconnect_->setEnabled(false);
        action_call_->setEnabled(false);
        action_hangup_->setEnabled(false);
        statusBar()->showMessage("Disconnected");
    });

    connect(channel_, &ControlChannel::callConnected, this, [this]() {
        log("Call connected, waiting for ICE...");
        is_call_active_ = true;
        action_call_->setEnabled(false);
        peer_input_->setEnabled(false);
        action_hangup_->setEnabled(true);
        action_voice_chat_->setEnabled(false);
        stopVoiceChat();
        statusBar()->showMessage("Call in progress — waiting for ICE...");
    });

    connect(channel_, &ControlChannel::callDisconnected, this, [this]() {
        log("Call disconnected, restarting daemon...");
        is_call_active_ = false;
        ai_active_type_.clear();
        held_key_ = 0;
        key_timer_->stop();
        action_hangup_->setEnabled(false);
        action_ai_->setEnabled(false);
        action_fall_->setEnabled(false);
        action_fire_->setEnabled(false);
        resetAllAiButtons();
        action_voice_chat_->setEnabled(false);
        if (ui.remote_video_) ui.remote_video_->setDetections({});
        stopVideoSources();
        switchToListMode();
        ui.chat_input_->setEnabled(false);
        ui.send_button_->setEnabled(false);

        bool was_first = first_connect_;
        first_connect_ = true;
        channel_->cmdDisconnect();
        QTimer::singleShot(500, this, [this, was_first]() {
            proc_mgr_->stop();
            QTimer::singleShot(1000, this, [this, was_first]() {
                log("Starting new daemon instance...");
                first_connect_ = was_first;
                proc_mgr_->start();
            });
        });
    });

    connect(channel_, &ControlChannel::dataChannelStateChanged, this,
            [this](const QString& state) {
        log("DataChannel: " + state);
        bool open = (state == "open");
        ui.chat_input_->setEnabled(open);
        ui.send_button_->setEnabled(open);
        if (open) {
            chatLog("--- DataChannel open ---");
            if (is_call_active_) {
                action_ai_->setEnabled(true);
                action_fall_->setEnabled(true);
                action_fire_->setEnabled(true);
            }
        }
    });
    connect(channel_, &ControlChannel::dataReceived, this,
            [this](const QString& text) {
        // AI protocol messages (from robot)
        if (text == "AI:ON_OK:yolov5") {
            ai_active_type_ = "yolov5";
            action_ai_->setText("停止AI");
            action_fall_->setText("跌倒检测");
            action_fire_->setText("火灾检测");
            log("Robot YOLOv5 inference started");
            return;
        }
        if (text == "AI:ON_OK:fall") {
            ai_active_type_ = "fall";
            action_ai_->setText("启动AI");
            action_fall_->setText("停止跌倒");
            action_fire_->setText("火灾检测");
            log("Robot fall detection started");
            return;
        }
        if (text == "AI:ON_OK:fire") {
            ai_active_type_ = "fire";
            action_ai_->setText("启动AI");
            action_fall_->setText("跌倒检测");
            action_fire_->setText("停止火灾");
            log("Robot fire detection started");
            return;
        }
        if (text == "AI:OFF_OK") {
            ai_active_type_.clear();
            resetAllAiButtons();
            if (ui.remote_video_) ui.remote_video_->setDetections({});
            log("Robot AI inference stopped");
            return;
        }
        if (text.startsWith("AI:DET:")) {
            QByteArray detJson = text.mid(7).toUtf8();
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(detJson, &err);
            if (err.error == QJsonParseError::NoError) {
                QVector<Detection> dets;
                QJsonArray arr = doc.object()["dets"].toArray();
                for (const auto& v : arr) {
                    QJsonObject d = v.toObject();
                    Detection det;
                    det.cls_id = d["cls"].toInt();
                    det.label = d["label"].toString();
                    det.conf = (float)d["conf"].toDouble();
                    QJsonArray box = d["box"].toArray();
                    if (box.size() >= 4) {
                        det.left = box[0].toInt();
                        det.top = box[1].toInt();
                        det.right = box[2].toInt();
                        det.bottom = box[3].toInt();
                    }
                    dets.append(det);
                }
                checkDangerLabels(dets);
                if (ui.remote_video_) ui.remote_video_->setDetections(dets);
            }
            return;
        }
        chatLog("Peer: " + text);
    });

    connect(channel_, &ControlChannel::iceStateChanged, this, [this](const QString& s) {
        statusBar()->showMessage("ICE: " + s);
        if (s == "connected") {
            log("ICE connected, starting video...");
            switchToCallMode();
            startVideoSources();
        }
    });
    connect(channel_, &ControlChannel::errorOccurred, this, [this](const QString& e) {
        log("Channel error: " + e);
    });

    connect(action_connect_, &QAction::triggered, this, [this]() {
        channel_->cmdConnect(server_addr_, server_port_);
        log(QString("Connecting to %1:%2...").arg(server_addr_).arg(server_port_));
    });
    connect(action_disconnect_, &QAction::triggered, this, [this]() {
        channel_->cmdDisconnect();
    });
    connect(action_call_, &QAction::triggered, this, [this]() {
        if (peers_.isEmpty()) return;
        QString input = peer_input_->text().trimmed();
        if (input.isEmpty()) {
            doCall(peers_.first().id);
        } else {
            bool is_number;
            int id = input.toInt(&is_number);
            if (is_number) {
                doCall(id);
            } else {
                for (const auto& p : peers_) {
                    if (p.name.contains(input, Qt::CaseInsensitive)) {
                        doCall(p.id);
                        return;
                    }
                }
                log(QString("Peer not found: \"%1\"").arg(input));
            }
        }
    });
    connect(action_hangup_, &QAction::triggered, this, [this]() {
        channel_->cmdHangup();
    });
    connect(ui.peer_tree_, &QTreeWidget::itemDoubleClicked,
            this, &MainWindow::onPeerDoubleClicked);
    connect(peer_input_, &QLineEdit::returnPressed, action_call_, &QAction::trigger);
    connect(ui.send_button_, &QPushButton::clicked, this, &MainWindow::doSendMessage);
    connect(ui.chat_input_, &QLineEdit::returnPressed, this, &MainWindow::doSendMessage);

    stats_timer_ = new QTimer(this);
    stats_timer_->setInterval(1000);
    connect(stats_timer_, &QTimer::timeout, this, &MainWindow::onUpdateStats);
}

void MainWindow::onProcessStateChanged(WebRtcProcessManager::State state) {
    switch (state) {
    case WebRtcProcessManager::Running:
        log("Daemon started, connecting socket...");
        updateVoiceChatEnabled();
        QTimer::singleShot(500, this, [this]() {
            channel_->connectToServer();
        });
        break;
    case WebRtcProcessManager::Stopped:
        log("Daemon stopped");
        is_call_active_ = false;
        action_hangup_->setEnabled(false);
        break;
    case WebRtcProcessManager::Error:
        log("Daemon error - check binary path");
        is_call_active_ = false;
        action_hangup_->setEnabled(false);
        statusBar()->showMessage("Daemon failed to start");
        break;
    default:
        break;
    }
}

void MainWindow::onControlConnected() {
    log("Control channel connected");
    if (first_connect_) {
        log("Auto-connecting to signaling server...");
        channel_->cmdConnect(server_addr_, server_port_);
    }
}

void MainWindow::onControlDisconnected() {
    log("Control channel disconnected");
    is_call_active_ = false;
    action_hangup_->setEnabled(false);
}

void MainWindow::onPeerListReceived(QVector<PeerInfo> peers) {
    peers_ = peers;
    ui.peer_tree_->clear();
    for (const auto& p : peers) {
        auto* item = new QTreeWidgetItem(ui.peer_tree_);
        item->setText(0, QString::number(p.id));
        item->setText(1, p.name);
        item->setText(2, "Online");
        item->setData(0, Qt::UserRole, p.id);
    }
    bool has_peer = !peers.isEmpty();
    action_call_->setEnabled(has_peer);
    peer_input_->setEnabled(has_peer);
    action_hangup_->setEnabled(is_call_active_);
    updateVoiceChatEnabled();
    if (has_peer) {
        const auto& first = peers.first();
        peer_input_->setPlaceholderText(
            QString("ID %1 \"%2\"").arg(first.id).arg(first.name));
    } else {
        peer_input_->setPlaceholderText("No peers online");
    }
    statusBar()->showMessage(QString("%1 peer(s) online").arg(peers.size()));
}

void MainWindow::onPeerDoubleClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    int peer_id = item->data(0, Qt::UserRole).toInt();
    doCall(peer_id);
}

void MainWindow::doCall(int peer_id) {
    stopVoiceChat();
    channel_->cmdCall(peer_id);
    log(QString("Calling peer %1...").arg(peer_id));
}

void MainWindow::switchToListMode() {
    ui.stack_->setCurrentIndex(0);
    centralWidget()->layout()->setContentsMargins(4, 4, 4, 4);
    ui.log_area_->setVisible(true);
    if (local_video_)
        local_video_->hide();
    stats_timer_->stop();
}

void MainWindow::switchToCallMode() {
    ui.stack_->setCurrentIndex(1);
    centralWidget()->layout()->setContentsMargins(0, 0, 0, 0);
    ui.log_area_->setVisible(false);
    stats_label_->show();
    stats_label_->raise();
    stats_timer_->start();
    // Force initial 80:20 splitter ratio after layout recalculation.
    // setStretchFactor alone only governs resize redistribution; setSizes
    // pins the starting allocation.
    QTimer::singleShot(0, this, [this]() {
        int totalW = splitter_->width();
        if (totalW > 0) {
            splitter_->setSizes({totalW * 80 / 100, totalW * 20 / 100});
        }
    });
}

void MainWindow::startVideoSources() {
    QString rdir = qEnvironmentVariable("WEBRTC_RUNTIME_DIR",
                                        "/tmp/webrtc_runtime");

    local_source_ = new ShmVideoSource(rdir + "/shm_video_buf_local", 0x89, this);
    remote_source_ = new ShmVideoSource(rdir + "/shm_video_buf_remote", 0x8a, this);

    connect(local_source_, &ShmVideoSource::frameReady, this,
            [this](const QImage& f) {
        local_width_ = f.width();
        local_height_ = f.height();
        local_frame_count_++;
        if (local_video_) {
            local_video_->setFrame(f);
            if (!local_video_->isVisible()) {
                local_video_->show();
                local_video_->raise();
                stats_label_->show();
                stats_label_->raise();
            }
        }
    });
    connect(remote_source_, &ShmVideoSource::frameReady, this,
            [this](const QImage& f) {
        remote_width_ = f.width();
        remote_height_ = f.height();
        remote_frame_count_++;
        ui.remote_video_->setFrame(f);
    });

    connect(local_source_, &ShmVideoSource::errorOccurred, this,
            [this](const QString& e) { log("Local video error: " + e); });
    connect(remote_source_, &ShmVideoSource::errorOccurred, this,
            [this](const QString& e) { log("Remote video error: " + e); });

    local_source_->start();
    remote_source_->start();
    log("Video sources started (SHM + libyuv)");
}

void MainWindow::stopVideoSources() {
    if (local_source_) {
        local_source_->stop();
        local_source_->deleteLater();
        local_source_ = nullptr;
    }
    if (remote_source_) {
        remote_source_->stop();
        remote_source_->deleteLater();
        remote_source_ = nullptr;
    }
}

void MainWindow::doSendMessage() {
    QString text = ui.chat_input_->text().trimmed();
    if (text.isEmpty()) return;
    channel_->cmdSendData(text);
    chatLog("Me: " + text);
    ui.chat_input_->clear();
    ui.chat_input_->setFocus();
}

void MainWindow::onUpdateStats() {
    QString stats = QString("Remote: %1x%2 | Local: %3x%4")
                        .arg(remote_width_).arg(remote_height_)
                        .arg(local_width_).arg(local_height_);
    stats_label_->setText(stats);
}

void MainWindow::log(const QString& msg) {
    ui.log_area_->appendPlainText(msg);
    if (log_file_ && log_file_->isOpen()) {
        QTextStream ts(log_file_);
        ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << " " << msg << "\n";
        ts.flush();
    }
}

void MainWindow::chatLog(const QString& msg) {
    chat_display_->appendPlainText(msg);
}

void MainWindow::toggleVoiceChat()
{
    if (!voice_chat_startup_complete_) return;
    if (voice_chat_running_) {
        stopVoiceChat();
    } else {
        QString runtime_dir = qEnvironmentVariable("WEBRTC_RUNTIME_DIR",
                                                    "/tmp/webrtc_runtime");
        QString vc_socket = runtime_dir + "/voice_chat.sock";
        voice_mgr_->start(vc_socket);
        voice_chat_running_ = true;
        action_voice_chat_->setText("停止语音");
        QTimer::singleShot(500, this, [this]() {
            voice_client_->connectToServer();
        });
        log("Voice chat started");
    }
}

void MainWindow::stopVoiceChat()
{
    if (!voice_chat_running_) return;
    voice_client_->cmdStop();
    voice_client_->disconnectFromServer();
    voice_mgr_->stop();
    voice_chat_running_ = false;
    action_voice_chat_->setText("语音对话");
    action_voice_chat_->setEnabled(false);
    updateVoiceChatEnabled();
    log("Voice chat stopped");
}

void MainWindow::toggleAi(const QString& type)
{
    if (!is_call_active_) return;
    if (ai_active_type_ == type) {
        // Currently active for this type — stop it
        channel_->cmdSendData("AI:OFF");
        ai_active_type_.clear();
        resetAllAiButtons();
        if (ui.remote_video_) ui.remote_video_->setDetections({});
        log(QString("Sent AI:OFF to robot (was %1)").arg(type));
    } else {
        // Stop current if any, then start new type
        if (!ai_active_type_.isEmpty()) {
            channel_->cmdSendData("AI:OFF");
        }
        channel_->cmdSendData("AI:ON:" + type);
        ai_active_type_ = type;
        // Update button states optimistically
        if (type == "yolov5") {
            action_ai_->setText("停止AI");
            action_fall_->setText("跌倒检测");
            action_fire_->setText("火灾检测");
        } else if (type == "fall") {
            action_ai_->setText("启动AI");
            action_fall_->setText("停止跌倒");
            action_fire_->setText("火灾检测");
        } else if (type == "fire") {
            action_ai_->setText("启动AI");
            action_fall_->setText("跌倒检测");
            action_fire_->setText("停止火灾");
        }
        log(QString("Sent AI:ON:%1 to robot").arg(type));
    }
}

void MainWindow::resetAllAiButtons()
{
    action_ai_->setText("启动AI");
    action_fall_->setText("跌倒检测");
    action_fire_->setText("火灾检测");
}

void MainWindow::checkDangerLabels(const QVector<Detection>& detections) {
    for (const auto& d : detections) {
        if (d.conf < 0.45f) continue;
        QString lbl = d.label.toLower();
        if (lbl == "fire") {
            triggerDangerWarning("⚠ 检测到火灾！");
            return;
        } else if (lbl == "smoke") {
            triggerDangerWarning("⚠ 检测到烟雾！");
            return;
        } else if (lbl == "fallen") {
            triggerDangerWarning("⚠ 检测到跌倒！");
            return;
        }
    }
}

void MainWindow::triggerDangerWarning(const QString& text) {
    danger_label_->setText(text);
    if (remote_container_) {
        int cw = remote_container_->width();
        int ch = remote_container_->height();
        danger_overlay_->setGeometry(0, 0, cw, ch);
        danger_label_->setGeometry(0, 0, cw, ch);
    }
    danger_blinks_ = 6;  // 3 visible blinks (6 half-cycles)
    danger_overlay_->show();
    danger_overlay_->raise();  // ensure above PIP and stats
    danger_timer_->start(400);  // 400ms toggle interval
}

void MainWindow::updateVoiceChatEnabled()
{
    bool ok = proc_mgr_->state() == WebRtcProcessManager::Running
           && !is_call_active_;
    action_voice_chat_->setEnabled(ok);
}
