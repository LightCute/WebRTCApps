/*
 *  Copyright 2011 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "apps/peerconnection/server/data_socket.h"
#include "apps/peerconnection/server/peer_channel.h"
#include "rtc_base/checks.h"

#if defined(WEBRTC_POSIX)
#include <sys/select.h>
#include <sys/stat.h>
#endif

// As of now, no components in peerconnection_server rely on WebRTC components
// that change its behavior based on a field trial, so this flag is currently
// unused. See peerconnection_client for example how this command line flag
// can be used and propagated.
ABSL_FLAG(
    std::string,
    force_fieldtrials,
    "",
    "Field trials control experimental features. This flag specifies the field "
    "trials in effect. E.g. running with "
    "--force_fieldtrials=WebRTC-FooFeature/Enabled/ "
    "will assign the group Enabled to field trial WebRTC-FooFeature. Multiple "
    "trials are separated by \"/\"");
ABSL_FLAG(int, port, 8888, "default: 8888");
ABSL_FLAG(std::string,
          log_file,
          "",
          "Log file path (default: $WEBRTC_RUNTIME_DIR/server.log or /tmp/webrtc_runtime/server.log)");

static const size_t kMaxConnections = (FD_SETSIZE - 2);

void HandleBrowserRequest(DataSocket* ds, bool* quit) {
  RTC_DCHECK(ds && ds->valid());
  RTC_DCHECK(quit);

  const std::string& path = ds->request_path();

  *quit = (path.compare("/quit") == 0);

  if (*quit) {
    ds->Send("200 OK", true, "text/html", "",
             "<html><body>Quitting...</body></html>");
  } else if (ds->method() == DataSocket::OPTIONS) {
    // We'll get this when a browsers do cross-resource-sharing requests.
    // The headers to allow cross-origin script support will be set inside
    // Send.
    ds->Send("200 OK", true, "", "", "");
  } else {
    // Here we could write some useful output back to the browser depending on
    // the path.
    printf("Received an invalid request: %s\n", ds->request_path().c_str());
    ds->Send("500 Sorry", true, "text/html", "",
             "<html><body>Sorry, not yet implemented</body></html>");
  }
}

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Example usage: ./peerconnection_server --port=8888\n");
  absl::ParseCommandLine(argc, argv);

  int port = absl::GetFlag(FLAGS_port);

  // Redirect stdout to log file for persistent logging.
  std::string log_path = absl::GetFlag(FLAGS_log_file);
  if (log_path.empty()) {
    const char* rt = getenv("WEBRTC_RUNTIME_DIR");
    log_path = std::string(rt ? rt : "/tmp/webrtc_runtime") + "/server.log";
  }
  // Ensure parent directories exist (equivalent to mkdir -p).
  {
    std::string dir = log_path;
    size_t sep = dir.rfind('/');
    if (sep != std::string::npos) {
      dir = dir.substr(0, sep);
      std::string cur;
      for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == '/' && !cur.empty())
          mkdir(cur.c_str(), 0755);
      }
      mkdir(cur.c_str(), 0755);
    }
  }
  if (!freopen(log_path.c_str(), "a", stdout)) {
    fprintf(stderr, "Warning: cannot open log file %s, logging to stdout only\n",
            log_path.c_str());
  } else {
    setbuf(stdout, nullptr);  // unbuffered, so logs appear immediately
    printf("=== Signaling server started, logging to %s ===\n", log_path.c_str());
  }

  // Abort if the user specifies a port that is outside the allowed
  // range [1, 65535].
  if ((port < 1) || (port > 65535)) {
    fprintf(stderr, "Error: %i is not a valid port.\n", port);
    return -1;
  }

  ListeningSocket listener;
  if (!listener.Create()) {
    fprintf(stderr, "Failed to create server socket\n");
    return -1;
  } else if (!listener.Listen(port)) {
    fprintf(stderr, "Failed to listen on server socket\n");
    return -1;
  }

  printf("Server listening on port %i\n", port);

  PeerChannel clients;
  typedef std::vector<DataSocket*> SocketArray;
  SocketArray sockets;
  bool quit = false;
  while (!quit) {
    fd_set socket_set;
    FD_ZERO(&socket_set);
    if (listener.valid())
      FD_SET(listener.socket(), &socket_set);

    for (SocketArray::iterator i = sockets.begin(); i != sockets.end(); ++i)
      FD_SET((*i)->socket(), &socket_set);

    struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
    if (select(FD_SETSIZE, &socket_set, nullptr, nullptr, &timeout) ==
        SOCKET_ERROR) {
      printf("select failed\n");
      break;
    }

    for (SocketArray::iterator i = sockets.begin(); i != sockets.end(); ++i) {
      DataSocket* s = *i;
      bool socket_done = true;
      if (FD_ISSET(s->socket(), &socket_set)) {
        if (s->OnDataAvailable(&socket_done) && s->request_received()) {
          printf("Request: %s\n", s->request_path().c_str());
          ChannelMember* member = clients.Lookup(s);
          if (member || PeerChannel::IsPeerConnection(s)) {
            if (!member) {
              if (s->PathEquals("/sign_in")) {
                clients.AddMember(s);
              } else {
                printf("No member found for: %s\n", s->request_path().c_str());
                s->Send("500 Error", true, "text/plain", "",
                        "Peer most likely gone.");
              }
            } else if (member->is_wait_request(s)) {
              // no need to do anything.
              socket_done = false;
            } else {
              ChannelMember* target = clients.IsTargetedRequest(s);
              if (target) {
                member->ForwardRequestToPeer(s, target);
              } else if (s->PathEquals("/hangup")) {
                clients.HandleHangUp(member);
                s->Send("200 OK", true, "text/plain", "", "");
              } else if (s->PathEquals("/hangup_confirm")) {
                printf("Received /hangup_confirm from %s(%d) in_call=%d\n",
                       member->name().c_str(), member->id(), member->in_call());
                clients.HandleHangUpConfirm(member);
                s->Send("200 OK", true, "text/plain", "", "");
              } else if (s->PathEquals("/sign_out")) {
                // Remove member and broadcast updated peer list to others.
                // Send the remaining peer list to the disconnecting client.
                std::string content_type;
                std::string peer_list = clients.BuildPeerList(member, &content_type);
                s->Send("200 OK", true, content_type, "", peer_list);
                clients.RemoveMember(member);
                member = nullptr;
              } else {
                printf("Couldn't find target for request: %s\n",
                       s->request_path().c_str());
                s->Send("500 Error", true, "text/plain", "",
                        "Peer most likely gone.");
              }
            }
          } else {
            HandleBrowserRequest(s, &quit);
            if (quit) {
              printf("Quitting...\n");
              FD_CLR(listener.socket(), &socket_set);
              listener.Close();
              clients.CloseAll();
            }
          }
        }
      } else {
        socket_done = false;
      }

      if (socket_done) {
        printf("Disconnecting socket\n");
        clients.OnClosing(s);
        RTC_DCHECK(s->valid());  // Close must not have been called yet.
        FD_CLR(s->socket(), &socket_set);
        delete (*i);
        i = sockets.erase(i);
        if (i == sockets.end())
          break;
      }
    }

    clients.CheckForTimeout();

    if (FD_ISSET(listener.socket(), &socket_set)) {
      DataSocket* s = listener.Accept();
      if (sockets.size() >= kMaxConnections) {
        delete s;  // sorry, that's all we can take.
        printf("Connection limit reached\n");
      } else {
        sockets.push_back(s);
        printf("New connection...\n");
      }
    }
  }

  for (SocketArray::iterator i = sockets.begin(); i != sockets.end(); ++i)
    delete (*i);
  sockets.clear();

  return 0;
}
