#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <vector>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/data_channel_interface.h"
#include "api/enable_media.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "apps/peerconnection/client/defaults.h"
#include "apps/peerconnection/ipc_test26_10_26/unix_socket_server.h"
#include "apps/peerconnection/learning/learning_common.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

using namespace webrtc;


class PeerConnectionObserverImpl;
class DcDataObserver;

// 状态机函数前向声明（IpcObserver 回调里调用，定义在后面）
void StartOffer();
void CreateAnswer();
void AddRemoteCandidate(const std::string& line);
void MaybeSendLocalCandidates();
void HandleLine(const std::string& line);
void Hangup();
void Teardown();

scoped_refptr<PeerConnectionFactoryInterface> g_factory;
scoped_refptr<PeerConnectionInterface> g_pc;
scoped_refptr<DataChannelInterface> g_dc;
// 声明顺序决定析构顺序（逆序析构）：
// g_pss 必须先于 g_thread 声明，确保 g_thread 析构时 g_pss 还活着
std::unique_ptr<PhysicalSocketServer> g_pss;
std::unique_ptr<Thread> g_thread;
std::string g_local_sdp;
int g_candidate_count = 0;

struct CandidateInfo {
    std::string mid;
    int mline_index = 0;
    std::string candidate_str;
};
std::vector<CandidateInfo> g_local_candidates;

// ===== IPC（事件驱动，来自 ipc_test26_10_26）=====
Role g_role = Role::kOfferer;
std::unique_ptr<UnixSocketServer> g_ipc_server;
int g_client_id = -1;
enum class SignalPhase { kAwaitSdp, kAwaitCandidates };
SignalPhase g_phase = SignalPhase::kAwaitSdp;
std::vector<std::string> g_peer_sdp_lines;  // 累积对端 SDP（offer 或 answer）
bool g_candidates_sent = false;
bool g_shutting_down = false;

class IpcObserver : public UnixSocketObserver {
public:
    void OnClientConnected(int client_id) override {
        // 回调在 io 线程，转投到 WebRTC 线程推进状态机
        g_thread->PostTask([client_id] {
            if (g_client_id != -1) {
                RTC_LOG(LS_WARNING) << "Second client ignored (1:1 demo)";
                return;
            }
            g_client_id = client_id;
            g_phase = SignalPhase::kAwaitSdp;
            if (g_role == Role::kOfferer) {
                StartOffer();
            }
            // answerer 不主动做什么，等对端 offer 到达
        });
    }
    void OnLineReceived(int client_id, const std::string& line) override {
        g_thread->PostTask([line] { HandleLine(line); });
    }
    void OnClientDisconnected(int client_id) override {
        g_thread->PostTask([client_id] {
            if (client_id == g_client_id) {
                RTC_LOG(LS_INFO) << "Client disconnected";
                Teardown();
            }
        });
    }
};
IpcObserver g_ipc_observer;

class DcdataObserver : public DataChannelObserver{
public:
    void OnStateChange() override {
        auto state_enum = g_dc->state();
        auto state_str = DataChannelInterface::DataStateString(state_enum);
        RTC_LOG(LS_INFO) << "DataChannel state: " << state_str;
        if (state_enum == DataChannelInterface::kOpen) {
            g_ipc_server->SendToClient(g_client_id, "DataChannel OPEN — ready to chat!");
        } else if (state_enum == DataChannelInterface::kClosed ||
                   state_enum == DataChannelInterface::kClosing) {
            g_ipc_server->SendToClient(g_client_id, "Peer disconnected.");
            Teardown();
        }
    }
    void OnMessage(const DataBuffer& buffer) override {
        std::string msg(buffer.data.data<char>(), buffer.data.size());
        RTC_LOG(LS_INFO) << "DataChannel onMessage: " << msg;
        if (msg == "/bye") {
            g_ipc_server->SendToClient(g_client_id, "Peer hung up.");
            Teardown();
            return;
        }
        g_ipc_server->SendToClient(g_client_id, "Peer: " + msg);
    }
    bool IsOkToCallOnTheNetworkThread() override { return true; }
};
DcdataObserver g_data_observer;

class PeerConnectionObserverImpl : public PeerConnectionObserver{
public:
    // Triggered when the SignalingState changed.
    void OnSignalingChange(
      PeerConnectionInterface::SignalingState new_state) override {
        RTC_LOG(LS_INFO) << "SignalingState: " << PeerConnectionInterface::AsString(new_state);
    }
    // Triggered when a remote peer opens a data channel.
    void OnDataChannel(
      scoped_refptr<DataChannelInterface> data_channel) override {
        g_dc = data_channel;
        g_dc->RegisterObserver(&g_data_observer);
    }
    // Called any time the IceGatheringState changes.
    void OnIceGatheringChange(
      PeerConnectionInterface::IceGatheringState new_state) override {
        RTC_LOG(LS_INFO) << "IceGatheringState: " << PeerConnectionInterface::AsString(new_state);
        if (new_state == PeerConnectionInterface::kIceGatheringComplete) {
            MaybeSendLocalCandidates();
        }
    }
    // A new ICE candidate has been gathered.
    void OnIceCandidate(const IceCandidate* candidate) override {
        g_candidate_count++;
        RTC_LOG(LS_INFO) << "ICE candidate #" << g_candidate_count
                         << ": " << candidate->candidate().ToString();
        CandidateInfo info;
        info.mid = candidate->sdp_mid();
        info.mline_index = candidate->sdp_mline_index();
        candidate->ToString(&info.candidate_str);
        g_local_candidates.push_back(std::move(info));
        RTC_LOG(LS_INFO) << "Local ICE candidate [" << info.mline_index << "|"
                        << info.mid << "]: " << info.candidate_str;

    }

    void OnConnectionChange(
        PeerConnectionInterface::PeerConnectionState state) override {
        RTC_LOG(LS_INFO) << "PeerConnectionState: "
                            << PeerConnectionInterface::AsString(state);
        if (state == PeerConnectionInterface::PeerConnectionState::kConnected) {
            RTC_LOG(LS_INFO) << ">>> CONNECTION ESTABLISHED <<<";
            g_ipc_server->SendToClient(g_client_id, ">>> CONNECTION ESTABLISHED <<<");
        } else if (state == PeerConnectionInterface::PeerConnectionState::kFailed) {
            RTC_LOG(LS_ERROR) << "Connection FAILED";
        } else if (state ==
                    PeerConnectionInterface::PeerConnectionState::kDisconnected) {
            RTC_LOG(LS_INFO) << "Disconnected";
        }
    }

};
PeerConnectionObserverImpl g_pc_observer;

class DcSetLocalObserver : public SetLocalDescriptionObserverInterface {
public:
    void OnSetLocalDescriptionComplete(RTCError error) {
        if(error.ok())
        {
            RTC_LOG(LS_INFO) << "SetLocalDesc OK";
        }
        else{
            RTC_LOG(LS_INFO) << "SetLocalDesc failed";
        }
    }

};



class DcCreateSdpObserver : public CreateSessionDescriptionObserver {
public:
    void OnSuccess(SessionDescriptionInterface* desc) {
        RTC_LOG(LS_INFO) << "Local SDP created (type= " << desc->GetType() << " )";
        desc->ToString(&g_local_sdp);
        g_pc->SetLocalDescription(
            std::unique_ptr<SessionDescriptionInterface>(desc),
            make_ref_counted<DcSetLocalObserver>()
        );
        // 把本端 SDP（offer 或 answer）推给对端；空行作为结束符
        g_ipc_server->SendToClient(g_client_id, g_local_sdp);
        g_ipc_server->SendToClient(g_client_id, "");
        if (g_role == Role::kAnswerer) {
            // 答方：answer 已发出，接下来收对端候选
            g_phase = SignalPhase::kAwaitCandidates;
            MaybeSendLocalCandidates();
        }
        // 发起方：offer 已发出，继续等 answer（phase 保持 kAwaitSdp）
    }
    void OnFailure(RTCError error) {
        RTC_LOG(LS_INFO) << "Local SDP create failed";
    }
};
// CreateOffer/CreateAnswer 收的是裸指针，必须自己持有引用直到回调结束
scoped_refptr<DcCreateSdpObserver> g_sdp_observer;

class DcSetRemoteObserver : public SetRemoteDescriptionObserverInterface {
public:
    void OnSetRemoteDescriptionComplete(RTCError error) {
        if(error.ok()){
            RTC_LOG(LS_INFO) << "SetRemoteDescription OK";
            if (g_role == Role::kAnswerer) {
                // 答方：设完远端 offer 后，紧接着生成 answer
                CreateAnswer();
            }
        } else {
            RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: " << error.message();
        }
    }
};

// ===== 事件驱动状态机 =====
void StartOffer() {
    DataChannelInit dc_init;
    dc_init.negotiated = false;
    dc_init.id = 0;
    auto dc_or_error = g_pc->CreateDataChannelOrError("chat", &dc_init);
    if (!dc_or_error.ok()) {
        RTC_LOG(LS_ERROR) << "Failed to create DataChannel: "
                          << dc_or_error.error().message();
        Teardown();
        return;
    }
    g_dc = std::move(dc_or_error.value());
    g_dc->RegisterObserver(&g_data_observer);
    PeerConnectionInterface::RTCOfferAnswerOptions options;
    g_pc->CreateOffer(g_sdp_observer.get(), options);
}

void CreateAnswer() {
    PeerConnectionInterface::RTCOfferAnswerOptions options;
    g_pc->CreateAnswer(g_sdp_observer.get(), options);
}

void AddRemoteCandidate(const std::string& line) {
    auto pipe1 = line.find('|');
    auto pipe2 = line.find('|', pipe1 + 1);
    if (pipe1 == std::string::npos || pipe2 == std::string::npos) {
        RTC_LOG(LS_WARNING) << "Bad candidate format: " << line;
        return;
    }
    int mline = std::stoi(line.substr(0, pipe1));
    std::string mid = line.substr(pipe1 + 1, pipe2 - pipe1 - 1);
    std::string cand = line.substr(pipe2 + 1);

    SdpParseError err;
    IceCandidateInterface* candidate =
        CreateIceCandidate(mid, mline, cand, &err);
    if (!candidate) {
        RTC_LOG(LS_WARNING) << "Failed to parse candidate: " << err.description;
        return;
    }
    g_pc->AddIceCandidate(std::unique_ptr<IceCandidateInterface>(candidate),
                          [](RTCError error) {
                            if (!error.ok()) {
                              RTC_LOG(LS_WARNING)
                                  << "AddIceCandidate failed: "
                                  << error.message();
                            }
                          });
}

void MaybeSendLocalCandidates() {
    if (g_candidates_sent) return;
    if (g_phase != SignalPhase::kAwaitCandidates) return;
    if (g_pc->ice_gathering_state() != PeerConnectionInterface::kIceGatheringComplete) return;
    for (const auto& c : g_local_candidates) {
        std::string line = std::to_string(c.mline_index) + "|" + c.mid + "|" +
                           c.candidate_str;
        g_ipc_server->SendToClient(g_client_id, line);
    }
    g_ipc_server->SendToClient(g_client_id, "done");
    g_candidates_sent = true;
    RTC_LOG(LS_INFO) << "Sent " << g_local_candidates.size()
                     << " local candidates";
}

void HandleLine(const std::string& line) {
    // 聊天中：DC 已 open，直接当消息
    if (g_dc && g_dc->state() == DataChannelInterface::kOpen) {
        if (line == "/quit") {
            Hangup();
            return;
        }
        g_dc->Send(DataBuffer(line));
        return;
    }

    switch (g_phase) {
        case SignalPhase::kAwaitSdp:
            if (line.empty()) {  // 空行 = 对端 SDP 结束
                std::string remote_sdp;
                for (const auto& l : g_peer_sdp_lines) remote_sdp += l + "\n";
                g_peer_sdp_lines.clear();
                SdpType type = (g_role == Role::kOfferer)
                                   ? SdpType::kAnswer
                                   : SdpType::kOffer;
                std::unique_ptr<SessionDescriptionInterface> sdp =
                    CreateSessionDescription(type, remote_sdp);
                if (sdp) {
                    g_pc->SetRemoteDescription(
                        std::move(sdp),
                        make_ref_counted<DcSetRemoteObserver>());
                } else {
                    RTC_LOG(LS_ERROR) << "Failed to parse remote SDP";
                }
                if (g_role == Role::kOfferer) {
                    // 发起方：设完 answer 后开始收对端候选
                    g_phase = SignalPhase::kAwaitCandidates;
                    MaybeSendLocalCandidates();
                }
                // 答方：等 DcSetRemoteObserver 回调里 CreateAnswer，
                // 再在 OnSuccess 里切到 kAwaitCandidates
            } else {
                g_peer_sdp_lines.push_back(line);
            }
            break;
        case SignalPhase::kAwaitCandidates:
            if (line == "done" || line == "DONE") {
                // 候选收完，等 DC open（由 OnStateChange 驱动进入聊天）
            } else {
                AddRemoteCandidate(line);
            }
            break;
    }
}

void Hangup() {
    if (g_dc && g_dc->state() == DataChannelInterface::kOpen) {
        g_dc->Send(DataBuffer("/bye"));
    }
    Teardown();
}

void Teardown() {
    if (g_shutting_down) return;
    g_shutting_down = true;
    // 先 Close 再 Quit，防止 BlockingCall 在 quitting 线程上执行
    if (g_dc) { g_dc->Close(); g_dc = nullptr; }
    if (g_pc) { g_pc->Close(); g_pc = nullptr; }
    g_factory = nullptr;
    g_thread->Quit();
}



// ===== 文件日志（继承 webrtc::LogSink）=====
class FileLogSink : public LogSink {
 public:
  explicit FileLogSink(const std::string& path) {
    file_.open(path, std::ios::out | std::ios::app);
  }

  // 主入口：拿到完整 LogLineRef，按默认格式写文件
  void OnLogMessage(const LogLineRef& line) override {
    if (!file_.is_open()) return;
    std::lock_guard<std::mutex> lock(mu_);
    file_ << line.DefaultLogLine();  // DefaultLogLine() 已含换行
    file_.flush();
  }

  // 纯虚必须实现（正常流程由 LogLineRef 主入口处理）
  void OnLogMessage(const std::string& message) override {
    if (!file_.is_open()) return;
    std::lock_guard<std::mutex> lock(mu_);
    file_ << message;  // WebRTC 消息管线已含换行
    file_.flush();
  }

 private:
  std::ofstream file_;
  std::mutex mu_;
};

FileLogSink* SetupFileLogging(Role role, const std::string& log_dir) {
  mkdir(log_dir.c_str(), 0755);

  const char* base = (role == Role::kOfferer) ? "learning_offer"
                                              : "learning_answer";
  // 用「年月日_时分秒」命名，直观且避免每次运行覆盖
  std::time_t now = std::time(nullptr);
  std::tm* tm_now = std::localtime(&now);
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm_now);
  std::string filename = std::string(base) + "_" + ts + ".log";

  auto* sink = new FileLogSink(log_dir + "/" + filename);
  LogMessage::AddLogToStream(sink, LS_INFO);
  return sink;
}



int RunSession(Role role, const std::string& socket_path)
{
    g_role = role;

    // 日志目录 = socket 所在目录
    std::string log_dir = socket_path.substr(0, socket_path.find_last_of('/'));
    if (log_dir.empty()) log_dir = ".";

    // 文件日志：AddLogToStream 后，RTC_LOG 同时打到 stderr + 文件
    FileLogSink* file_sink = SetupFileLogging(role, log_dir);

    RTC_LOG(LS_INFO) << "WebRTC learning started as "
                     << (role == Role::kOfferer ? "OFFERER" : "ANSWERER");

    g_pss = std::make_unique<PhysicalSocketServer>();
    g_thread = std::make_unique<Thread>(g_pss.get());
    ThreadManager::Instance()->SetCurrentThread(g_thread.get());
    InitializeSSL();

    Environment env = CreateEnvironment(std::make_unique<FieldTrials>(""));
    PeerConnectionFactoryDependencies deps;
    deps.signaling_thread = g_thread.get();
    deps.network_thread = g_thread.get();
    deps.worker_thread = g_thread.get();
    deps.env = env;
    deps.adm = nullptr;
    g_factory = CreateModularPeerConnectionFactory(std::move(deps));
    if(!g_factory){
        RTC_LOG(LS_INFO) << "Failed to create PeerConnectionFactory";
        exit(1);
    }

    PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = SdpSemantics::kUnifiedPlan;

    // STUN 服务器 —— 不需要认证，纯做 NAT 穿透
    PeerConnectionInterface::IceServer stun;
    stun.uri = "stun:stun.l.google.com:19302";
    config.servers.push_back(stun);

    // TURN 服务器 —— 需要认证，用于中继数据
    PeerConnectionInterface::IceServer turn;
    turn.urls.push_back("turn:120.79.210.6:3478");   // UDP
    // turn.urls.push_back("turn:your-turn-server.com:3478?transport=tcp");  // TCP
    // turn.urls.push_back("turns:your-turn-server.com:5349");  // TLS (加密)
    turn.username = "test";
    turn.password = "123";
    config.servers.push_back(turn);

    PeerConnectionDependencies pc_deps(&g_pc_observer);
    auto err_or =
        g_factory->CreatePeerConnectionOrError(config, std::move(pc_deps));
    if(!err_or.ok()){
        RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: "
                            << err_or.error().message();
    }
    else{
        RTC_LOG(LS_INFO) << "CreatePeerConnection init success!";
    }
    g_pc = std::move(err_or.value());

    // 观察者生命周期兜底：CreateOffer/CreateAnswer 收裸指针，需自己持有引用
    g_sdp_observer = make_ref_counted<DcCreateSdpObserver>();

    // IPC：用命令行参数决定 socket 路径，再注册观察者 + 非阻塞启动
    g_ipc_server = std::make_unique<UnixSocketServer>(socket_path);
    g_ipc_server->AddObserver(&g_ipc_observer);
    g_ipc_server->Start();
    RTC_LOG(LS_INFO) << "Daemon listening on " << socket_path
                     << ", waiting for client...";

    // 主线程跑 WebRTC 消息循环（阻塞），直到 Teardown() 里 Quit
    g_thread->Run();

    // 收尾：停 io 线程（join，最多 ~100ms epoll 超时）
    g_ipc_server->Stop();

    // 兜底清理（异常路径，如从未连上客户端）
    if (g_dc) {
        g_dc->Close();
        g_dc = nullptr;
    }
    if (g_pc) {
        g_pc->Close();
        g_pc = nullptr;
    }
    g_factory = nullptr;

    LogMessage::RemoveLogToStream(file_sink);
    delete file_sink;

    ThreadManager::Instance()->SetCurrentThread(nullptr);
    CleanupSSL();

    return 0;
}
