# AGENTS.md

## 项目概述
本项目是一个软硬件结合的桌面状态监视项目：
- 设备端基于 `ESP32-C5 + PlatformIO`
- PC 端基于 `FastAPI + WebSocket + psutil`
- 设备通过 WebSocket 从 PC 服务读取状态，并显示在 128x64 LCD 上
- 提供 Web 服务，可以进行状态查看和配置修改/固件升级
- WiFi和一些运行配置存储在littleFS分区的config.json文件中
- 更详细介绍可以看README.md

## 软件更新方式
- COM
- espressif ota
- web update


## 编译测试
- 只需要编译wifi环境就行
