#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "apps/peerconnection/learning/learning_common.h"

ABSL_FLAG(std::string, socket_path, "/tmp/webrtc_runtime/answer.sock",
          "Unix domain socket path for the answerer daemon to listen on");

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  return RunSession(Role::kAnswerer, absl::GetFlag(FLAGS_socket_path));
}
