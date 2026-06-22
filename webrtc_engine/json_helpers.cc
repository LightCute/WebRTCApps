#include "apps/webrtc_engine/json_helpers.h"

#include <string>

// JSON field names for ICE candidate serialization.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// JSON field names for SessionDescription serialization.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";

std::string EscapeJsonString(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (char c : input) {
    switch (c) {
      case '"':  output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b";  break;
      case '\f': output += "\\f";  break;
      case '\n': output += "\\n";  break;
      case '\r': output += "\\r";  break;
      case '\t': output += "\\t";  break;
      default:   output += c;      break;
    }
  }
  return output;
}

std::string BuildPeerListJson(const Peers& peers) {
  std::string json = R"({"event":"peer_list","peers":[)";
  bool first = true;
  for (const auto& p : peers) {
    if (!first)
      json += ",";
    first = false;
    json += "{\"id\":" + std::to_string(p.first) +
            ",\"name\":\"" + p.second + "\"}";
  }
  json += "]}";
  return json;
}

const char* IceConnectionStateToString(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  switch (state) {
    case webrtc::PeerConnectionInterface::kIceConnectionNew:
      return "new";
    case webrtc::PeerConnectionInterface::kIceConnectionChecking:
      return "checking";
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
      return "connected";
    case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
      return "completed";
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
      return "failed";
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
      return "disconnected";
    case webrtc::PeerConnectionInterface::kIceConnectionClosed:
      return "closed";
    default:
      return "unknown";
  }
}

const char* DataChannelStateToString(
    webrtc::DataChannelInterface::DataState s) {
  switch (s) {
    case webrtc::DataChannelInterface::kConnecting: return "connecting";
    case webrtc::DataChannelInterface::kOpen:        return "open";
    case webrtc::DataChannelInterface::kClosing:     return "closing";
    case webrtc::DataChannelInterface::kClosed:      return "closed";
    default:                                         return "unknown";
  }
}
