# OTA 测试服务器

ESP32 OTA 升级的本地测试服务器，零依赖（纯 Python 标准库）。

## 快速开始

```bash
# 1. 把测试固件放到 firmware/ 目录，文件名格式 <版本号>.bin
cp my_firmware.bin firmware/2.0.0.bin

# 2. 启动（默认 8080 端口）
python server.py

# 3. 访问 http://localhost:8080 查看状态页
```

## 目录结构

```
test-server/
├── server.py          # 测试服务器
├── firmware/          # 固件存放目录
│   ├── 1.0.0.bin
│   └── 2.0.0.bin
├── docs/              # 设计文档
└── README.md
```

## API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 状态页（固件列表 + 已注册设备）|
| POST | `/api/ota` | OTA 版本检查（设备端调用）|
| POST | `/api/bind-device` | 设备绑定（APP 端调用）|
| GET | `/firmware/<file>` | 固件下载 |

## 工作流程

```
设备 POST /api/ota（无 Authorization）
  → 服务端返回激活码
  → 用户在 APP 端 POST /api/bind-device 输入激活码
  → 设备再次 POST /api/ota（带签名）
  → 服务端返回固件信息（如有新版本）
  → 设备 GET /firmware/2.0.0.bin 下载固件
```

## 固件命名

服务器自动扫描 `firmware/` 目录，从文件名提取版本号：

```
2.0.0.bin          → version: 2.0.0
firmware_1.2.3.bin → version: 1.2.3
```

版本号最大的作为最新版本。无固件时，OTA 响应中 `firmware.url` 为空。

## 文档

| 文档 | 说明 |
|------|------|
| [API.md](docs/API.md) | 完整 API 接口文档 |
| [OTA.md](docs/OTA.md) | OTA 模块设计 |
| [DEVICE.md](docs/DEVICE.md) | ESP32 设备端对接 |
