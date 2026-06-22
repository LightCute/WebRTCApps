#include <jni.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "api/create_modular_peer_connection_factory.h"
#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "apps/peerconnection/client/data_channel_manager.h"
#include "apps/peerconnection/client/defaults.h"
#include "apps/peerconnection/client/json_helpers.h"
#include "apps/peerconnection/client/peer_connection_client.h"
#include "apps/peerconnection/client/signaling_interface.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

using namespace webrtc;

// ═══════════════════════════════════════════════════════════
// Global JNI state
// ═══════════════════════════════════════════════════════════
static JavaVM* g_jvm = nullptr;
static jobject g_activity = nullptr;
static std::mutex g_activity_mutex;

void JniSetJavaVM(JavaVM* vm) { g_jvm = vm; }
JavaVM* JniGetJavaVM() { return g_jvm; }

void JniSetActivity(jobject a) {
  JNIEnv* env;
  g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  std::lock_guard<std::mutex> lock(g_activity_mutex);
  if (g_activity) env->DeleteGlobalRef(g_activity);
  g_activity = env->NewGlobalRef(a);
}

// Get JNIEnv, attach current thread if needed
static JNIEnv* GetEnv() {
  JNIEnv* env = nullptr;
  if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    g_jvm->AttachCurrentThread(&env, nullptr);
  }
  return env;
}

// Call a void-arg void-return Java method on MainActivity
static void CallJavaVoidMethod(const char* name, const char* sig, ...) {
  JNIEnv* env = GetEnv();
  std::lock_guard<std::mutex> lock(g_activity_mutex);
  if (!g_activity || !env) return;

  jclass cls = env->GetObjectClass(g_activity);
  jmethodID mid = env->GetMethodID(cls, name, sig);
  if (!mid) { env->ExceptionClear(); return; }

  va_list args;
  va_start(args, sig);
  env->CallVoidMethodV(g_activity, mid, args);
  va_end(args);
  env->ExceptionClear();
}

static void JniCallback(const char* method) {
  CallJavaVoidMethod(method, "()V");
}
static void JniCallbackInt(const char* method, int a) {
  CallJavaVoidMethod(method, "(I)V", a);
}
static void JniCallbackString(const char* method, const std::string& a) {
  JNIEnv* env = GetEnv();
  std::lock_guard<std::mutex> lock(g_activity_mutex);
  if (!g_activity) return;
  jclass cls = env->GetObjectClass(g_activity);
  jmethodID mid = env->GetMethodID(cls, method, "(Ljava/lang/String;)V");
  if (!mid) { env->ExceptionClear(); return; }
  jstring js = env->NewStringUTF(a.c_str());
  env->CallVoidMethod(g_activity, mid, js);
  env->DeleteLocalRef(js);
  env->ExceptionClear();
}
static void JniCallbackIntString(const char* method, int a, const std::string& b) {
  JNIEnv* env = GetEnv();
  std::lock_guard<std::mutex> lock(g_activity_mutex);
  if (!g_activity) return;
  jclass cls = env->GetObjectClass(g_activity);
  jmethodID mid = env->GetMethodID(cls, method, "(ILjava/lang/String;)V");
  if (!mid) { env->ExceptionClear(); return; }
  jstring js = env->NewStringUTF(b.c_str());
  env->CallVoidMethod(g_activity, mid, a, js);
  env->DeleteLocalRef(js);
  env->ExceptionClear();
}

// ═══════════════════════════════════════════════════════════
// WebRTC threads & state
// ═══════════════════════════════════════════════════════════
static std::unique_ptr<webrtc::Thread> g_signaling_thread;
static std::unique_ptr<webrtc::Thread> g_worker_thread;
static std::unique_ptr<webrtc::Thread> g_network_thread;

static std::unique_ptr<PeerConnectionClient> g_signaling;
static scoped_refptr<PeerConnectionFactoryInterface> g_factory;
static scoped_refptr<PeerConnectionInterface> g_pc;
static std::unique_ptr<DataChannelManager> g_dc_manager;

static int g_peer_id = -1;
static std::atomic<bool> g_call_in_progress{false};

// Forward declarations for observer classes
class AndroidSetLocalObserver;
class AndroidCreateSdpObserver;

// ═══════════════════════════════════════════════════════════
// Signaling observer — bridges PeerConnectionClientObserver to Java
// ═══════════════════════════════════════════════════════════
class AndroidSignalingObserver : public PeerConnectionClientObserver {
  void OnSignedIn() override {
    RTC_LOG(LS_INFO) << "Signed in, my_id=" << g_signaling->id();
    JniCallback("onSignedIn");
  }
  void OnDisconnected() override {
    RTC_LOG(LS_INFO) << "Disconnected from server";
    JniCallback("onDisconnected");
  }
  void OnPeerConnected(int id, const std::string& name) override {
    RTC_LOG(LS_INFO) << "Peer connected: " << id << " " << name;
    g_peer_id = id;
    JniCallbackIntString("onPeerConnected", id, name);
  }
  void OnPeerDisconnected(int id) override {
    RTC_LOG(LS_INFO) << "Peer disconnected: " << id;
    JniCallbackInt("onPeerDisconnected", id);
  }
  void OnPeerBusy(int peer_id) override {
    JniCallbackString("onError", "Peer " + std::to_string(peer_id) + " busy");
  }
  void OnMessageFromPeer(int peer_id, const std::string& message) override {
    RTC_LOG(LS_INFO) << "Message from peer " << peer_id << ": " << message;
    if (message == "BYE") {
      JniCallback("onCallDisconnected");
      g_call_in_progress = false;
      return;
    }
    JniCallbackString("onDataReceived", message);
  }
  void OnMessageSent(int err) override {
    if (err != 0) RTC_LOG(LS_WARNING) << "Message send error: " << err;
  }
  void OnServerConnectionFailure() override {
    JniCallbackString("onError", "Server connection failed");
  }
};
static AndroidSignalingObserver g_signaling_observer;

// ═══════════════════════════════════════════════════════════
// CreateSessionDescriptionObserver (Offer & Answer)
// ═══════════════════════════════════════════════════════════
class AndroidCreateSdpObserver : public CreateSessionDescriptionObserver {
 public:
  void OnSuccess(SessionDescriptionInterface* desc) override {
    std::string sdp_str;
    desc->ToString(&sdp_str);
    auto local_obs = make_ref_counted<AndroidSetLocalObserver>();
    g_pc->SetLocalDescription(
        std::unique_ptr<SessionDescriptionInterface>(desc),
        local_obs.get());
    // Send SDP to peer via signaling
    Json::Value msg;
    msg[kSessionDescriptionTypeName] = desc->GetType();
    msg[kSessionDescriptionSdpName] = sdp_str;
    std::string json_str = Json::writeString(Json::StreamWriterBuilder(), msg);
    g_signaling->SendToPeer(g_peer_id, json_str);
  }
  void OnFailure(RTCError error) override {
    RTC_LOG(LS_ERROR) << "CreateSDP failed: " << error.message();
    std::string err_msg = "CreateSDP failed: ";
    err_msg += error.message();
    JniCallbackString("onError", err_msg);
  }
};

// ═══════════════════════════════════════════════════════════
// SetLocalDescriptionObserver
// ═══════════════════════════════════════════════════════════
class AndroidSetLocalObserver : public SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(RTCError error) override {
    if (!error.ok()) {
      RTC_LOG(LS_ERROR) << "SetLocalDescription failed: " << error.message();
    }
  }
};

// ═══════════════════════════════════════════════════════════
// PeerConnectionObserver
// ═══════════════════════════════════════════════════════════
class AndroidPcObserver : public PeerConnectionObserver {
 public:
  void OnSignalingChange(PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(scoped_refptr<DataChannelInterface> dc) override {
    RTC_LOG(LS_INFO) << "DataChannel received (answer side)";
    if (g_dc_manager)
      g_dc_manager->OnRemoteDataChannel(dc);
  }
  void OnIceCandidate(const IceCandidateInterface* candidate) override {
    std::string mid = candidate->sdp_mid();
    int mline = candidate->sdp_mline_index();
    std::string cand_str;
    candidate->ToString(&cand_str);
    Json::Value msg;
    msg[kCandidateSdpMidName] = mid;
    msg[kCandidateSdpMlineIndexName] = mline;
    msg[kCandidateSdpName] = cand_str;
    std::string json_str = Json::writeString(Json::StreamWriterBuilder(), msg);
    g_signaling->SendToPeer(g_peer_id, json_str);
  }
  void OnIceGatheringChange(PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidateError(const std::string&, int, const std::string&,
                           int, const std::string&) override {}
  void OnConnectionChange(PeerConnectionInterface::PeerConnectionState state) override {
    if (state == PeerConnectionInterface::PeerConnectionState::kConnected) {
      JniCallback("onCallConnected");
      g_call_in_progress = true;
    } else if (state == PeerConnectionInterface::PeerConnectionState::kFailed ||
               state == PeerConnectionInterface::PeerConnectionState::kDisconnected) {
      JniCallback("onCallDisconnected");
      g_call_in_progress = false;
    }
  }
  void OnAddTrack(scoped_refptr<RtpReceiverInterface>,
                  const std::vector<scoped_refptr<MediaStreamInterface>>&) override {}
  void OnRemoveTrack(scoped_refptr<RtpReceiverInterface>) override {}
  void OnRenegotiationNeeded() override {}
};
static AndroidPcObserver g_pc_observer;

// ═══════════════════════════════════════════════════════════
// PeerConnectionFactory + PeerConnection creation
// ═══════════════════════════════════════════════════════════
static bool CreatePeerConnection() {
  PeerConnectionFactoryDependencies factory_deps;
  factory_deps.network_thread = g_network_thread.get();
  factory_deps.worker_thread = g_worker_thread.get();
  factory_deps.signaling_thread = g_signaling_thread.get();

  g_factory = CreateModularPeerConnectionFactory(std::move(factory_deps));
  if (!g_factory) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return false;
  }

  PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = SdpSemantics::kUnifiedPlan;

  PeerConnectionInterface::IceServer stun_server;
  stun_server.uri = GetSTUNServer();
  config.servers.push_back(stun_server);

  PeerConnectionDependencies pc_deps(&g_pc_observer);
  auto result = g_factory->CreatePeerConnectionOrError(config, std::move(pc_deps));
  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: " << result.error().message();
    return false;
  }
  g_pc = std::move(result.value());
  g_dc_manager = std::make_unique<DataChannelManager>();
  g_dc_manager->SetEventCallback([](const std::string& json) {
    JniCallbackString("onDataReceived", json);
  });
  return true;
}

// ═══════════════════════════════════════════════════════════
// JNI exported functions
// ═══════════════════════════════════════════════════════════
extern "C" {

JNIEXPORT void JNICALL
Java_org_light_webrtc_MainActivity_nativeConnect(
    JNIEnv* env, jobject activity, jstring server, jint port, jstring name) {
  JniSetActivity(activity);

  const char* server_c = env->GetStringUTFChars(server, nullptr);
  const char* name_c = env->GetStringUTFChars(name, nullptr);
  std::string server_str(server_c);
  std::string name_str(name_c);
  env->ReleaseStringUTFChars(server, server_c);
  env->ReleaseStringUTFChars(name, name_c);

  g_signaling_thread->PostTask([server_str, name_str, port] {
    RTC_LOG(LS_INFO) << "Connecting to " << server_str << ":" << port;
    g_signaling = std::make_unique<PeerConnectionClient>();
    g_signaling->RegisterObserver(&g_signaling_observer);
    g_signaling->Connect(server_str, static_cast<int>(port), name_str);
  });
}

JNIEXPORT void JNICALL
Java_org_light_webrtc_MainActivity_nativeDisconnect(JNIEnv*, jobject) {
  g_signaling_thread->PostTask([] {
    if (g_signaling) g_signaling->SignOut();
  });
}

JNIEXPORT void JNICALL
Java_org_light_webrtc_MainActivity_nativeCall(JNIEnv*, jobject, jint peer_id) {
  g_signaling_thread->PostTask([peer_id] {
    g_peer_id = peer_id;
    g_call_in_progress = false;

    if (!CreatePeerConnection()) {
      JniCallbackString("onError", "Failed to create PeerConnection");
      return;
    }

    // Create negotiated DataChannel (before Offer)
    g_dc_manager->Add(g_pc.get());

    // Create Offer (observer kept alive by the scoped_refptr until callback)
    auto obs = make_ref_counted<AndroidCreateSdpObserver>();
    g_pc->CreateOffer(obs.get(),
        PeerConnectionInterface::RTCOfferAnswerOptions());
    (void)obs;  // suppress unused warning, kept alive until scope end
  });
}

JNIEXPORT void JNICALL
Java_org_light_webrtc_MainActivity_nativeHangup(JNIEnv*, jobject) {
  g_signaling_thread->PostTask([] {
    g_call_in_progress = false;
    if (g_dc_manager) {
      g_dc_manager->Shutdown();
      g_dc_manager.reset();
    }
    if (g_pc) {
      g_pc->Close();
      g_pc = nullptr;
    }
    g_factory = nullptr;
    if (g_signaling && g_peer_id != -1) {
      g_signaling->SendHangUp(g_peer_id);
    }
  });
}

JNIEXPORT void JNICALL
Java_org_light_webrtc_MainActivity_nativeSendData(JNIEnv* env, jobject, jstring msg) {
  const char* msg_c = env->GetStringUTFChars(msg, nullptr);
  std::string msg_str(msg_c);
  env->ReleaseStringUTFChars(msg, msg_c);

  g_signaling_thread->PostTask([msg_str] {
    if (g_dc_manager) {
      g_dc_manager->Send(msg_str);
    }
  });
}

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void*) {
  JniSetJavaVM(vm);

  // Create WebRTC threads
  g_signaling_thread = webrtc::Thread::CreateWithSocketServer();
  g_worker_thread = webrtc::Thread::Create();
  g_network_thread = webrtc::Thread::CreateWithSocketServer();

  g_signaling_thread->Start();
  g_worker_thread->Start();
  g_network_thread->Start();

  InitializeSSL();

  RTC_LOG(LS_INFO) << "JNI_OnLoad done, threads started";
  return JNI_VERSION_1_6;
}

}  // extern "C"
