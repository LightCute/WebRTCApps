// cli_runner.h
#ifndef APPS_PEERCONNECTION_CLIENT_CLI_RUNNER_H_
#define APPS_PEERCONNECTION_CLIENT_CLI_RUNNER_H_

#include <atomic>
#include <string>

class WebRTCEngine;

class CliRunner {
 public:
  CliRunner(WebRTCEngine* engine,
            const std::string& server,
            int port,
            bool autoconnect,
            bool autocall);
  ~CliRunner();

  void Run();
  void Stop();

  static void PrintPrompt();

 private:
  void InputLoop();
  void HandleInput(const std::string& line);

  WebRTCEngine* engine_;
  std::string server_;
  int port_;
  bool autoconnect_;
  bool autocall_;
  std::atomic<bool> running_{false};
};

#endif
