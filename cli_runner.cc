// cli_runner.cc
#include "cli_runner.h"

#include <iostream>
#include <sstream>

#include "apps/peerconnection/client/webrtc_engine.h"
#include "rtc_base/logging.h"

CliRunner::CliRunner(WebRTCEngine* engine,
                     const std::string& server,
                     int port,
                     bool autoconnect,
                     bool autocall)
    : engine_(engine),
      server_(server),
      port_(port),
      autoconnect_(autoconnect),
      autocall_(autocall) {}

CliRunner::~CliRunner() { Stop(); }

void CliRunner::Run() {
  // Wire engine events to stdout; also handle auto-call
  engine_->SetEventCallback([this](const std::string& json) {
    std::cout << "[event] " << json << std::endl;
    PrintPrompt();
    // Auto-call: when a peer appears and autocall is set, call the first peer
    if (autocall_ && !engine_->connection_active()) {
      // Look for "peer_online" event to extract peer_id
      auto pos = json.find("\"peer_online\"");
      if (pos != std::string::npos) {
        auto id_pos = json.find("\"id\":");
        if (id_pos != std::string::npos) {
          int id = std::stoi(json.substr(id_pos + 5));
          std::cout << "Auto-calling peer " << id << "..." << std::endl;
          engine_->ConnectToPeer(id);
          autocall_ = false;  // only auto-call once
        }
      }
    }
  });

  std::cout << "=== WebRTC CLI (standalone mode) ===" << std::endl;
  std::cout << "Commands: connect, disconnect, call <id>, hangup, mute, unmute, pause, resume, send <text>, quit" << std::endl;

  // Auto-connect if requested
  if (autoconnect_) {
    engine_->ConnectToServer(server_, port_);
  }

  running_ = true;
  InputLoop();
}

void CliRunner::Stop() { running_ = false; }

void CliRunner::PrintPrompt() {
  std::cout << "> " << std::flush;
}

void CliRunner::InputLoop() {
  PrintPrompt();
  std::string line;
  while (running_ && std::getline(std::cin, line)) {
    HandleInput(line);
    if (running_) PrintPrompt();
  }
}

void CliRunner::HandleInput(const std::string& line) {
  if (line.empty()) return;

  std::istringstream iss(line);
  std::string cmd;
  iss >> cmd;

  if (cmd == "connect") {
    engine_->ConnectToServer(server_, port_);
  } else if (cmd == "disconnect") {
    engine_->DisconnectFromServer();
  } else if (cmd == "call") {
    int peer_id;
    if (iss >> peer_id) {
      engine_->ConnectToPeer(peer_id);
    } else {
      std::cout << "Usage: call <peer_id>" << std::endl;
    }
  } else if (cmd == "hangup") {
    engine_->HangUp();
  } else if (cmd == "mute") {
    engine_->SetAudioMuted(true);
  } else if (cmd == "unmute") {
    engine_->SetAudioMuted(false);
  } else if (cmd == "pause") {
    engine_->SetVideoPaused(true);
  } else if (cmd == "resume") {
    engine_->SetVideoPaused(false);
  } else if (cmd == "send") {
    std::string text;
    std::getline(iss, text);
    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
    if (!text.empty()) engine_->SendData(text);
  } else if (cmd == "quit" || cmd == "exit") {
    running_ = false;
  } else {
    std::cout << "Unknown command: " << cmd << std::endl;
  }
}
