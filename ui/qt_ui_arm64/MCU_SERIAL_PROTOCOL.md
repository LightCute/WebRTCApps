# RK3588 ↔ MCU 串口通信协议 v2

> **v2 变更** (2026-07-04): PC 端事件驱动 + 结构化指令。
> v=0.5 不再硬编码——PC 端调速滑块决定速度系数；MCU 端仅做看门狗安全保护。
> 新增 `move_ptz` 合并指令支持组合键，新增 `ptz_home` 云台归位，新增 `stop` 显式停止。
> 看门狗从 100ms 延长到 300ms。

## 物理层

| 参数 | 值 |
|------|-----|
| 接口 | UART |
| 波特率 | 115200 |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | 无 |
| 流控 | 无 |
| 设备 | RK3588 `/dev/ttyS9` → MCU |

## 帧格式

```
┌──────┬──────┬──────┬────────────────────┬──────┬──────┬──────┐
│ 0xAA │ 0x55 │  LEN  │ JSON payload (LEN)  │ XOR  │ 0x0D │ 0x0A │
│帧头1 │帧头2 │1 byte│    ≤255 bytes       │1 byte│  \r  │  \n  │
└──────┴──────┴──────┴────────────────────┴──────┴──────┴──────┘
```

**校验**：`XOR = LEN ^ payload[0] ^ payload[1] ^ ... ^ payload[LEN-1]`

## RK3588 → MCU 命令 (v2)

### 命令类型

| cmd | 触发条件 | 示例 |
|-----|---------|------|
| `move` | 仅底盘键按下 | `{"cmd":"move","v":0.5,"w":0}` |
| `ptz` | 仅云台键按下 | `{"cmd":"ptz","pan":0,"tilt":0.5}` |
| `move_ptz` | 底盘+云台同时按 | `{"cmd":"move_ptz","v":0.5,"w":0.2,"pan":0,"tilt":0.5}` |
| `ptz_home` | 按 H 键 | `{"cmd":"ptz_home"}` |
| `stop` | 所有键松开 | `{"cmd":"stop"}` |

### 字段说明

| 字段 | 类型 | cmd | 说明 |
|------|------|-----|------|
| cmd | string | 全部 | 命令类型标识 |
| v | float | move, move_ptz | 线速度 -1..1 (正=前进) |
| w | float | move, move_ptz | 角速度 -1..1 (正=左转) |
| pan | float | ptz, move_ptz | 水平转角 -1..1 (正=右) |
| tilt | float | ptz, move_ptz | 垂直转角 -1..1 (正=上) |

### 按键映射

| 按键 | 功能 |
|------|------|
| W / ↑ | 底盘前进 |
| S / ↓ | 底盘后退 |
| A / ← | 底盘左转 |
| D / → | 底盘右转 |
| I | 云台上仰 |
| K | 云台下俯 |
| J | 云台左转 |
| L | 云台右转 |
| H | 云台归位 |

- PC 端调速滑块控制速度系数 (0.1~1.0)，MCU 无需关心
- 组合键 (W+J) → `move_ptz`，同时控制底盘和云台
- 按下立刻发第一条指令，按住期间每 80ms 重发保活
- 松开最后一个键立刻发 `stop`

## MCU 控制逻辑 (v2)

MCU 收到帧后：

1. 解析 JSON，获取 cmd 字段
2. `move` → `set_motors(v, w)`
3. `ptz` → `set_ptz(pan, tilt)`
4. `move_ptz` → `set_motors(v, w)` + `set_ptz(pan, tilt)`
5. `ptz_home` → `servo_home()` (pan/tilt 回中位)
6. `stop` → `set_motors(0, 0)` + `set_ptz(0, 0)`
7. 启动 **300ms 硬件看门狗**
8. 看门狗到期 → 停止底盘+云台

**变更**: 看门狗从 100ms 延长到 **300ms**（PC 端 80ms 保活间隔 × 3 余量，网络抖动容忍度更高）。

```
MCU 伪代码:
  on_frame_received(json):
      cmd = json["cmd"]
      if cmd == "move" or cmd == "move_ptz":
          set_motors(json["v"], json["w"])
      if cmd == "ptz" or cmd == "move_ptz":
          set_ptz(json["pan"], json["tilt"])
      if cmd == "stop":
          set_motors(0, 0)
          set_ptz(0, 0)
      if cmd == "ptz_home":
          servo_home()
      restart_timer(300ms)

  on_timer_expired():
      set_motors(0, 0)
      set_ptz(0, 0)
```

## MCU → RK3588 应答

成功：`{"ack":"ok"}`  
错误：`{"ack":"err","msg":"checksum"}` / `{"ack":"err","msg":"json"}` / `{"ack":"err","msg":"len"}`

## 接收参考实现 (C) — v2

```c
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ── 状态机 (不变) ──
enum { WAIT_H1, WAIT_H2, WAIT_LEN, WAIT_DATA, WAIT_XOR, WAIT_CR, WAIT_LF };
static int  rx_state = WAIT_H1;
static uint8_t rx_buf[256];
static int  rx_pos, rx_len;
static uint8_t rx_xor;

void uart_rx_byte(uint8_t byte) {
    switch (rx_state) {
    case WAIT_H1:
        if (byte == 0xAA) rx_state = WAIT_H2;
        break;
    case WAIT_H2:
        rx_state = (byte == 0x55) ? WAIT_LEN : WAIT_H1;
        break;
    case WAIT_LEN:
        rx_len = byte;
        if (rx_len > 0 && rx_len <= 250) {
            rx_pos = 0; rx_xor = byte; rx_state = WAIT_DATA;
        } else {
            rx_state = WAIT_H1;
        }
        break;
    case WAIT_DATA:
        rx_buf[rx_pos++] = byte;
        rx_xor ^= byte;
        if (rx_pos >= rx_len) rx_state = WAIT_XOR;
        break;
    case WAIT_XOR:
        if (byte == rx_xor) rx_state = WAIT_CR;
        else { reply_err("checksum"); rx_state = WAIT_H1; }
        break;
    case WAIT_CR:
        rx_state = (byte == '\r') ? WAIT_LF : WAIT_H1;
        break;
    case WAIT_LF:
        if (byte == '\n') { rx_buf[rx_len] = '\0'; handle_frame((char*)rx_buf); reply_ok(); }
        rx_state = WAIT_H1;
        break;
    }
}

// ── 帧处理 (v2: cmd 类型驱动) ──
static int motion_timer_running = 0;

// 简单的 JSON 字段提取 (避免引入完整 JSON 库)
static float json_get_float(const char* json, const char* key) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(json, search);
    return p ? atof(p + strlen(search)) : 0.0f;
}

void handle_frame(char* json) {
    // 先判断 cmd (简单字符串匹配，无需完整 JSON 解析)
    int is_move = (strstr(json, "\"move\"") != NULL);
    int is_ptz  = (strstr(json, "\"ptz\"") != NULL);
    int is_stop = (strstr(json, "\"stop\"") != NULL);
    int is_home = (strstr(json, "\"ptz_home\"") != NULL);

    if (is_stop) {
        set_motors(0, 0);
        set_ptz(0, 0);
        if (motion_timer_running) { timer_stop(); motion_timer_running = 0; }
        return;
    }

    if (is_home) {
        servo_home();  // pan=1500us, tilt=1500us
        return;
    }

    if (is_move) {
        float v = json_get_float(json, "v");
        float w = json_get_float(json, "w");
        set_motors(v, w);
    }
    if (is_ptz) {
        float pan  = json_get_float(json, "pan");
        float tilt = json_get_float(json, "tilt");
        set_ptz(pan, tilt);
    }

    if (is_move || is_ptz) {
        if (motion_timer_running) timer_reset();
        else { timer_start(300); motion_timer_running = 1; }
    }
}

void on_motion_timer_expired() {
    set_motors(0, 0);
    set_ptz(0, 0);
    motion_timer_running = 0;
}

// ── 云台归位 ──
void servo_home(void) {
    servo_set(SERVO_PAN,  1500);   // 中位
    servo_set(SERVO_TILT, 1500);
}

// ── 驱动参考 (不变) ──
void set_motors(float v, float w) {
    float left  = v * 100.0f - w * 50.0f;
    float right = v * 100.0f + w * 50.0f;
    #define CLAMP(x) ((x) > 100 ? 100 : ((x) < -100 ? -100 : (x)))
    motor_set(MOTOR_LEFT,  CLAMP(left));
    motor_set(MOTOR_RIGHT, CLAMP(right));
}

void set_ptz(float pan, float tilt) {
    servo_set(SERVO_PAN,  1500 + (int)(pan  * 500));
    servo_set(SERVO_TILT, 1500 + (int)(tilt * 500));
}

// ── 应答 (不变) ──
static void send_frame(const char* payload) {
    int len = strlen(payload);
    uint8_t x = (uint8_t)len;
    for (int i = 0; i < len; i++) x ^= payload[i];
    uart_send(0xAA); uart_send(0x55); uart_send((uint8_t)len);
    for (int i = 0; i < len; i++) uart_send(payload[i]);
    uart_send(x); uart_send('\r'); uart_send('\n');
}
void reply_ok()  { send_frame("{\"ack\":\"ok\"}"); }
void reply_err(const char* why) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ack\":\"err\",\"msg\":\"%s\"}", why);
    send_frame(buf);
}
```



