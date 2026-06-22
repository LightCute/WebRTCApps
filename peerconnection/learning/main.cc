#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
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
#include "apps/peerconnection/dc_call/unix_socket_server.h"
#include "rtc_base/log_sinks.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

using namespace webrtc;


class PeerConnectionObserverImpl;
class DcDataObserver;

std::unique_ptr<UnixSocketServer> g_socket;
scoped_refptr<PeerConnectionFactoryInterface> g_factory;
scoped_refptr<PeerConnectionInterface> g_pc;
scoped_refptr<DataChannelInterface> g_dc;
// 声明顺序决定析构顺序（逆序析构）：
// g_pss 必须先于 g_thread 声明，确保 g_thread 析构时 g_pss 还活着
std::unique_ptr<PhysicalSocketServer> g_pss;
std::unique_ptr<Thread> g_thread;
std::string g_local_sdp;
bool g_ice_complete = false;
int g_candidate_count = 0;

struct CandidateInfo {
    std::string mid;
    int mline_index = 0;
    std::string candidate_str;
};
std::vector<CandidateInfo> g_local_candidates;


// ===== I/O helpers (switch between standalone and daemon mode) =====
void IoPrint(const std::string& s) {
    g_socket->Write(s);
}

void IoPrintLn(const std::string& s) {
    g_socket->WriteLine(s);
}

// Blocking read. Returns empty on EOF/disconnect.
std::string IoReadLine() {
    return g_socket->ReadLine();
}

bool IoConnected() {
  return g_socket && g_socket->IsConnected();
}

std::atomic<bool> g_chat_running{false};

class DcdataObserver : public DataChannelObserver{
public:
    void OnStateChange() override {
        auto state_enum = g_dc->state();
        auto state_str = DataChannelInterface::DataStateString(state_enum);
        RTC_LOG(LS_INFO) << "DataChannel state: " << state_str;
        if (state_enum == DataChannelInterface::kClosed ||
            state_enum == DataChannelInterface::kClosing) {
            if (g_chat_running) {
                IoPrintLn("\nPeer disconnected.");
                g_chat_running = false;
                // 先 Close 再 Quit，防止 BlockingCall 在 quitting 线程上执行
                if (g_pc) { g_pc->Close(); g_pc = nullptr; }
                g_factory = nullptr;
                g_thread->Quit();
            }
        }
    }
    void OnMessage(const DataBuffer& buffer) override {
        std::string msg(buffer.data.data<char>(), buffer.data.size());
        RTC_LOG(LS_INFO) << "DataChannel onMessage: " << msg;
        if (msg == "/bye") {
            IoPrintLn("\nPeer hung up.");
            g_chat_running = false;
            if (g_dc) { g_dc->Close(); g_dc = nullptr; }
            if (g_pc) { g_pc->Close(); g_pc = nullptr; }
            g_factory = nullptr;
            g_thread->Quit();
            return;
        }
        IoPrint("\nPeer: " + msg + "\nYou: ");
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
            g_ice_complete = true;
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
            IoPrintLn(">>> CONNECTION ESTABLISHED <<<");
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
    }
    void OnFailure(RTCError error) {
        RTC_LOG(LS_INFO) << "Local SDP create failed";
    }
};

class DcSetRemoteObserver : public SetRemoteDescriptionObserverInterface {
public:
    void OnSetRemoteDescriptionComplete(RTCError error) {
        if(error.ok()){
            RTC_LOG(LS_INFO) << "SetRemoteDescription OK";

        } else {
            RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: " << error.message();
        }
    }
};

// ===== ICE exchange =====
void PrintLocalCandidates() {
  IoPrintLn("\nLocal ICE candidates (copy to peer):");
  IoPrintLn("----------------------------------------");
  if (g_local_candidates.empty()) {
    IoPrintLn("(none)");
  } else {
    for (const auto& c : g_local_candidates) {
      std::string line = std::to_string(c.mline_index) + "|" + c.mid + "|" +
                         c.candidate_str;
      IoPrintLn(line);
    }
  }
  IoPrintLn("----------------------------------------");
}


void ReadRemoteCandidates() {
  IoPrintLn(
      "\nPaste remote ICE candidates (one per line, 'done' to finish):");
  std::string line;
  while (IoConnected()) {
    line = IoReadLine();
    if (line.empty()) continue;
    if (line == "done" || line == "DONE") break;

    auto pipe1 = line.find('|');
    auto pipe2 = line.find('|', pipe1 + 1);
    if (pipe1 == std::string::npos || pipe2 == std::string::npos) {
      RTC_LOG(LS_WARNING) << "Bad candidate format: " << line;
      IoPrintLn("WARNING: Expected format mline|mid|candidate");
      continue;
    }
    int mline = std::stoi(line.substr(0, pipe1));
    std::string mid = line.substr(pipe1 + 1, pipe2 - pipe1 - 1);
    std::string cand = line.substr(pipe2 + 1);

    SdpParseError err;
    IceCandidateInterface* candidate =
        CreateIceCandidate(mid, mline, cand, &err);
    if (!candidate) {
      RTC_LOG(LS_WARNING) << "Failed to parse candidate: " << err.description;
      IoPrintLn("WARNING: Failed to parse candidate");
      continue;
    }
    g_pc->AddIceCandidate(std::unique_ptr<IceCandidateInterface>(candidate),
                          [](RTCError error) {
                            if (!error.ok()) {
                              RTC_LOG(LS_WARNING)
                                  << "AddIceCandidate failed: "
                                  << error.message();
                            }
                          });
    g_thread->ProcessMessages(10);
    // Echo back so the user knows the candidate was accepted
    IoPrintLn("  OK");
  }
  RTC_LOG(LS_INFO) << "ICE exchange complete";
  IoPrintLn("ICE exchange complete.");
}

void DoOfferFlow(){
    DataChannelInit dc_init;
    dc_init.negotiated = false;
    dc_init.id = 0;
    auto dc_or_error = g_pc->CreateDataChannelOrError("chat", &dc_init);
    if(!dc_or_error.ok()){
        RTC_LOG(LS_ERROR) << "Failed to create Datachannel: "
                            << dc_or_error.error().message();
        exit(1); 
    }else {
        RTC_LOG(LS_INFO) << "DataChannel init success!";
    }
    g_dc = std::move(dc_or_error.value());
    g_dc->RegisterObserver(&g_data_observer);
    auto obs = make_ref_counted<DcCreateSdpObserver>();
    PeerConnectionInterface::RTCOfferAnswerOptions options;
    g_pc->CreateOffer(obs.get(), options);

    // === 阶段 1: 等待 SDP 生成 + SetLocalDescription ===
    int wait_count = 0;
    while (g_local_sdp.empty() && wait_count < 150) {  // 最多等 30 秒
        g_thread->ProcessMessages(200);  // 每次处理 200ms 的消息
        wait_count++;
    }

    IoPrintLn("\n========================================");
    IoPrintLn("=== WebRTC DataChannel Chat (OFFER) ===");
    IoPrintLn("========================================");
    IoPrintLn("\nLocal OFFER SDP (copy to answerer):");
    IoPrintLn("----------------------------------------");
    IoPrint(g_local_sdp);
    IoPrintLn("----------------------------------------");

    IoPrintLn("\nPaste remote ANSWER SDP (end with empty line):");
    // === 阶段 2: 等待 ICE 采集完成（STUN DNS解析 + STUN请求/响应） ===
    RTC_LOG(LS_INFO) << "Waiting for ICE gathering to complete...";
    int ice_wait = 0;
    while (!g_ice_complete && ice_wait < 150) {  // 最多再等 30 秒
        g_thread->ProcessMessages(200);
        ice_wait++;
    }
    if (g_ice_complete) {
        RTC_LOG(LS_INFO) << "ICE gathering complete! Total candidates: " << g_candidate_count;
        RTC_LOG(LS_INFO) <<  "SDP: " << g_local_sdp;
    } else {
        RTC_LOG(LS_WARNING) << "ICE gathering timed out. Candidates so far: " << g_candidate_count;
    }


    if(g_ice_complete)
    {

        std::string remote_sdp;
        std::string line;
        while (IoConnected() && 
            !(line = IoReadLine()).empty() && 
            !line.empty())
        {
            remote_sdp += line + "\n";
        }
        std::unique_ptr<SessionDescriptionInterface> answer =
            CreateSessionDescription(SdpType::kAnswer, remote_sdp);
        if(!answer){
            RTC_LOG(LS_ERROR) << "Failed to parse remote anser SDP: " ;
        }
        g_pc->SetRemoteDescription(std::move(answer), 
                                    make_ref_counted<DcSetRemoteObserver>());
        g_thread->ProcessMessages(2000);
        PrintLocalCandidates();
        ReadRemoteCandidates();
        RTC_LOG(LS_INFO) << "Waiting for connection...";
        int conn_wait = 0;
        bool dc_open = false;
        while (!dc_open && conn_wait < 150) {
            g_thread->ProcessMessages(200);
            if (g_dc && g_dc->state() == DataChannelInterface::kOpen) {
                dc_open = true;
                RTC_LOG(LS_INFO) << "DataChannel OPEN!";
            }
            conn_wait++;
        }

        // === 阶段 3: 聊天循环 ===
        // 必须用独立线程读输入，因为 g_thread 需要跑 ProcessMessages
        // 来处理对端发来的 DataChannel 消息。IoReadLine() 会阻塞，
        // 如果也在 g_thread 上跑，收到的消息永远无法投递。
        if (dc_open) {
            g_chat_running = true;
            IoPrintLn("\nDataChannel OPEN — ready to chat!");
            IoPrintLn("Type your message and press Enter. '/quit' to exit.");
            IoPrintLn("========================================");
            IoPrint("You: ");

            // 独立线程读 socket 输入，主线程跑 WebRTC 消息循环
            std::thread reader_thread([] {
                while (g_chat_running && IoConnected()) {
                    std::string line = IoReadLine();
                    if (line.empty() && !IoConnected()) break;
                    if (line.empty()) {
                        IoPrint("You: ");
                        continue;
                    }
                    if (line == "/quit") {
                        // 参考 dc_call DoHangup：先 Close 再 Quit
                        g_thread->PostTask([] {
                            if (g_dc && g_dc->state() == DataChannelInterface::kOpen) {
                                g_dc->Send(DataBuffer("/bye"));
                            }
                            g_chat_running = false;
                            if (g_dc) { g_dc->Close(); g_dc = nullptr; }
                            if (g_pc) { g_pc->Close(); g_pc = nullptr; }
                            g_factory = nullptr;
                            g_thread->Quit();
                        });
                        break;
                    }
                    // 发送消息必须 Post 到 g_thread
                    g_thread->PostTask([msg = std::move(line)] {
                        if (g_dc && g_dc->state() == DataChannelInterface::kOpen) {
                            g_dc->Send(DataBuffer(msg));
                        }
                    });
                    IoPrint("You: ");
                }
            });

            g_thread->Run();        // 主线程跑 WebRTC 消息循环
            reader_thread.join();   // 等待读线程退出

            IoPrintLn("\nChat ended.");
        }
    }

}



int main(int argc, char* argv[])
{

    RTC_LOG(LS_INFO) << "WebRTC started";

    g_pss = std::make_unique<PhysicalSocketServer>();
    g_thread = std::make_unique<Thread>(g_pss.get());
    ThreadManager::Instance()->SetCurrentThread(g_thread.get());
    InitializeSSL();

#ifdef VERBOSE
    // Introspection — only public APIs
    RTC_LOG(LS_INFO) << "=== Thread introspection ===";
    RTC_LOG(LS_INFO) << "g_thread->name(): " << g_thread->name();
    RTC_LOG(LS_INFO) << "g_thread->IsCurrent(): " << g_thread->IsCurrent();
    RTC_LOG(LS_INFO) << "ThreadManager::Instance(): " << ThreadManager::Instance();
    RTC_LOG(LS_INFO) << "ThreadManager::CurrentThread(): "
                     << ThreadManager::Instance()->CurrentThread();
    RTC_LOG(LS_INFO) << "pss.get(): " << pss.get();

    RTC_LOG(LS_INFO) << "Signaling g_thread set up";
#endif

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
    
    std::string runtime_dir = "/tmp/webrtc_runtime";
    g_socket = std::make_unique<UnixSocketServer>(runtime_dir + "/dc_call.sock");
    RTC_LOG(LS_INFO) << "Daemon waiting for cilent...";
    if(!g_socket->WaitForClient()){
        RTC_LOG(LS_ERROR) << "Socket accept failed";
    }
    g_socket->WriteLine("Connected!");
    DoOfferFlow();


    //auto obs = make_ref_counted<DcCreate

    // === 关闭流程 ===
    // 正常退出时 DC/PC 已在 reader 线程的 /quit 或 /bye 处理中关闭。
    // 这里的操作只会处理异常路径（如超时未连接成功）。
    RTC_LOG(LS_INFO) << "Shutting down...";

    if (g_dc) {
        g_dc->Close();
        g_dc = nullptr;
    }
    if (g_pc) {
        g_pc->Close();
        g_pc = nullptr;
    }
    g_factory = nullptr;

    ThreadManager::Instance()->SetCurrentThread(nullptr);
    CleanupSSL();

    return 0;
}

