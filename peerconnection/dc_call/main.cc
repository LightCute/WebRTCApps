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

ABSL_FLAG(bool, offer, false, "Run as the offer side");
ABSL_FLAG(bool, standalone, false, "Run in standalone CLI mode (stdin/stdout)");
ABSL_FLAG(std::string, force_fieldtrials, "", "Field trials override");

using namespace webrtc;

// ===== Global state =====
bool g_is_offer = false;
bool g_standalone = false;
std::unique_ptr<PhysicalSocketServer> g_pss;
std::unique_ptr<Thread> g_thread;
scoped_refptr<PeerConnectionFactoryInterface> g_factory;
scoped_refptr<PeerConnectionInterface> g_pc;
scoped_refptr<DataChannelInterface> g_dc;

// Socket (daemon mode only)
std::unique_ptr<UnixSocketServer> g_socket;

// Sync flags
bool g_sdp_ready = false;
bool g_remote_set = false;
bool g_remote_failed = false;
std::string g_local_sdp;
std::string g_error_msg;

struct CandidateInfo {
  std::string mid;
  int mline_index = 0;
  std::string candidate_str;
};
std::vector<CandidateInfo> g_local_candidates;

std::atomic<bool> g_chat_running{true};

// ===== I/O helpers (switch between standalone and daemon mode) =====
void IoPrint(const std::string& s) {
  if (g_standalone) {
    std::cout << s << std::flush;
  } else if (g_socket) {
    g_socket->Write(s);
  }
}

void IoPrintLn(const std::string& s) {
  if (g_standalone) {
    std::cout << s << std::endl;
  } else if (g_socket) {
    g_socket->WriteLine(s);
  }
}

// Blocking read. Returns empty on EOF/disconnect.
std::string IoReadLine() {
  if (g_standalone) {
    std::string line;
    if (!std::getline(std::cin, line)) return {};
    return line;
  }
  if (g_socket && g_socket->IsConnected()) {
    return g_socket->ReadLine();
  }
  return {};
}

bool IoConnected() {
  if (g_standalone) return true;
  return g_socket && g_socket->IsConnected();
}

// ===== Forward declarations =====
class DcDataObserver;
class DcSetLocalObserver;
class DcSetRemoteObserver;
void ProcessUntil(std::function<bool()> cond, int poll_ms = 50);

// ===== DataChannelObserver =====
class DcDataObserver : public DataChannelObserver {
 public:
  void OnStateChange() override {
    if (!g_dc) return;
    auto state = g_dc->state();
    RTC_LOG(LS_INFO) << "DataChannel state: " << state;
    if (state == DataChannelInterface::kClosed ||
        state == DataChannelInterface::kClosing) {
      if (g_chat_running) {
        IoPrintLn("\nPeer disconnected.");
        g_chat_running = false;
        if (g_pc) g_pc->Close();
        g_thread->Quit();
      }
    }
  }

  void OnMessage(const DataBuffer& buffer) override {
    std::string msg(buffer.data.data<char>(), buffer.data.size());
    if (msg == "/bye") {
      IoPrintLn("\nPeer hung up.");
      g_chat_running = false;
      if (g_dc) g_dc->Close();
      if (g_pc) g_pc->Close();
      g_thread->Quit();
      return;
    }
    std::string out = "\nPeer: " + msg + "\nYou: ";
    IoPrint(out);
  }

  void OnBufferedAmountChange(uint64_t sent_data_size) override {}
};

DcDataObserver g_data_observer;

// ===== PeerConnectionObserver =====
class DcPeerObserver : public PeerConnectionObserver {
 public:
  void OnSignalingChange(
      PeerConnectionInterface::SignalingState state) override {
    RTC_LOG(LS_INFO) << "SignalingState: " << state;
  }

  void OnIceCandidate(const IceCandidateInterface* candidate) override {
    CandidateInfo info;
    info.mid = candidate->sdp_mid();
    info.mline_index = candidate->sdp_mline_index();
    candidate->ToString(&info.candidate_str);
    g_local_candidates.push_back(std::move(info));
    RTC_LOG(LS_INFO) << "Local ICE candidate [" << info.mline_index << "|"
                     << info.mid << "]: " << info.candidate_str;
  }

  void OnDataChannel(scoped_refptr<DataChannelInterface> dc) override {
    RTC_LOG(LS_INFO) << "DataChannel received (answer side)";
    g_dc = dc;
    g_dc->RegisterObserver(&g_data_observer);
  }

  void OnIceGatheringChange(
      PeerConnectionInterface::IceGatheringState state) override {
    RTC_LOG(LS_INFO) << "IceGatheringState: "
                     << PeerConnectionInterface::AsString(state);
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
      g_chat_running = false;
    } else if (state ==
               PeerConnectionInterface::PeerConnectionState::kDisconnected) {
      RTC_LOG(LS_INFO) << "Disconnected";
      g_chat_running = false;
    }
  }

  void OnIceCandidateError(const std::string& address,
                           int port,
                           const std::string& url,
                           int error_code,
                           const std::string& error_text) override {
    RTC_LOG(LS_WARNING) << "ICE candidate error — " << address << ":" << port
                        << " url=" << url << " code=" << error_code
                        << " text=" << error_text;
  }

  void OnRenegotiationNeeded() override {
    RTC_LOG(LS_INFO) << "Renegotiation needed";
  }
};

DcPeerObserver g_pc_observer;

// ===== SetLocalDescriptionObserver =====
class DcSetLocalObserver : public SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(RTCError error) override {
    if (error.ok()) {
      RTC_LOG(LS_INFO) << "SetLocalDescription OK";
    } else {
      g_error_msg = error.message();
      RTC_LOG(LS_ERROR) << "SetLocalDescription failed: " << g_error_msg;
    }
    g_sdp_ready = true;
  }
};

// ===== SetRemoteDescriptionObserver =====
class DcSetRemoteObserver : public SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(RTCError error) override {
    if (error.ok()) {
      RTC_LOG(LS_INFO) << "SetRemoteDescription OK";
      g_remote_set = true;
    } else {
      g_error_msg = error.message();
      RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: " << g_error_msg;
      g_remote_failed = true;
      g_remote_set = true;
    }
  }
};

// ===== CreateSessionDescriptionObserver =====
class DcCreateSdpObserver : public CreateSessionDescriptionObserver {
 public:
  void OnSuccess(SessionDescriptionInterface* desc) override {
    RTC_LOG(LS_INFO) << "Local SDP created (type=" << desc->GetType() << ")";
    desc->ToString(&g_local_sdp);
    g_pc->SetLocalDescription(
        std::unique_ptr<SessionDescriptionInterface>(desc),
        make_ref_counted<DcSetLocalObserver>());
  }

  void OnFailure(RTCError error) override {
    g_error_msg = error.message();
    g_sdp_ready = true;
  }
};

// ===== Helper =====
void ProcessUntil(std::function<bool()> cond, int poll_ms) {
  while (IoConnected() && !cond()) {
    g_thread->ProcessMessages(poll_ms);
  }
}

// ===== Initialize WebRTC =====
void InitWebRtcEnv() {
  g_pss = std::make_unique<PhysicalSocketServer>();
  g_thread = std::make_unique<Thread>(g_pss.get());
  ThreadManager::Instance()->SetCurrentThread(g_thread.get());
  InitializeSSL();
  RTC_LOG(LS_INFO) << "WebRTC initialized";
}

// ===== Create PeerConnectionFactory (DataChannel-only, no media) =====
void InitFactory(const Environment& env) {
  PeerConnectionFactoryDependencies deps;
  deps.signaling_thread = g_thread.get();
  deps.network_thread = g_thread.get();
  deps.worker_thread = g_thread.get();
  deps.env = env;
  deps.adm = nullptr;
  // DataChannel-only: no audio/video codec factories, no EnableMedia().
  // SCTP/DTLS transport is set up by the PeerConnection itself.

  g_factory = CreateModularPeerConnectionFactory(std::move(deps));
  if (!g_factory) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
    exit(1);
  }
  RTC_LOG(LS_INFO) << "PeerConnectionFactory created";
}

// ===== Create PeerConnection (per call) =====
void CreatePc() {
  PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = SdpSemantics::kUnifiedPlan;

  PeerConnectionInterface::IceServer stun;
  stun.uri = GetSTUNServer();
  config.servers.push_back(stun);

  std::string turn_uri = GetTURNServer();
  std::string turn_user = GetTurnUserName();
  std::string turn_pass = GetTurnPassword();
  if (!turn_uri.empty()) {
    PeerConnectionInterface::IceServer turn;
    turn.uri = turn_uri;
    turn.username = turn_user;
    turn.password = turn_pass;
    config.servers.push_back(turn);
    RTC_LOG(LS_INFO) << "TURN server configured: " << turn_uri;
  }

  PeerConnectionDependencies pc_deps(&g_pc_observer);
  auto err_or =
      g_factory->CreatePeerConnectionOrError(config, std::move(pc_deps));
  if (!err_or.ok()) {
    RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: "
                      << err_or.error().message();
    exit(1);
  }
  g_pc = std::move(err_or.value());
  RTC_LOG(LS_INFO) << "PeerConnection created";
}

// ===== Reset per-call state =====
void ResetCallState() {
  g_dc = nullptr;
  g_pc = nullptr;
  g_sdp_ready = false;
  g_remote_set = false;
  g_remote_failed = false;
  g_local_sdp.clear();
  g_error_msg.clear();
  g_local_candidates.clear();
  g_chat_running = true;
}

// ===== Clean up current call (keep factory, socket, SSL alive) =====
void CleanupCall() {
  if (g_dc) {
    g_dc->Close();
    g_dc = nullptr;
  }
  if (g_pc) {
    g_pc->Close();
    g_pc = nullptr;
  }
  ResetCallState();
  RTC_LOG(LS_INFO) << "Call cleaned up, ready for next";
}

// ===== Offer flow =====
void DoOfferFlow() {
  DataChannelInit dc_init;
  dc_init.negotiated = false;
  dc_init.id = 0;
  auto dc_or_error = g_pc->CreateDataChannelOrError("chat", &dc_init);
  if (!dc_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to create DataChannel: "
                      << dc_or_error.error().message();
    exit(1);
  }
  g_dc = std::move(dc_or_error.value());
  g_dc->RegisterObserver(&g_data_observer);
  RTC_LOG(LS_INFO) << "DataChannel created (offer side)";

  g_sdp_ready = false;
  g_error_msg.clear();
  auto obs = make_ref_counted<DcCreateSdpObserver>();
  PeerConnectionInterface::RTCOfferAnswerOptions options;
  g_pc->CreateOffer(obs.get(), options);

  ProcessUntil([] { return g_sdp_ready; }, 50);
  if (!g_error_msg.empty()) {
    RTC_LOG(LS_ERROR) << "CreateOffer/SetLocal failed: " << g_error_msg;
    exit(1);
  }

  RTC_LOG(LS_INFO) << "Collecting ICE candidates...";
  g_thread->ProcessMessages(2000);

  IoPrintLn("\n========================================");
  IoPrintLn("=== WebRTC DataChannel Chat (OFFER) ===");
  IoPrintLn("========================================");
  IoPrintLn("\nLocal OFFER SDP (copy to answerer):");
  IoPrintLn("----------------------------------------");
  IoPrint(g_local_sdp);
  IoPrintLn("----------------------------------------");

  IoPrintLn("\nPaste remote ANSWER SDP (end with empty line):");
  std::string remote_sdp;
  std::string line;
  while (IoConnected() && !(line = IoReadLine()).empty() &&
         !line.empty()) {
    remote_sdp += line + "\n";
  }
  if (!IoConnected()) {
    RTC_LOG(LS_INFO) << "Client disconnected during SDP exchange";
    exit(0);
  }
  if (remote_sdp.empty()) {
    RTC_LOG(LS_ERROR) << "No remote SDP provided";
    exit(1);
  }

  g_remote_set = false;
  g_remote_failed = false;
  g_error_msg.clear();
  std::unique_ptr<SessionDescriptionInterface> answer =
      CreateSessionDescription(SdpType::kAnswer, remote_sdp);
  if (!answer) {
    RTC_LOG(LS_ERROR) << "Failed to parse remote answer SDP";
    exit(1);
  }
  g_pc->SetRemoteDescription(std::move(answer),
                             make_ref_counted<DcSetRemoteObserver>());

  ProcessUntil([] { return g_remote_set; }, 50);
  if (g_remote_failed) {
    RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: " << g_error_msg;
    exit(1);
  }
  RTC_LOG(LS_INFO) << "SDP exchange complete";
}

// ===== Answer flow =====
void DoAnswerFlow() {
  IoPrintLn("\n==========================================");
  IoPrintLn("=== WebRTC DataChannel Chat (ANSWER) ===");
  IoPrintLn("==========================================");

  IoPrintLn("\nPaste remote OFFER SDP (end with empty line):");
  std::string remote_sdp;
  std::string line;
  while (IoConnected() && !(line = IoReadLine()).empty()) {
    if (line.empty()) break;
    remote_sdp += line + "\n";
  }
  if (!IoConnected() || remote_sdp.empty()) {
    RTC_LOG(LS_ERROR) << "No remote SDP provided or disconnected";
    exit(1);
  }

  g_remote_set = false;
  g_remote_failed = false;
  g_error_msg.clear();
  std::unique_ptr<SessionDescriptionInterface> offer =
      CreateSessionDescription(SdpType::kOffer, remote_sdp);
  if (!offer) {
    RTC_LOG(LS_ERROR) << "Failed to parse remote offer SDP";
    exit(1);
  }
  g_pc->SetRemoteDescription(std::move(offer),
                             make_ref_counted<DcSetRemoteObserver>());

  ProcessUntil([] { return g_remote_set; }, 50);
  if (g_remote_failed) {
    RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: " << g_error_msg;
    exit(1);
  }

  g_sdp_ready = false;
  g_error_msg.clear();
  auto obs = make_ref_counted<DcCreateSdpObserver>();
  PeerConnectionInterface::RTCOfferAnswerOptions options;
  g_pc->CreateAnswer(obs.get(), options);

  ProcessUntil([] { return g_sdp_ready; }, 50);
  if (!g_error_msg.empty()) {
    RTC_LOG(LS_ERROR) << "CreateAnswer/SetLocal failed: " << g_error_msg;
    exit(1);
  }

  RTC_LOG(LS_INFO) << "Collecting ICE candidates...";
  g_thread->ProcessMessages(2000);

  IoPrintLn("\nLocal ANSWER SDP (copy to offerer):");
  IoPrintLn("----------------------------------------");
  IoPrint(g_local_sdp);
  IoPrintLn("----------------------------------------");

  RTC_LOG(LS_INFO) << "SDP exchange complete";
}

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

// ===== Hangup helper (must run on signaling thread) =====
void DoHangup() {
  if (!g_chat_running) return;  // Already hung up
  g_chat_running = false;

  // Send a bye message so the remote peer knows we're leaving
  if (g_dc && g_dc->state() == DataChannelInterface::kOpen) {
    RTC_LOG(LS_INFO) << "Sending bye message";
    g_dc->Send(DataBuffer(std::string("/bye")));
  }

  // Tear down this call's PC/DC.  In daemon mode the factory, socket
  // server and g_thread stay alive for the next call.
  CleanupCall();
  g_thread->Quit();

  // Disconnect the old nc client so the daemon loop can accept a new one.
  if (!g_standalone && g_socket) {
    g_socket->Disconnect();
  }
}

// ===== Socket reader thread (daemon mode) or stdin thread (standalone) =====
void SocketReaderLoop() {
  IoPrintLn("DataChannel OPEN — ready to chat!");
  IoPrintLn("Type your message and press Enter. '/quit' to exit.");
  IoPrintLn("========================================");
  IoPrint("You: ");

  std::string line;
  while (g_chat_running && IoConnected()) {
    line = IoReadLine();
    if (line.empty() && !IoConnected()) break;
    if (line.empty()) {
      IoPrint("You: ");
      continue;
    }
    if (line == "/quit") {
      // Hang up current call.  In daemon mode the process stays alive
      // for the next call; Ctrl+C to exit the daemon entirely.
      g_thread->PostTask([] { DoHangup(); });
      break;
    }

    std::string msg = std::move(line);
    g_thread->PostTask([msg = std::move(msg)] {
      if (g_dc && g_dc->state() == DataChannelInterface::kOpen) {
        g_dc->Send(DataBuffer(msg));
      }
    });
    IoPrint("You: ");
  }
}

// ===== Cleanup =====
void Cleanup() {
  if (g_dc) {
    g_dc->Close();
    g_dc = nullptr;
  }
  if (g_pc) {
    g_pc->Close();
    g_pc = nullptr;
  }
  g_factory = nullptr;
  if (g_thread) {
    g_thread->Quit();
  }
  ThreadManager::Instance()->SetCurrentThread(nullptr);
  CleanupSSL();
}

// ===== Setup file logging =====
FileRotatingLogSink* SetupFileLogging(const std::string& runtime_dir) {
  mkdir(runtime_dir.c_str(), 0755);

  int log_index = 0;
  DIR* dir = opendir(runtime_dir.c_str());
  if (dir) {
    struct dirent* ent;
    while ((ent = readdir(dir))) {
      int n = 0;
      if (sscanf(ent->d_name, "dc_call_%d.", &n) == 1 && n >= log_index)
        log_index = n + 1;
    }
    closedir(dir);
  }

  std::string log_prefix = "dc_call_" + std::to_string(log_index);
  auto* sink =
      new FileRotatingLogSink(runtime_dir, log_prefix, 10 * 1024 * 1024, 5);
  sink->Init();
  sink->DisableBuffering();
  LogMessage::AddLogToStream(sink, LS_INFO);
  return sink;
}

// ===== Main =====
int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  g_is_offer = absl::GetFlag(FLAGS_offer);
  g_standalone = absl::GetFlag(FLAGS_standalone);

  // Runtime directory
  const char* rt_dir_env = getenv("WEBRTC_RUNTIME_DIR");
  std::string runtime_dir = rt_dir_env ? rt_dir_env : "/tmp/webrtc_runtime";

  // File logging
  FileRotatingLogSink* file_sink = SetupFileLogging(runtime_dir);

  // Console logging (stderr, not interfering with socket/stdout)
  // RTC_LOG already outputs to stderr by default.

  RTC_LOG(LS_INFO) << "dc_call starting as "
                   << (g_is_offer ? "OFFERER" : "ANSWERER")
                   << " mode=" << (g_standalone ? "standalone" : "daemon")
                   << " runtime_dir=" << runtime_dir;

  InitWebRtcEnv();

  Environment env = CreateEnvironment(std::make_unique<FieldTrials>(
      absl::GetFlag(FLAGS_force_fieldtrials)));

  InitFactory(env);

  // Daemon mode: keep-alive loop, accept one call at a time
  if (!g_standalone) {
    g_socket = std::make_unique<UnixSocketServer>(runtime_dir +
                                                   "/dc_call.sock");
    bool running = true;
    while (running) {
      RTC_LOG(LS_INFO) << "Daemon waiting for client...";
      if (!g_socket->WaitForClient()) {
        RTC_LOG(LS_ERROR) << "Socket accept failed";
        break;
      }

      ResetCallState();
      CreatePc();

      if (g_is_offer) {
        DoOfferFlow();
      } else {
        DoAnswerFlow();
      }

      if (!IoConnected()) {
        RTC_LOG(LS_INFO) << "Client disconnected before ICE, waiting for next";
        CleanupCall();
        continue;
      }

      PrintLocalCandidates();
      ReadRemoteCandidates();

      if (!IoConnected()) {
        RTC_LOG(LS_INFO) << "Client disconnected during ICE, waiting for next";
        CleanupCall();
        continue;
      }

      // Wait for DataChannel to open
      int wait_count = 0;
      bool dc_open = false;
      while (!dc_open) {
        g_thread->ProcessMessages(200);
        if (!g_chat_running) { RTC_LOG(LS_INFO) << "Connection failed"; break; }
        if (!IoConnected()) { RTC_LOG(LS_INFO) << "Client disconnected"; break; }
        if (g_dc && g_dc->state() == DataChannelInterface::kOpen) {
          dc_open = true;
          break;
        }
        if (++wait_count > 150) { RTC_LOG(LS_ERROR) << "DC open timeout"; break; }
      }

      if (!dc_open) {
        CleanupCall();
        continue;
      }

      // Chat loop — g_thread->Run() blocks until DoHangup calls Quit()
      g_chat_running = true;
      std::thread reader_thread(SocketReaderLoop);
      g_thread->Run();
      reader_thread.join();

      // After hangup: CleanupCall was already done by DoHangup.
      // /quit daemon → break; otherwise loop for next call.
      IoPrintLn("\nReady for next call.");
    }
    LogMessage::RemoveLogToStream(file_sink);
    delete file_sink;
    Cleanup();
    return 0;
  }

  // Standalone mode: single call, then exit (original behaviour)
  CreatePc();

  if (g_is_offer) {
    DoOfferFlow();
  } else {
    DoAnswerFlow();
  }

  if (!IoConnected()) { Cleanup(); return 0; }

  PrintLocalCandidates();
  ReadRemoteCandidates();

  if (!IoConnected()) { Cleanup(); return 0; }

  RTC_LOG(LS_INFO) << "Waiting for DataChannel to open (standalone)...";
  int wait_count = 0;
  while (!g_dc || g_dc->state() != DataChannelInterface::kOpen) {
    g_thread->ProcessMessages(200);
    if (!g_chat_running) break;
    if (++wait_count > 150) break;
  }

  if (!g_dc || g_dc->state() != DataChannelInterface::kOpen) {
    Cleanup();
    LogMessage::RemoveLogToStream(file_sink);
    delete file_sink;
    return 1;
  }

  g_chat_running = true;
  std::thread reader_thread(SocketReaderLoop);
  g_thread->Run();
  reader_thread.join();

  IoPrintLn("\nGoodbye.");
  Cleanup();
  LogMessage::RemoveLogToStream(file_sink);
  delete file_sink;
  return 0;
}
