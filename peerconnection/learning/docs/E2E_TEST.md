# learning 端到端测试指南

`learning` 是独立的 WebRTC DataChannel 最小工程，拆成两个二进制，两者互相对测：

| 二进制 | 角色 | 默认 socket |
|---|---|---|
| `learning_offer` | offerer | `/tmp/webrtc_runtime/offer.sock` |
| `learning_answer` | answerer | `/tmp/webrtc_runtime/answer.sock` |

**重要**：SDP 和 ICE 候选的交换是**手动**完成的——测试者用 `socat` 分别连接 offerer 和 answerer，在两个终端之间**手动复制粘贴** SDP 和候选，从而走完整个信令流程。这不是自动建连。

socket 路径可用 `--socket_path` 覆盖。

## 1. 协议（两端一致）

| 阶段 | 方向 | 结束标记 |
|---|---|---|
| SDP（offer / answer） | 双向 | **空行** |
| ICE 候选 | 双向 | `done` |
| 聊天挂断 | 双向 | `/quit`（主动）、`/bye`（通知对端） |

候选格式：`mline_index|mid|candidate`（一行一条）。SDP 和候选都是**裸文本**，无额外 `---` 分隔线。

## 2. 构建

```bash
cd /home/light/webrtc/checkout/src/apps/peerconnection/learning
bash build_x64.sh
# 产出：
#   /home/light/webrtc/checkout/src/out/learning_x64/learning_offer
#   /home/light/webrtc/checkout/src/out/learning_x64/learning_answer
```

## 3. 手动端到端测试（4 个终端）

### 3.1 启动两个 daemon

```bash
SRC=/home/light/webrtc/checkout/src
mkdir -p /tmp/webrtc_runtime

# 终端 O：offerer
"$SRC/out/learning_x64/learning_offer" --socket_path=/tmp/webrtc_runtime/offer.sock

# 终端 A：answerer
"$SRC/out/learning_x64/learning_answer" --socket_path=/tmp/webrtc_runtime/answer.sock
```

### 3.2 连接两个 socat

```bash
# 终端 OC：连 offerer
socat - UNIX-CONNECT:/tmp/webrtc_runtime/offer.sock

# 终端 AC：连 answerer
socat - UNIX-CONNECT:/tmp/webrtc_runtime/answer.sock
```

终端 OC 连上后会立刻收到 offer SDP（`v=0 …` 多行，末尾空行）。

### 3.3 手动交换 SDP

1. 复制终端 OC 的整段 offer SDP（`v=0` 开头的行）。
2. 粘贴到终端 AC，**再按一次回车**（空行结束）。
3. 终端 AC 随后会打印 answer SDP，复制它。
4. 粘贴到终端 OC，**再按一次回车**（空行结束）。

### 3.4 手动交换 ICE 候选

1. 终端 OC 收到本地候选（`0|…|candidate:…` 形式），最后一行是 `done`。
2. 把候选行复制到终端 AC，最后输入 `done`。
3. 终端 AC 随后打印它的本地候选，复制到终端 OC，最后输入 `done`。

### 3.5 聊天与挂断

1. 两端都打印 `DataChannel OPEN — ready to chat!` 后，在任意终端输入一行文本回车，对端打印 `Peer: <文本>`。
2. 挂断：输入 `/quit`，对端打印 `Peer hung up.`，两边干净退出。

## 4. 期望结果

- 两个 daemon 的 stderr 日志依次出现 `SetLocalDesc OK`、`SetRemoteDescription OK`、`Sent N local candidates`、`PeerConnectionState: connected`、`>>> CONNECTION ESTABLISHED <<<`、`DataChannel state: open`。
- 双方能互发文本消息。
- `/quit` 后两进程退出，socket 文件被 unlink：

  ```bash
  ls /tmp/webrtc_runtime/offer.sock /tmp/webrtc_runtime/answer.sock 2>&1
  # 应报 No such file or directory
  ```
