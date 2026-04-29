#include "shm_reader.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

// 与 peerconnection_client 的 main_cli.h 保持一致
#define LOCAL_SHM_KEY   "/tmp/webrtc_local"
#define REMOTE_SHM_KEY  "/tmp/webrtc_remote"
#define LOCAL_SHM_ID    888
#define REMOTE_SHM_ID   999

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_local_done{false};
static std::atomic<bool> g_remote_done{false};

void sigint_handler(int) { g_running = false; }

void read_loop(const char* name, const char* key_path, int proj_id,
               std::atomic<bool>& done_flag) {
  ShmVideoReader reader;
  if (!reader.init(key_path, proj_id)) {
    fprintf(stderr, "[%s] 未能连接到共享内存 %s (id=%d)，等待发送端创建...\n",
            name, key_path, proj_id);
    return;
  }
  printf("[%s] 已连接共享内存: %s (id=%d)\n", name, key_path, proj_id);

  std::vector<uint8_t> buffer(FRAME_MAX_SIZE);
  uint64_t frame_idx = 0;
  uint64_t last_frame_idx = 0;
  uint64_t total_bytes = 0;
  uint64_t last_total_bytes = 0;
  auto last_report = std::chrono::steady_clock::now();

  // 可选：将帧写入文件（ffplay 可播放）
  FILE* dump_file = nullptr;
  const char* dump_path = getenv("SHM_DUMP_FILE");
  if (dump_path) {
    dump_file = fopen(dump_path, "wb");
    if (dump_file) printf("[%s] 正在将帧写入: %s\n", name, dump_path);
  }

  while (g_running) {
    VideoFrameHead head{};
    if (!reader.read_frame(head, buffer.data(), FRAME_MAX_SIZE)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    frame_idx++;
    total_bytes += head.frame_len;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       now - last_report).count();
    if (elapsed >= 2) {
      double fps = static_cast<double>(frame_idx - last_frame_idx) / elapsed;
      double mbps = (total_bytes - last_total_bytes) / elapsed / 1024.0 / 1024.0;
      printf("[%s] 帧 #%lu  %dx%d  %u bytes  ts=%.3fs  "
             "avg %.1f fps  %.1f MB/s\n",
             name, frame_idx,
             head.width, head.height, head.frame_len,
             head.timestamp / 1e6,
             fps, mbps);
      last_report = now;
      last_frame_idx = frame_idx;
      last_total_bytes = total_bytes;
    }

    if (dump_file) {
      fwrite(buffer.data(), 1, head.frame_len, dump_file);
      fflush(dump_file);
    }
  }

  if (dump_file) fclose(dump_file);
  printf("[%s] 已停止，共读取 %lu 帧\n", name, frame_idx);
  done_flag = true;
}

int main(int argc, char** argv) {
  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  const char* mode = "both";
  if (argc > 1) mode = argv[1];

  printf("===== 共享内存视频读取器 =====\n");
  printf("用法: %s [local|remote|both]\n", argv[0]);
  printf("设置 SHM_DUMP_FILE 环境变量可将帧写入文件:\n");
  printf("  SHM_DUMP_FILE=output.i420 %s\n\n", argv[0]);

  std::thread local_thread, remote_thread;

  if (strcmp(mode, "local") == 0 || strcmp(mode, "both") == 0) {
    local_thread = std::thread(read_loop, "LOCAL", LOCAL_SHM_KEY, LOCAL_SHM_ID,
                               std::ref(g_local_done));
  }
  if (strcmp(mode, "remote") == 0 || strcmp(mode, "both") == 0) {
    remote_thread = std::thread(read_loop, "REMOTE", REMOTE_SHM_KEY, REMOTE_SHM_ID,
                                std::ref(g_remote_done));
  }

  // 等待 Ctrl+C 信号
  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // 收到信号后，给 reader 线程最多 3 秒排空 ring buffer 并退出
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while ((local_thread.joinable() && !g_local_done) ||
         (remote_thread.joinable() && !g_remote_done)) {
    if (std::chrono::steady_clock::now() > deadline) {
      printf("等待 reader 线程超时，强制退出\n");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // join 已完成的线程，僵死线程 detach 交给 OS 清理
  if (g_local_done && local_thread.joinable()) local_thread.join();
  else if (local_thread.joinable()) local_thread.detach();
  if (g_remote_done && remote_thread.joinable()) remote_thread.join();
  else if (remote_thread.joinable()) remote_thread.detach();

  printf("读取器已退出\n");
  return 0;
}
