# GuardBox 外卖防盗 App

这是根据 `外卖防盗系统App_PRD_V1.0.docx` 和当前 `main.c.docx` 硬件代码做出的 MVP App 原型。它是一个零依赖移动端 Web App，可以直接用于演示，也预留了 Web Serial 串口联调真实设备。

## 已实现

- 首页 Dashboard：连接状态、当前重量、基准重量、下降重量、掉重阈值、蜂鸣器状态。
- 防盗控制：开始防盗、停止监测、关闭报警、刷新状态。
- 校准向导：空载去皮、已知重量校准、读取 raw、显示 offset/scale。
- 报警闭环：演示模式会触发 ALARM、弹出报警层、生成报警记录、模拟证据。
- 历史记录：本地保存最近报警事件，记录重量、掉重、处理状态和证据说明。
- 设置页：切换演示模式，设置掉重阈值，对应固件 `set_drop` 命令。
- 真实设备预留：支持浏览器 Web Serial，波特率 115200，发送当前固件已有 MSH 命令。
- 独立 ESP32 摄像头：支持配置 HTTP 抓拍地址，例如 `http://192.168.4.1/capture`。

## 运行方式

最简单方式：

1. 在 `E:\delivery_locker` 目录启动一个本地静态服务器。
2. 用 Chrome 或 Edge 打开本地地址。

推荐命令：

```powershell
python -m http.server 4173
```

然后打开：

```text
http://localhost:4173
```

如果只看界面，直接双击 `index.html` 也可以；如果要使用 Web Serial 串口连接，必须通过 `localhost` 打开。

## 硬件联调说明

你的硬件是两块独立设备时，推荐这样连：

- RT-Thread 开发板：USB 串口连接电脑，负责 HX711 称重、蜂鸣器、防盗状态机。
- ESP32 摄像头：通过 Wi-Fi 提供 HTTP 抓拍/视频流，App 里填写抓拍地址。

当前 RT-Thread 固件暴露的是 FinSH/MSH 命令，App 会发送：

- `hx711_tare`
- `hx711_cal 500`
- `hx711_raw`
- `guard_start`
- `guard_stop`
- `guard_status`
- `alarm_off`
- `set_drop 300`

串口参数：

- Baud rate: `115200`
- Line ending: `\r\n`

当前 App 既能解析建议的 JSON Line 协议，也尽量兼容现有 `rt_kprintf` 文本输出。后续如果固件新增 PRD 中建议的 JSON Line 协议，前端的解析稳定性会更好。

## ESP32 摄像头接入

如果 ESP32-CAM 跑的是常见摄像头 WebServer，常用地址是：

- AP 模式：`http://192.168.4.1/capture`
- 局域网模式：`http://ESP32的IP/capture`

在 App 的“设置 → ESP32 摄像头”里填写抓拍地址，点“测试画面”。报警发生时，App 会自动刷新这个抓拍地址并把它关联到报警记录。

如果你的 ESP32 只有 USB 串口、没有 HTTP 摄像头服务，建议先给 ESP32 烧录一个 CameraWebServer 固件。图片/视频不适合走串口传输，串口更适合调试和控制命令。

## 重要固件提醒

PRD 中提到一个关键点：当前硬件代码进入报警时，需要在触发 ALARM 前记录：

```c
g_alarm_weight_g_x100 = current_weight_g_x100;
```

建议分别加在“重量太低”和“下降超过阈值”两个进入 `GUARD_STATE_ALARM` 的分支里。否则自动恢复时 `rise_from_alarm` 可能不准确。
