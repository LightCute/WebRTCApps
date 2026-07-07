# AI 视觉模型 — 训练、部署与推理

## 一、模型总览

本项目部署了 3 种 AI 视觉模型，覆盖陪护场景中的核心安全需求：

| 模型 | 基础架构 | 检测目标 | 部署平台 | 推理间隔 |
|------|---------|---------|---------|---------|
| 行人识别与追踪 | YOLOv5 | person (行人) | RK3588 NPU | 250ms |
| 跌倒检测 | YOLOv11 | fallen, standing, sitting | RK3588 NPU | 250ms |
| 火灾烟雾检测 | YOLOv8 | fire, smoke | RK3588 NPU | 250ms |

三种模型通过 Qt UI 的"启动AI"/"跌倒检测"/"火灾检测"按钮在线切换，无需重启进程。切换时 QProcess 终止当前 AI 进程，启动新的推理二进制。

**代码证据**：[mainwindow.cpp:904-969](apps/ui/qt_ui_arm64/mainwindow.cpp#L904-L969) — `startAi()` 根据 `AiType` 选择不同模型路径和推理程序。

---

## 二、模型训练流程

### 2.1 训练管线

```
┌──────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────┐
│ 数据采集  │    │  数据标注      │    │  模型训练      │    │  模型转换  │
│          │───►│              │───►│              │───►│          │
│ 摄像头    │    │  标注工具:     │    │  PyTorch      │    │  ONNX    │
│ 实拍/公开 │    │  LabelImg     │    │  YOLOv5/v8/v11│    │  export  │
│ 数据集    │    │  / Roboflow   │    │  GPU 训练      │    │          │
└──────────┘    └──────────────┘    └──────────────┘    └─────┬────┘
                                                              │
                                                              ▼
┌──────────┐    ┌──────────────┐    ┌──────────────────────────────┐
│ NPU 推理  │    │  模型编译      │    │  模型量化                     │
│          │◄───│              │◄───│                              │
│ RKNN API │    │  RKNN-Toolkit│    │  FP32 → INT8                 │
│ 实时检测  │    │  ONNX→RKNN   │    │  精度损失 < 1% mAP            │
└──────────┘    └──────────────┘    └──────────────────────────────┘
```

### 2.2 数据采集与标注

**数据来源**：
- 公开数据集：COCO (Common Objects in Context) 的 person 类 —— 用于基础行人检测
- 自采集数据：在实际居家环境中，用机器人摄像头拍摄老人行走、坐立、跌倒等动作的视频帧
- 火灾/烟雾数据：来自公开火灾检测数据集 + 网络爬虫采集的火灾和烟雾图片

**标注工具**：LabelImg 或 Roboflow，对每张图片标注目标类别和边界框：
- 行人检测：`person` 类，标注全身框
- 跌倒检测：`fallen` (跌倒)、`standing` (站立)、`sitting` (坐下)
- 火灾烟雾：`fire` (明火)、`smoke` (烟雾)

**数据增强**：训练时应用随机翻转、旋转、缩放、亮度/对比度调整、Mosaic 拼接等增强策略来提升模型在各种光照、角度和距离下的鲁棒性。

### 2.3 模型训练 (PyTorch, GPU)

**训练环境**：NVIDIA GPU (RTX 3060 或更高)，PyTorch 框架，Ultralytics YOLO 训练框架。

**训练参数**：

| 参数 | 行人检测 | 跌倒检测 | 火灾烟雾 |
|------|---------|---------|---------|
| 基础模型 | YOLOv5s | YOLOv11n | YOLOv8n |
| 输入尺寸 | 640×640 | 640×640 | 640×640 |
| 训练轮数 | 300 epochs | 300 epochs | 300 epochs |
| 批量大小 | 16 | 16 | 16 |
| 优化器 | SGD + Momentum | AdamW | AdamW |
| 初始学习率 | 0.01 | 0.001 | 0.001 |

**YOLO 算法原理（简述）**：

YOLO (You Only Look Once) 是一种单阶段目标检测算法。它将输入图像划分为 S×S 的网格，每个网格预测 B 个边界框和对应的类别概率。与两阶段检测器（如 Faster R-CNN）不同，YOLO 通过单次前向传播直接输出检测结果，速度更快，适合嵌入式部署。

YOLOv5/v8/v11 均采用 Anchor-Free（无锚框）设计，通过预测边界框中心点相对于网格的偏移和宽高来定位目标，简化了输出解码过程。

损失函数由三部分组成：
1. **边界框回归损失** (CIoU Loss) — 度量预测框与真实框的位置偏差
2. **目标置信度损失** (BCE Loss) — 判断该预测框中是否包含目标
3. **分类损失** (BCE Loss) — 判断目标的类别

### 2.4 模型导出与量化 (FP32 → INT8)

训练完成后，PyTorch 模型 (.pt) 需要转换为 RK3588 NPU 可执行的 RKNN 格式：

1. **导出为 ONNX**：`model.export(format="onnx")` 将 PyTorch 模型导出为标准 ONNX 格式
2. **量化**：RKNN-Toolkit2 将 FP32 模型量化为 INT8。量化过程使用校准数据集（从训练集中采样约 200 张图片），统计每一层的激活值分布，将 32 位浮点权重量化到 8 位整数
3. **编译**：RKNN-Toolkit2 编译量化后的模型为 `.rknn` 文件，该文件包含 NPU 可直接执行的指令序列

**量化效果**：INT8 量化后推理速度提升约 3-4 倍，模型大小缩减约 4 倍，精度损失通常在 1% mAP 以内。

**代码证据**：推理时加载 `.rknn` 模型的代码位于 AI 推理进程的 `main.cc`：[rknn_init](main.cc#L151) — `rknn_init(&ctx, model_data, model_size, 0, nullptr)` 加载编译后的 RKNN 模型到 NPU。

---

## 三、模型部署与推理

### 3.1 NPU 推理管线

```
摄像头 DMA-BUF (I420 640×480)
        │
        ▼
   RGA 硬件转换: I420 → BGR (640×640, resize到模型输入)
        │
        ▼
   RKNN NPU 推理: 输入 [1,640,640,3] → 输出 [1,6,8400]
        │
        ▼
   后处理解码: anchor box → 边界框坐标
        │  ltrb 距离 → 绝对坐标 (x1,y1,x2,y2)
        │  sigmoid 激活 → 类别置信度
        ▼
   NMS (非极大值抑制): 抑制重叠框, IoU 阈值 0.45
        │
        ▼
   坐标缩放: 模型坐标(640×640) → 原始帧坐标(640×480)
        │
        ▼
   Unix Socket 发送: JSON → /tmp/webrtc_runtime/ai_detections.sock
```

**代码证据**：[main.cc:218-349](main.cc#L218-L349) — 完整的推理 + 后处理 + 结果发送流程。

### 3.2 YOLO 输出解码

YOLOv5/v8/v11 的 Anchor-Free 输出格式为 `[1, 6, 8400]`：
- 第 1 维：batch size = 1
- 第 2 维：6 = 4 个 bbox 距离 (left, top, right, bottom) + 2 个类别分数 (fire, smoke)
- 第 3 维：8400 = 80×80 + 40×40 + 20×20 三个尺度的 anchor 点总和

解码过程：每个 anchor 点预测到目标边界的 ltrb 距离，乘以对应尺度的 stride 得到像素坐标，再缩放到原始帧分辨率。

**代码证据**：[main.cc:248-279](main.cc#L248-L279) — anchor box 解码逻辑。

### 3.3 NMS (非极大值抑制)

解码后可能产生大量重叠的检测框（同一目标被多个 anchor 点检出）。NMS 按以下步骤去重：

1. 按置信度降序排列所有候选框
2. 取置信度最高的框，计算它与其余框的 IoU (交并比)
3. 若 IoU > 0.45 且类别相同，则抑制低置信度框
4. 对剩下未被抑制的框重复步骤 2-3
5. 保留置信度 > 0.45 的框作为最终检测结果

**代码证据**：[main.cc:282-304](main.cc#L282-L304) — NMS 实现。

### 3.4 UI 端二次 NMS

在 Qt UI 的 `GlVideoWidget::setDetections()` 中，对 AI 推理进程发来的检测结果再次执行 NMS（IoU 阈值 0.5），作为二次去重保障，防止同一人在多个尺度上被重复标注。

**代码证据**：[gl_video_widget.cpp:43-76](apps/ui/qt_ui_arm64/gl_video_widget.cpp#L43-L76)

---

## 四、ASR / TTS / LLM 语音对话管线

语音对话功能使用现有成熟模型的调度，未自行训练：

```
┌──────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────┐
│ 麦克风    │    │  ASR (本地)   │    │  LLM (云端)   │    │ TTS (本地)│
│ 采集      │───►│  Whisper      │───►│  大模型 API   │───►│  本地TTS  │
│          │    │  Speech→Text  │    │  Text→Text    │    │ Text→语音 │
└──────────┘    └──────────────┘    └──────────────┘    └─────┬────┘
                                                              │
                                                              ▼
                                                        ┌──────────┐
                                                        │ 扬声器    │
                                                        │ 播放      │
                                                        └──────────┘
```

### 4.1 ASR：Whisper 本地部署

OpenAI 开源的 Whisper 模型在 RK3588 上本地部署运行。Whisper 是一个端到端的语音识别 Transformer 模型，支持多语言，在通用语音识别任务上表现优异。选择本地部署而非云端 API 的原因：降低延迟、不依赖公网连接。

### 4.2 LLM：云端 API 调度

语音转文字后，将文本发送至大语言模型 API（如 OpenAI GPT 或国产大模型），LLM 生成回复文本。选择云端 API 而非本地部署的原因：大模型参数量巨大（7B-70B），无法在 RK3588 上实时推理。

### 4.3 TTS：本地语音合成

LLM 生成的回复文本通过本地 TTS 模型转换为语音波形，经扬声器输出。选择本地部署以降低延迟。

**代码证据**：[voice_chat_manager.cpp](apps/ui/qt_ui_arm64/voice_chat_manager.cpp) — 语音对话进程的启动管理。

---

## 五、AI 推理进程管理

Qt UI 通过 `QProcess` 管理 AI 推理进程的生命周期：

```
启动AI 按钮
    │
    ├─ QProcess::start(binary, args)
    │    args: --ctrl-shm (视频帧 SHM 路径)
    │          --model (RKNN 模型路径)
    │          --result-socket (检测结果 Unix Socket)
    │          --infer-interval-ms=250
    │
    ├─ AI 进程从 SHM 读取视频帧
    ├─ RGA→RKNN→后处理→JSON
    ├─ JSON → Unix Socket (/tmp/webrtc_runtime/ai_detections.sock)
    │
    └─ 停止AI → QProcess::terminate() → 等待 3s → kill()
```

**代码证据**：[mainwindow.cpp:904-969](apps/ui/qt_ui_arm64/mainwindow.cpp#L904-L969) — `startAi()` 完整流程；[mainwindow.cpp:55](apps/ui/qt_ui_arm64/mainwindow.cpp#L55) — `AiReceiver` 监听检测结果。
