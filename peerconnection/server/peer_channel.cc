/*
 *  Copyright 2011 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "apps/peerconnection/server/peer_channel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "apps/peerconnection/server/data_socket.h"
#include "rtc_base/checks.h"

// Set to the peer id of the originator when messages are being
// exchanged between peers, but set to the id of the receiving peer
// itself when notifications are sent from the server about the state
// of other peers.
//
// WORKAROUND: Since support for CORS varies greatly from one browser to the
// next, we don't use a custom name for our peer-id header (originally it was
// "X-Peer-Id: ").  Instead, we use a "simple header", "Pragma" which should
// always be exposed to CORS requests.  There is a special CORS header devoted
// to exposing proprietary headers (Access-Control-Expose-Headers), however
// at this point it is not working correctly in some popular browsers.
static const char kPeerIdHeader[] = "Pragma: ";

static const char* kRequestPaths[] = {
    "/wait",
    "/sign_out",
    "/message",
    "/hangup",
    "/hangup_confirm",
};

enum RequestPathIndex {
  kWait,
  kSignOut,
  kMessage,
  kHangUp,
  kHangUpConfirm,
};

const size_t kMaxNameLength = 512;

//
// ChannelMember
//

int ChannelMember::s_member_id_ = 0;

ChannelMember::ChannelMember(DataSocket* socket)
    : waiting_socket_(nullptr),
      id_(++s_member_id_),
      connected_(true),
      timestamp_(time(nullptr)) {
  RTC_DCHECK(socket);
  RTC_DCHECK_EQ(socket->method(), DataSocket::GET);
  RTC_DCHECK(socket->PathEquals("/sign_in"));
  name_ = socket->request_arguments();
  if (name_.empty())
    name_ = "peer_" + absl::StrCat(id_);
  else if (name_.length() > kMaxNameLength)
    name_.resize(kMaxNameLength);

  std::replace(name_.begin(), name_.end(), ',', '_');
}

ChannelMember::~ChannelMember() {}

bool ChannelMember::is_wait_request(DataSocket* ds) const {
  return ds && ds->PathEquals(kRequestPaths[kWait]);
}

bool ChannelMember::TimedOut() {
  return waiting_socket_ == nullptr && (time(nullptr) - timestamp_) > 30;
}

std::string ChannelMember::GetPeerIdHeader() const {
  return kPeerIdHeader + absl::StrCat(id_) + "\r\n";
}

bool ChannelMember::NotifyOfOtherMember(const ChannelMember& other) {
  RTC_DCHECK_NE(&other, this);
  QueueResponse("200 OK", "text/plain", GetPeerIdHeader(), other.GetEntry());
  return true;
}

// Returns a string in the form "name,id,connected,busy\n".
std::string ChannelMember::GetEntry() const {
  RTC_DCHECK(name_.length() <= kMaxNameLength);

  char entry[kMaxNameLength + 20];
  snprintf(entry, sizeof(entry), "%s,%d,%d,%d\n",
           name_.substr(0, kMaxNameLength).c_str(), id_, connected_,
           call_partner_id_ != 0 ? 1 : 0);
  return entry;
}

void ChannelMember::ForwardRequestToPeer(DataSocket* ds, ChannelMember* peer) {
  RTC_DCHECK(peer);
  RTC_DCHECK(ds);

  std::string extra_headers(GetPeerIdHeader());

  if (peer == this) {
    ds->Send("200 OK", true, ds->content_type(), extra_headers, ds->data());
  } else {
    // Check if target is busy (already in a call with someone else)
    if (peer->in_call() && peer->call_partner_id() != id_) {
      const char* busy_msg = "BUSY";
      ds->Send("200 OK", true, "text/plain", extra_headers, busy_msg);
      printf("Client %s tried to call busy %s\n", name_.c_str(),
             peer->name().c_str());
      return;
    }
    printf("Client %s sending to %s\n", name_.c_str(), peer->name().c_str());
    peer->QueueResponse("200 OK", ds->content_type(), extra_headers,
                        ds->data());
    ds->Send("200 OK", true, "text/plain", "", "");
    // Track call state: mark both as in-call with each other
    call_partner_id_ = peer->id();
    peer->set_call_partner(id_);
  }
}

void ChannelMember::OnClosing(DataSocket* ds) {
  if (ds == waiting_socket_) {
    waiting_socket_ = nullptr;
    timestamp_ = time(nullptr);
  }
}

void ChannelMember::QueueResponse(const std::string& status,
                                  const std::string& content_type,
                                  const std::string& extra_headers,
                                  const std::string& data) {
  if (waiting_socket_) {
    RTC_DCHECK(queue_.empty());
    RTC_DCHECK_EQ(waiting_socket_->method(), DataSocket::GET);
    bool ok =
        waiting_socket_->Send(status, true, content_type, extra_headers, data);
    if (!ok) {
      printf("Failed to deliver data to waiting socket\n");
    }
    waiting_socket_ = nullptr;
    timestamp_ = time(nullptr);
  } else {
    QueuedResponse qr;
    qr.status = status;
    qr.content_type = content_type;
    qr.extra_headers = extra_headers;
    qr.data = data;
    queue_.push(qr);
  }
}

void ChannelMember::SetWaitingSocket(DataSocket* ds) {
  RTC_DCHECK_EQ(ds->method(), DataSocket::GET);
  if (ds && !queue_.empty()) {
    RTC_DCHECK(!waiting_socket_);
    const QueuedResponse& response = queue_.front();
    ds->Send(response.status, true, response.content_type,
             response.extra_headers, response.data);
    queue_.pop();
  } else {
    waiting_socket_ = ds;
  }
}

//
// PeerChannel
//

// static
bool PeerChannel::IsPeerConnection(const DataSocket* ds) {
  RTC_DCHECK(ds);
  return (ds->method() == DataSocket::POST && ds->content_length() > 0) ||
         (ds->method() == DataSocket::GET && ds->PathEquals("/sign_in"));
}

ChannelMember* PeerChannel::Lookup(DataSocket* ds) const {
  RTC_DCHECK(ds);

  if (ds->method() != DataSocket::GET && ds->method() != DataSocket::POST)
    return nullptr;

  size_t i = 0;
  for (; i < std::size(kRequestPaths); ++i) {
    if (ds->PathEquals(kRequestPaths[i]))
      break;
  }

  if (i == std::size(kRequestPaths))
    return nullptr;

  std::string args(ds->request_arguments());
  static constexpr absl::string_view kPeerId = "peer_id=";
  size_t found = args.find(kPeerId);
  if (found == std::string::npos)
    return nullptr;

  int id = atoi(&args[found + kPeerId.size()]);
  Members::const_iterator iter = members_.begin();
  for (; iter != members_.end(); ++iter) {
    if (id == (*iter)->id()) {
      if (i == kWait)
        (*iter)->SetWaitingSocket(ds);
      if (i == kSignOut)
        (*iter)->set_disconnected();
      return *iter;
    }
  }

  return nullptr;
}

ChannelMember* PeerChannel::IsTargetedRequest(const DataSocket* ds) const {
  RTC_DCHECK(ds);
  // Regardless of GET or POST, we look for the peer_id parameter
  // only in the request_path.
  const std::string& path = ds->request_path();
  size_t args = path.find('?');
  if (args == std::string::npos)
    return nullptr;
  size_t found;
  static constexpr absl::string_view kTargetPeerIdParam = "to=";
  do {
    found = path.find(kTargetPeerIdParam, args);
    if (found == std::string::npos)
      return nullptr;
    if (found == (args + 1) || path[found - 1] == '&') {
      found += kTargetPeerIdParam.size();
      break;
    }
    args = found + kTargetPeerIdParam.size();
  } while (true);
  int id = atoi(&path[found]);
  Members::const_iterator i = members_.begin();
  for (; i != members_.end(); ++i) {
    if ((*i)->id() == id) {
      return *i;
    }
  }
  return nullptr;
}

bool PeerChannel::AddMember(DataSocket* ds) {
  RTC_DCHECK(IsPeerConnection(ds));
  ChannelMember* new_guy = new ChannelMember(ds);
  Members failures;
  BroadcastChangedState(*new_guy, &failures);
  HandleDeliveryFailures(&failures);
  members_.push_back(new_guy);

  printf("New member added (total=%zu): %s\n", members_.size(),
         new_guy->name().c_str());

  // Let the newly connected peer know about other members of the channel.
  std::string content_type;
  std::string response = BuildResponseForNewMember(*new_guy, &content_type);
  ds->Send("200 Added", true, content_type, new_guy->GetPeerIdHeader(),
           response);
  return true;
}

void PeerChannel::RemoveMember(ChannelMember* member) {
  if (!member) return;
  // Clear partner's call state if this member was in a call.
  if (member->in_call()) {
    int partner_id = member->call_partner_id();
    for (auto* m : members_) {
      if (m->id() == partner_id) { m->set_call_partner(0); break; }
    }
  }
  member->set_disconnected();
  Members::iterator i = std::find(members_.begin(), members_.end(), member);
  if (i != members_.end()) {
    members_.erase(i);
    Members failures;
    BroadcastChangedState(*member, &failures);
    HandleDeliveryFailures(&failures);
  }
  delete member;
}

std::string PeerChannel::BuildPeerList(const ChannelMember* exclude,
                                       std::string* content_type) {
  *content_type = "text/plain";
  std::string response;
  for (Members::iterator i = members_.begin(); i != members_.end(); ++i) {
    if (exclude && exclude->id() == (*i)->id()) continue;
    if ((*i)->connected())
      response += (*i)->GetEntry();
  }
  return response;
}

void PeerChannel::HandleHangUp(ChannelMember* member) {
  if (!member || !member->in_call()) return;
  int partner_id = member->call_partner_id();
  ChannelMember* partner = nullptr;
  for (auto* m : members_) {
    if (m->id() == partner_id) { partner = m; break; }
  }

  // Phase 1: Notify both parties.  The initiator is auto-confirmed;
  // call state is NOT cleared yet — that happens in HandleHangUpConfirm
  // after both parties have acknowledged.
  member->hangup_received_ = true;  // initiator auto-confirmed

  printf("HangUp: %s(%d) → %s(%d) — waiting for confirmations\n",
         member->name().c_str(), member->id(),
         partner ? partner->name().c_str() : "?", partner_id);

  std::string confirm_msg = "HANGUP_CONFIRM";
  member->QueueResponse("200 OK", "text/plain",
                        kPeerIdHeader + std::to_string(partner_id) + "\r\n",
                        confirm_msg);
  if (partner) {
    partner->QueueResponse("200 OK", "text/plain",
                           kPeerIdHeader + std::to_string(member->id()) + "\r\n",
                           confirm_msg);
  }
}

void PeerChannel::HandleHangUpConfirm(ChannelMember* member) {
  if (!member || !member->in_call()) return;
  int partner_id = member->call_partner_id();
  ChannelMember* partner = nullptr;
  for (auto* m : members_) {
    if (m->id() == partner_id) { partner = m; break; }
  }

  member->hangup_received_ = true;

  // If the partner has also confirmed (or was the initiator, auto-confirmed),
  // we can finalize the hangup.
  if (partner && partner->hangup_received_) {
    member->set_call_partner(0);
    partner->set_call_partner(0);
    member->hangup_received_ = false;
    partner->hangup_received_ = false;

    printf("HangUp: %s(%d) and %s(%d) call cleared (both confirmed)\n",
           member->name().c_str(), member->id(),
           partner->name().c_str(), partner_id);

    // Broadcast updated peer list to everyone
    for (auto* m : members_) {
      std::string ct;
      std::string pl = BuildPeerList(nullptr, &ct);
      m->QueueResponse("200 OK", ct, m->GetPeerIdHeader(), pl);
    }
  } else {
    printf("HangUp: %s(%d) confirmed, waiting for %s(%d)\n",
           member->name().c_str(), member->id(),
           partner ? partner->name().c_str() : "?", partner_id);
  }
}

bool PeerChannel::IsPeerBusy(int peer_id) const {
  for (auto* m : members_) {
    if (m->id() == peer_id) return m->in_call();
  }
  return false;
}

bool PeerChannel::SetCallPair(int caller_id, int target_id) {
  ChannelMember *caller = nullptr, *target = nullptr;
  for (auto* m : members_) {
    if (m->id() == caller_id) caller = m;
    if (m->id() == target_id) target = m;
  }
  if (!caller || !target) return false;
  caller->set_call_partner(target_id);
  target->set_call_partner(caller_id);
  return true;
}

void PeerChannel::CloseAll() {
  Members::const_iterator i = members_.begin();
  for (; i != members_.end(); ++i) {
    (*i)->QueueResponse("200 OK", "text/plain", "", "Server shutting down");
  }
  DeleteAll();
}

void PeerChannel::OnClosing(DataSocket* ds) {
  for (Members::iterator i = members_.begin(); i != members_.end(); ++i) {
    ChannelMember* m = (*i);
    m->OnClosing(ds);
    if (!m->connected()) {
      i = members_.erase(i);
      Members failures;
      BroadcastChangedState(*m, &failures);
      HandleDeliveryFailures(&failures);
      delete m;
      if (i == members_.end())
        break;
    }
  }
  printf("Total connected: %zu\n", members_.size());
}

void PeerChannel::CheckForTimeout() {
  for (Members::iterator i = members_.begin(); i != members_.end(); ++i) {
    ChannelMember* m = (*i);
    if (m->TimedOut()) {
      printf("Timeout: %s\n", m->name().c_str());
      m->set_disconnected();
      i = members_.erase(i);
      Members failures;
      BroadcastChangedState(*m, &failures);
      HandleDeliveryFailures(&failures);
      delete m;
      if (i == members_.end())
        break;
    }
  }
}

void PeerChannel::DeleteAll() {
  for (Members::iterator i = members_.begin(); i != members_.end(); ++i)
    delete (*i);
  members_.clear();
}

void PeerChannel::BroadcastChangedState(const ChannelMember& member,
                                        Members* delivery_failures) {
  // This function should be called prior to DataSocket::Close().
  RTC_DCHECK(delivery_failures);

  if (!member.connected()) {
    printf("Member disconnected: %s\n", member.name().c_str());
  }

  Members::iterator i = members_.begin();
  for (; i != members_.end(); ++i) {
    if (&member != (*i)) {
      if (!(*i)->NotifyOfOtherMember(member)) {
        (*i)->set_disconnected();
        delivery_failures->push_back(*i);
        i = members_.erase(i);
        if (i == members_.end())
          break;
      }
    }
  }
}

void PeerChannel::HandleDeliveryFailures(Members* failures) {
  RTC_DCHECK(failures);

  while (!failures->empty()) {
    Members::iterator i = failures->begin();
    ChannelMember* member = *i;
    RTC_DCHECK(!member->connected());
    failures->erase(i);
    BroadcastChangedState(*member, failures);
    delete member;
  }
}

// Builds a simple list of "name,id\n" entries for each member.
std::string PeerChannel::BuildResponseForNewMember(const ChannelMember& member,
                                                   std::string* content_type) {
  RTC_DCHECK(content_type);

  *content_type = "text/plain";
  // The peer itself will always be the first entry.
  std::string response(member.GetEntry());
  for (Members::iterator i = members_.begin(); i != members_.end(); ++i) {
    if (member.id() != (*i)->id()) {
      if ((*i)->connected())
        response += (*i)->GetEntry();
    }
  }

  return response;
}
