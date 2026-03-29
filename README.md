# PCMonitor

[English README](./README_EN.md)

PCMonitor 是一个软硬件结合的桌面状态监视项目：

- 设备端基于 `ESP32-C5 + Arduino + PlatformIO`
- PC 端基于 `FastAPI + WebSocket + psutil`
- 设备通过 WebSocket 从 PC 服务读取状态，并显示在 128x64 LCD 上

项目当前包含两部分：

- 根目录：ESP32 固件工程
- [`server/`](./server/)：Windows 侧系统监控服务

## 项目预览

![main](./doc/main-pic.png)

## 功能特性

- 在 LCD 上显示 CPU、内存、上传速率、下载速率、日期时间
- 通过 WebSocket 实时接收 PC 监控数据
- 设备端支持 LittleFS 持久化配置
- 支持多个 WebSocket 服务器地址，掉线后自动轮换
- 支持 Wi-Fi 配网门户
- 支持 Web 配置页面
- 支持网页 OTA 固件更新
- 支持屏幕旋转、背光亮度、自动背光时段
- 支持 mDNS 访问：`http://monitor.local`

## 目录结构

```text
PCMonitor/
|- src/                 ESP32 固件源码
|- data/                LittleFS 初始配置
|- lib/                 本地库
|- server/              PC 端监控服务
|- doc/                 原理图、图片、装配辅助资料
|- platformio.ini       PlatformIO 配置
|- partitions.csv       Flash 分区表
|- README.md            中文说明
`- README_EN.md         English README
```

## 系统架构

1. PC 端运行 [`server/sysmonitor.py`](./server/sysmonitor.py) 或打包后的 `sysmonitor.exe`
2. 服务每秒采集一次本机状态，并通过 `/ws` 推送 JSON
3. ESP32 连接到同一局域网中的服务地址
4. 设备收到数据后刷新 LCD 显示

设备端当前消费的数据格式大致如下：

```json
{
  "time": "2025-10-14 18:02:02",
  "name": "MyPC",
  "week": "Tuesday",
  "cpu_usage": 12.3,
  "memory_usage": 48.7,
  "network": {
    "up": "14.4 KB/s",
    "down": "15.4 KB/s"
  }
}
```

## 硬件与固件

固件工程使用 PlatformIO，目标板在 [`platformio.ini`](./platformio.ini) 中配置为：

- `esp32-c5-devkitc-1`
- framework: `arduino`
- 文件系统：`LittleFS`

主要依赖包括：

- `U8g2`
- `WiFiManager`
- `ArduinoJson`
- `WebSockets`
- `OneButton`
- `TimeLib`

### 引脚定义

定义位于 [`src/main.h`](./src/main.h)：

- `KEY_B1 = GPIO9`
- `KEY_B2 = GPIO0`
- `LED = GPIO10`
- `DIS_RST = GPIO4`
- `DIS_DC = GPIO5`
- `DIS_SCK = GPIO6`
- `DIS_SDA = GPIO7`
- `DIS_BL = GPIO8`

### 按键行为

根据当前固件逻辑：

- `B1` 单击：切换背光开关（暂时取消此功能）
- `B2` 单击：切换下一个 WebSocket 服务器
- `B2` 长按后松开：重启设备

### 默认配置

LittleFS 初始配置文件为 [`data/config.json`](./data/config.json)。

示例：

```json
{
  "ssid": "your-ssid",
  "psw": "your-passwd",
  "wifiBand": "5g",
  "backlight": 50,
  "rotation": 2,
  "currentIdx": 0,
  "autoBacklight": false,
  "onTime": "07:50",
  "offTime": "23:50",
  "servers": [
    "192.168.1.100:8000/ws"
  ]
}
```

## PC 端监控服务

PC 服务位于 [`server/`](./server/)。

它会：

- 读取本机 CPU 使用率
- 读取内存使用率
- 计算网络上传/下载速度
- 通过 FastAPI 提供 HTTP 和 WebSocket 接口
- 可作为托盘程序长期驻留

### 依赖

见 [`server/requirements.txt`](./server/requirements.txt)：

- `fastapi`
- `uvicorn[standard]`
- `psutil`
- `pystray`
- `pillow`

### 服务配置

默认配置文件是 [`server/config.json`](./server/config.json)：

```json
{
  "name": "Asrock",
  "host": "0.0.0.0",
  "port": 8000
}
```

其中：

- `name` 会显示在设备屏幕底部
- `host` 为服务监听地址
- `port` 为 HTTP / WebSocket 端口

### 可访问接口

- 首页：`http://<host>:<port>/`
- 统计接口：`http://<host>:<port>/api/stats`
- WebSocket：`ws://<host>:<port>/ws`

## 快速开始

### 1. 启动 PC 端服务

进入 [`server/`](./server/) 后：

```bash
pip install -r requirements.txt
python sysmonitor.py
```

如果你直接使用仓库内的 `sysmonitor.exe`，也可以先修改同目录下的 `config.json` 再运行。

### 2. 编译并烧录固件

在项目根目录执行：

```bash
pio run -e usb
pio run -e usb -t upload
```

如果已经联网并启用了 OTA，也可以使用：

```bash
pio run -e wifi -t upload
```

说明：

- `usb` 环境通过串口烧录
- `wifi` 环境通过 `espota` 上传

### 3. 上传 LittleFS 配置

首次使用建议把 [`data/config.json`](./data/config.json) 一并写入文件系统：

```bash
pio run -t uploadfs
```

上传前请先把其中的 Wi-Fi 和服务器地址改成你自己的环境。

### 4. 首次配网

如果设备无法连上已保存的 Wi-Fi，会启动配网门户：

- AP 名称：`MonitorV1.2`
- AP 密码：`11111178`

连接该热点后，按提示完成 Wi-Fi 配置。

## 设备 Web 配置

设备连网后可通过以下方式访问配置页：

- `http://monitor.local`
- 或设备当前 IP 地址

配置页支持：

- 修改 Wi-Fi SSID / 密码
- 选择 Wi-Fi 频段偏好：`5G only / 2.4G only / Auto`
- 设置背光亮度
- 设置 LCD 方向
- 设置自动背光时间
- 编辑多个 WebSocket 服务器地址
- 打开固件更新页面

## OTA 更新

项目支持两种 OTA 方式：

- Arduino OTA
- Web OTA

Web OTA 页面入口：

- `http://monitor.local/update`
- 或 `http://<device-ip>/update`

固件内部也保留了 `/ota` 上传处理逻辑。

## 相关资料

`doc/` 目录中包含：

- 屏幕与实物图
- 原理图 PDF
- BOM Excel
- Wi-Fi 配置步骤
- 焊接辅助页面

示例图片：

- ![board](./doc/3d-pic.png)

## 开发说明

- 设备端版本字符串目前定义在 [`src/main.cpp`](./src/main.cpp) 中：
  - `hw_ver = "1.2"`
  - `sw_ver = "1.3"`
- NTP 默认使用 `ntp6.aliyun.com`
- 当前代码默认时区为 `UTC+8`
- mDNS 主机名为 `MonitorV1`

## 已知使用前提

- PC 服务和设备需要在同一局域网，或设备可以访问到配置中的服务器地址
- Windows 防火墙可能需要放行对应端口
- `monitor.local` 依赖本地网络环境对 mDNS 的支持
- `server/sysmonitor.exe` 为已打包产物，若运行异常，优先使用 Python 源码方式启动

## License

项目使用 [`LICENSE`](./LICENSE) 中的 MIT License。
