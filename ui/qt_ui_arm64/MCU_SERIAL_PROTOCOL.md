# RK3588 ↔ MCU 串口通信协议

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

## RK3588 → MCU 命令

### 底盘控制 (WASD)

| 按键 | 动作 | JSON |
|------|------|------|
| W | 前进 | `{"cmd":"move","v":0.5,"w":0}` |
| S | 后退 | `{"cmd":"move","v":-0.3,"w":0}` |
| A | 左转 | `{"cmd":"move","v":0,"w":0.8}` |
| D | 右转 | `{"cmd":"move","v":0,"w":-0.8}` |

| 字段 | 类型 | 说明 |
|------|------|------|
| cmd | string | 固定 "move" |
| v | float | 线速度 -1~1 (正=前进) |
| w | float | 角速度 -1~1 (正=左转) |

### 云台控制 (↑↓←→)

| 按键 | 动作 | JSON |
|------|------|------|
| ↑ | 上仰 | `{"cmd":"ptz","pan":0,"tilt":0.5}` |
| ↓ | 下俯 | `{"cmd":"ptz","pan":0,"tilt":-0.5}` |
| ← | 左转 | `{"cmd":"ptz","pan":-0.5,"tilt":0}` |
| → | 右转 | `{"cmd":"ptz","pan":0.5,"tilt":0}` |

| 字段 | 类型 | 说明 |
|------|------|------|
| cmd | string | 固定 "ptz" |
| pan | float | 水平转角 -1~1 (正=右) |
| tilt | float | 垂直转角 -1~1 (正=上) |

## MCU 控制逻辑

RK3588 按住按键时每 **100ms** 重复发送一帧。MCU 收到帧后：

1. 解析 JSON，判断 cmd 类型
2. `move` → `set_motors(v, w)`，驱动底盘
3. `ptz` → `set_ptz(pan, tilt)`，驱动云台舵机
4. 启动 **100ms 硬件定时器**
5. 定时器到期 → 停止底盘+云台

**关键**: MCU 每次收到新帧，重置定时器。按键松开后 RK3588 不再发送帧，MCU 定时器到期自动停。

```
MCU 伪代码:
  on_frame_received(json):
      cmd = parse(json)
      if cmd == "move":  set_motors(v, w)
      if cmd == "ptz":   set_ptz(pan, tilt)
      restart_timer(100ms)

  on_timer_expired():
      set_motors(0, 0)
      set_ptz(0, 0)
```

## MCU → RK3588 应答

成功：`{"ack":"ok"}`  
错误：`{"ack":"err","msg":"checksum"}` / `{"ack":"err","msg":"json"}` / `{"ack":"err","msg":"len"}`

## 接收参考实现 (C)

```c
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ── 状态机 ──
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

// ── 帧处理 ──
static int motion_timer_running = 0;

void handle_frame(char* json) {
    float v = 0, w = 0, pan = 0, tilt = 0;
    char* p;
    p = strstr(json, "\"v\":");    if (p) v    = atof(p + 4);
    p = strstr(json, "\"w\":");    if (p) w    = atof(p + 4);
    p = strstr(json, "\"pan\":");  if (p) pan  = atof(p + 6);
    p = strstr(json, "\"tilt\":"); if (p) tilt = atof(p + 7);

    if (strstr(json, "\"move\"")) set_motors(v, w);
    if (strstr(json, "\"ptz\""))  set_ptz(pan, tilt);

    if (motion_timer_running) timer_reset();
    else { timer_start(100); motion_timer_running = 1; }
}

void on_motion_timer_expired() {
    set_motors(0, 0);
    set_ptz(0, 0);
    motion_timer_running = 0;
}

// ── 应答 ──
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

## 驱动参考

### 底盘 (差速)
```c
void set_motors(float v, float w) {
    float left  = v * 100.0f - w * 50.0f;
    float right = v * 100.0f + w * 50.0f;
    #define CLAMP(x) ((x) > 100 ? 100 : ((x) < -100 ? -100 : (x)))
    motor_set(MOTOR_LEFT,  CLAMP(left));
    motor_set(MOTOR_RIGHT, CLAMP(right));
}
```

### 云台 (舵机)
```c
void set_ptz(float pan, float tilt) {
    // 1500us = 中位, ±500us = 满偏
    servo_set(SERVO_PAN,  1500 + (int)(pan  * 500));
    servo_set(SERVO_TILT, 1500 + (int)(tilt * 500));
}
```
