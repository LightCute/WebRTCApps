#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "apps/peerconnection/client/unix_socket_server.h"
#include "apps/peerconnection/test/EngineController/fake_engine_controller.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

// ---- helpers ----

static std::string temp_socket_path() {
  char tmpl[] = "/tmp/unixsocket_test_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd >= 0) {
    close(fd);
    unlink(tmpl);
  }
  return tmpl;
}

static int connect_test_client(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int timed_read(int fd, char* buf, size_t len, int timeout_ms) {
  struct pollfd pfd = {};
  pfd.fd = fd;
  pfd.events = POLLIN;
  int ret = poll(&pfd, 1, timeout_ms);
  if (ret <= 0) return ret;  // timeout or error
  return read(fd, buf, len);
}

static std::string send_and_recv(int fd, const std::string& json) {
  std::string line = json + "\n";
  ssize_t nw = write(fd, line.c_str(), line.size());
  if (nw <= 0) return "";

  char buf[4096];
  ssize_t nr = timed_read(fd, buf, sizeof(buf) - 1, 2000);
  if (nr <= 0) return "";
  buf[nr] = '\0';

  // Strip trailing newline
  std::string result(buf, nr);
  if (!result.empty() && result.back() == '\n')
    result.pop_back();
  return result;
}

static void wait_a_bit() { usleep(100000); }  // 100ms

// ---- assertions ----

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name)                                \
  do {                                            \
    tests_run++;                                  \
    std::cout << "  " << name << " ... ";         \
  } while (0)

#define PASS()                                    \
  do {                                            \
    std::cout << "PASS" << std::endl;             \
  } while (0)

#define FAIL(reason)                                                      \
  do {                                                                    \
    tests_failed++;                                                       \
    std::cout << "FAIL (" << __LINE__ << "): " << reason << std::endl;   \
  } while (0)

#define CHECK(cond, reason) \
  do {                      \
    if (!(cond))            \
      FAIL(reason);         \
    else                    \
      PASS();               \
  } while (0)

// ---- test cases ----

void test_connect_command() {
  TEST("connect command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect_test_client");

  {
    std::string resp = send_and_recv(
        fd, R"({"id":1,"cmd":"connect","params":{"server":"10.0.1.2","port":7777}})");
    CHECK(!resp.empty(), "should get response");
    CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
    CHECK(fake.calls.size() == 1, "one call recorded");
    CHECK(fake.calls[0].method == "ConnectToServer", "method");
    CHECK(fake.calls[0].arg_str == "10.0.1.2", "arg_str");
    CHECK(fake.calls[0].arg_int == 7777, "arg_int");
  }

  // Shutdown via command
  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_connect_missing_params() {
  TEST("connect missing params");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(fd, R"({"id":2,"cmd":"connect","params":{}})");
  CHECK(resp.find(R"("ok":false)") != std::string::npos, "should be false");
  CHECK(resp.find("Missing") != std::string::npos, "should say Missing");
  CHECK(fake.calls.empty(), "no engine calls");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_disconnect_command() {
  TEST("disconnect command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(fd, R"({"id":3,"cmd":"disconnect"})");
  CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
  CHECK(fake.calls.size() == 1, "one call");
  CHECK(fake.calls[0].method == "DisconnectFromServer", "method");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_call_command() {
  TEST("call command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(fd, R"({"id":4,"cmd":"call","params":{"peer_id":42}})");
  CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
  CHECK(fake.calls[0].method == "ConnectToPeer", "method");
  CHECK(fake.calls[0].arg_int == 42, "arg_int");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_call_bad_peer_id() {
  TEST("call with invalid peer_id");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(fd, R"({"id":5,"cmd":"call","params":{"peer_id":-1}})");
  CHECK(resp.find(R"("ok":false)") != std::string::npos, "should be false");
  CHECK(resp.find("Invalid") != std::string::npos, "should say Invalid");
  CHECK(fake.calls.empty(), "no engine calls");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_hangup_command() {
  TEST("hangup command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(fd, R"({"id":6,"cmd":"hangup"})");
  CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
  CHECK(fake.calls[0].method == "HangUp", "method");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_mute_command() {
  TEST("set_mute command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(
      fd, R"({"id":7,"cmd":"set_mute","params":{"audio":true,"video":true}})");
  CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
  CHECK(fake.calls.size() == 2, "two calls");
  CHECK(fake.calls[0].method == "SetAudioMuted" && fake.calls[0].arg_bool == true,
        "audio muted");
  CHECK(fake.calls[1].method == "SetVideoPaused" && fake.calls[1].arg_bool == true,
        "video paused");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_send_data_command() {
  TEST("send_data command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(
      fd, R"({"id":8,"cmd":"send_data","params":{"text":"hello world"}})");
  CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
  CHECK(fake.calls[0].method == "SendData", "method");
  CHECK(fake.calls[0].arg_str == "hello world", "arg_str");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_query_devices_command() {
  TEST("query_devices command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(fd, R"({"id":9,"cmd":"query_devices"})");
  CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
  CHECK(fake.calls[0].method == "QueryDevices", "method");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_set_video_device_command() {
  TEST("set_video_device command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(
      fd, R"({"id":10,"cmd":"set_video_device","params":{"device_idx":3}})");
  CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
  CHECK(fake.calls[0].method == "SetVideoDevice", "method");
  CHECK(fake.calls[0].arg_int == 3, "arg_int");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_set_audio_input_device_command() {
  TEST("set_audio_input_device command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(
      fd, R"({"id":11,"cmd":"set_audio_input_device","params":{"device_idx":1}})");
  CHECK(resp.find(R"("ok":true)") != std::string::npos, "should be ok");
  CHECK(fake.calls[0].method == "SetAudioInputDevice", "method");
  CHECK(fake.calls[0].arg_int == 1, "arg_int");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_unknown_command() {
  TEST("unknown command");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  std::string resp = send_and_recv(fd, R"({"id":12,"cmd":"bogus"})");
  CHECK(resp.find(R"("ok":false)") != std::string::npos, "should be false");
  CHECK(resp.find("Unknown") != std::string::npos, "should say Unknown");
  CHECK(fake.calls.empty(), "no engine calls");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_invalid_json() {
  TEST("invalid JSON does not crash");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  // Send garbage; server should not crash. No response expected.
  std::string line = "this is not json\n";
  write(fd, line.c_str(), line.size());
  wait_a_bit();
  CHECK(fake.calls.empty(), "no engine calls");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

void test_on_engine_event_forwarding() {
  TEST("OnEngineEvent forwards to client");
  std::string path = temp_socket_path();
  FakeEngineController fake;

  UnixSocketServer server(path, &fake);
  fake.RegisterObserver(&server);  // wire observer so EmitEvent → SendToClient
  server.Start();
  wait_a_bit();

  int fd = connect_test_client(path);
  CHECK(fd >= 0, "connect");

  // Sync: send a dummy command and wait for response to ensure the IO thread
  // has accepted the connection and client_fd_ is set.
  std::string sync = send_and_recv(fd, R"({"id":0,"cmd":"disconnect"})");
  CHECK(!sync.empty(), "sync response received");

  // Now fire an event — client_fd_ is guaranteed set
  fake.EmitEvent(R"({"event":"peer_online","peer":{"id":10,"name":"alice"}})");

  // Read what arrived on the client socket (with timeout)
  char buf[4096];
  ssize_t nr = timed_read(fd, buf, sizeof(buf) - 1, 2000);
  CHECK(nr > 0, "client received data");
  buf[nr] = '\0';
  std::string event(buf, nr);
  CHECK(event.find("peer_online") != std::string::npos, "contains peer_online");
  CHECK(event.find("alice") != std::string::npos, "contains alice");
  CHECK(event.find("10") != std::string::npos, "contains id 10");

  send_and_recv(fd, R"({"id":99,"cmd":"shutdown"})");
  close(fd);
  server.Stop();
  unlink(path.c_str());
}

// ---- main ----

int main() {
  std::cout << "=== UnixSocketServer Tests ===" << std::endl;

  test_connect_command();
  test_connect_missing_params();
  test_disconnect_command();
  test_call_command();
  test_call_bad_peer_id();
  test_hangup_command();
  test_mute_command();
  test_send_data_command();
  test_query_devices_command();
  test_set_video_device_command();
  test_set_audio_input_device_command();
  test_unknown_command();
  test_invalid_json();
  test_on_engine_event_forwarding();

  std::cout << std::endl
            << tests_run << " tests, " << tests_failed << " failed"
            << std::endl;
  return tests_failed ? 1 : 0;
}

#pragma GCC diagnostic pop
