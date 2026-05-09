#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "apps/peerconnection/client/cli_runner.h"
#include "apps/peerconnection/test/EngineController/fake_engine_controller.h"

// ---- stdin/stdout redirection helpers ----

struct IORedirect {
  int stdin_pipe[2];
  int stdout_pipe[2];
  int saved_stdin;
  int saved_stdout;
  FILE* captured_out;

  IORedirect() : captured_out(nullptr) {
    saved_stdin = dup(STDIN_FILENO);
    saved_stdout = dup(STDOUT_FILENO);
    pipe(stdin_pipe);
    pipe(stdout_pipe);
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    captured_out = fdopen(stdout_pipe[0], "r");
    setvbuf(captured_out, nullptr, _IONBF, 0);
  }

  ~IORedirect() {
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    if (captured_out) fclose(captured_out);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
  }

  void write_cmd(const std::string& s) {
    std::string line = s + "\n";
    write(stdin_pipe[1], line.c_str(), line.size());
  }

  void close_stdin() { close(stdin_pipe[1]); stdin_pipe[1] = -1; }

  std::string read_out() {
    char buf[4096];
    ssize_t n = ::read(stdout_pipe[0], buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return std::string(buf, n); }
    return "";
  }
};

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

// Helper: run CliRunner::InputLoop in a background thread so we can feed
// commands and check results. The CliRunner must NOT be destroyed until the
// thread exits.
struct CliTest {
  FakeEngineController fake;
  CliRunner cli;
  IORedirect io;
  std::thread runner;

  CliTest(const std::string& server, int port, bool autoconnect, bool autocall)
      : cli(&fake, server, port, autoconnect, autocall) {}

  // Feed commands then wait for thread to finish
  void feed_and_wait(const std::vector<std::string>& commands) {
    for (auto& cmd : commands)
      io.write_cmd(cmd);
    io.close_stdin();
    if (runner.joinable())
      runner.join();
  }

  // Start the runner thread
  void start() {
    runner = std::thread([this] { cli.Run(); });
  }
};

void test_connect_command() {
  TEST("connect command");
  CliTest t("10.0.1.2", 7777, false, false);
  t.start();
  t.feed_and_wait({"connect", "quit"});
  CHECK(t.fake.calls.size() >= 2, "at least 2 calls (RegisterObserver + ConnectToServer)");
  CHECK(t.fake.calls[0].method == "RegisterObserver", "first call is RegisterObserver");
  CHECK(t.fake.calls[1].method == "ConnectToServer", "ConnectToServer");
  CHECK(t.fake.calls[1].arg_str == "10.0.1.2", "server arg");
  CHECK(t.fake.calls[1].arg_int == 7777, "port arg");
}

void test_disconnect_command() {
  TEST("disconnect command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"disconnect", "quit"});
  CHECK(t.fake.calls.size() >= 2, "at least 2 calls");
  CHECK(t.fake.calls[1].method == "DisconnectFromServer", "DisconnectFromServer");
}

void test_call_command() {
  TEST("call command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"call 42", "quit"});
  CHECK(t.fake.calls.size() >= 2, "at least 2 calls");
  CHECK(t.fake.calls[1].method == "ConnectToPeer", "ConnectToPeer");
  CHECK(t.fake.calls[1].arg_int == 42, "peer_id = 42");
}

void test_call_no_arg() {
  TEST("call with no peer_id");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"call", "quit"});
  // Should print usage, not call engine
  bool has_usage = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "ConnectToPeer") has_usage = true;
  }
  CHECK(!has_usage, "ConnectToPeer not called");
  std::string out = t.io.read_out();
  CHECK(out.find("Usage: call <peer_id>") != std::string::npos, "usage message printed");
}

void test_hangup_command() {
  TEST("hangup command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"hangup", "quit"});
  bool found = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "HangUp") { found = true; break; }
  }
  CHECK(found, "HangUp called");
}

void test_mute_command() {
  TEST("mute command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"mute", "quit"});
  bool found = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "SetAudioMuted" && c.arg_bool == true) { found = true; break; }
  }
  CHECK(found, "SetAudioMuted(true) called");
}

void test_unmute_command() {
  TEST("unmute command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"unmute", "quit"});
  bool found = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "SetAudioMuted" && c.arg_bool == false) { found = true; break; }
  }
  CHECK(found, "SetAudioMuted(false) called");
}

void test_pause_command() {
  TEST("pause command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"pause", "quit"});
  bool found = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "SetVideoPaused" && c.arg_bool == true) { found = true; break; }
  }
  CHECK(found, "SetVideoPaused(true) called");
}

void test_resume_command() {
  TEST("resume command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"resume", "quit"});
  bool found = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "SetVideoPaused" && c.arg_bool == false) { found = true; break; }
  }
  CHECK(found, "SetVideoPaused(false) called");
}

void test_send_command() {
  TEST("send command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"send hello world", "quit"});
  bool found = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "SendData" && c.arg_str == "hello world") { found = true; break; }
  }
  CHECK(found, "SendData('hello world') called");
}

void test_send_empty() {
  TEST("send with no text");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"send", "quit"});
  bool found = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "SendData") { found = true; break; }
  }
  CHECK(!found, "SendData not called for empty text");
}

void test_unknown_command() {
  TEST("unknown command");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"bogus_cmd", "quit"});
  std::string out = t.io.read_out();
  CHECK(out.find("Unknown command: bogus_cmd") != std::string::npos, "unknown command message");
}

void test_empty_line() {
  TEST("empty line ignored");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"", "  ", "quit"});
  // Should not crash; only RegisterObserver should be called
  CHECK(t.fake.calls.size() == 1, "only RegisterObserver");
}

void test_quit_command() {
  TEST("quit exits input loop");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"quit"});
  // Input loop should exit cleanly (thread joins successfully)
  CHECK(t.fake.calls.size() == 1, "only RegisterObserver");  // quit itself doesn't call engine
}

void test_exit_command() {
  TEST("exit exits input loop");
  CliTest t("localhost", 8888, false, false);
  t.start();
  t.feed_and_wait({"exit"});
  CHECK(t.fake.calls.size() == 1, "only RegisterObserver");
}

void test_autoconnect() {
  TEST("autoconnect calls ConnectToServer on start");
  CliTest t("myhost", 9999, true, false);
  t.start();
  t.feed_and_wait({"quit"});
  bool found = false;
  for (auto& c : t.fake.calls) {
    if (c.method == "ConnectToServer" && c.arg_str == "myhost" && c.arg_int == 9999)
      { found = true; break; }
  }
  CHECK(found, "ConnectToServer called with myhost:9999");
}

void test_on_engine_event() {
  TEST("OnEngineEvent prints event + prompt");
  FakeEngineController fake;
  IORedirect io;
  CliRunner cli(&fake, "localhost", 8888, false, false);

  cli.OnEngineEvent(R"({"event":"test_event","data":123})");
  std::string out = io.read_out();
  CHECK(out.find("[event]") != std::string::npos, "has [event] prefix");
  CHECK(out.find("test_event") != std::string::npos, "contains test_event");
  CHECK(out.find("> ") != std::string::npos, "prints prompt after event");
}

void test_autocall_on_event() {
  TEST("autocall triggers ConnectToPeer on peer_online event");
  FakeEngineController fake;
  IORedirect io;
  CliRunner cli(&fake, "localhost", 8888, false, true);  // autocall = true

  fake.active = false;  // connection_active() returns false
  cli.OnEngineEvent(R"({"event":"peer_online","peer":{"id":55,"name":"bob"}})");

  bool found = false;
  for (auto& c : fake.calls) {
    if (c.method == "ConnectToPeer" && c.arg_int == 55) { found = true; break; }
  }
  CHECK(found, "ConnectToPeer(55) called via auto-call");
}

void test_autocall_only_once() {
  TEST("autocall fires only once");
  FakeEngineController fake;
  IORedirect io;
  CliRunner cli(&fake, "localhost", 8888, false, true);

  fake.active = false;
  cli.OnEngineEvent(R"({"event":"peer_online","peer":{"id":1,"name":"a"}})");
  cli.OnEngineEvent(R"({"event":"peer_online","peer":{"id":2,"name":"b"}})");

  // Count ConnectToPeer calls
  int count = 0;
  for (auto& c : fake.calls) {
    if (c.method == "ConnectToPeer") count++;
  }
  CHECK(count == 1, "only one auto-call");
}

void test_stop_stops_loop() {
  TEST("Stop() exits InputLoop");
  CliTest t("localhost", 8888, false, false);
  t.start();
  // Don't feed quit; use Stop() from outside instead
  usleep(50000);  // let thread start
  t.cli.Stop();
  t.io.close_stdin();
  if (t.runner.joinable())
    t.runner.join();
  CHECK(true, "thread joined without deadlock");
}

// ---- main ----

int main() {
  std::cout << "=== CliRunner Tests ===" << std::endl;

  test_connect_command();
  test_disconnect_command();
  test_call_command();
  test_call_no_arg();
  test_hangup_command();
  test_mute_command();
  test_unmute_command();
  test_pause_command();
  test_resume_command();
  test_send_command();
  test_send_empty();
  test_unknown_command();
  test_empty_line();
  test_quit_command();
  test_exit_command();
  test_autoconnect();
  test_on_engine_event();
  test_autocall_on_event();
  test_autocall_only_once();
  test_stop_stops_loop();

  std::cout << std::endl
            << tests_run << " tests, " << tests_failed << " failed"
            << std::endl;
  return tests_failed ? 1 : 0;
}
