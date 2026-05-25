#ifndef APPS_PEERCONNECTION_CLIENT_JSON_HELPERS_H_
#define APPS_PEERCONNECTION_CLIENT_JSON_HELPERS_H_

#include <string>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "apps/peerconnection/client_arm64/signaling_interface.h"

// JSON field names for ICE candidate serialization.
extern const char kCandidateSdpMidName[];
extern const char kCandidateSdpMlineIndexName[];
extern const char kCandidateSdpName[];

// JSON field names for SessionDescription serialization.
extern const char kSessionDescriptionTypeName[];
extern const char kSessionDescriptionSdpName[];

// Escapes special characters for JSON string values.
std::string EscapeJsonString(const std::string& input);

// Builds a {"event":"peer_list","peers":[...]} JSON string.
std::string BuildPeerListJson(const Peers& peers);

// Converts ICE connection state enum to string.
const char* IceConnectionStateToString(
    webrtc::PeerConnectionInterface::IceConnectionState state);

// Converts DataChannel state enum to string.
const char* DataChannelStateToString(
    webrtc::DataChannelInterface::DataState state);

#endif  // APPS_PEERCONNECTION_CLIENT_JSON_HELPERS_H_
