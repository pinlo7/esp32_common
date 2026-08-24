# OTA 测试服务器

ESP32 OTA 升级的本地测试服务器，零依赖（纯 Python 标准库），行为与生产后端
`/api/device_manage/ota` 对齐：组密钥首次激活、HMAC 签名校验、时间戳规则、响应结构一致。

## 快速开始

```bash
# 1. 把测试固件放到 firmware/ 目录，文件名格式 <版本号>.bin
cp my_firmware.bin firmware/2.0.0.bin

# 2. 启动（默认 8080 端口）
PROVISION_KEYS=test-provision-key python3 server.py

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
| POST | `/api/device_manage/ota` | OTA 检查 / 首次激活（与生产一致）|
| POST | `/api/ota` | 兼容别名 |
| GET | `/firmware/<file>` | 固件下载 |

## 工作流程

```
设备 POST /api/device_manage/ota（首次激活）
  Headers: Device-Id + X-Timestamp + X-Provision-Key + Authorization: Bearer {签名}
  → 服务端用组密钥验签（不校验时间戳）
  → 返回 device_secret + MQTT 凭据 + server_time（设备校时）+ 固件信息

设备 POST /api/device_manage/ota（已注册）
  Headers: Device-Id + X-Timestamp + Authorization: Bearer {签名(device_secret)}
  → 服务端校验时间戳 ±300s，签名通过后返回最新固件信息
```

签名算法：`SHA256(密钥 + 时间戳 + device_id + SHA256(body))`；密钥在首次激活时用组密钥，
激活后用 `device_secret`。

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `PROVISION_KEYS` | `test-provision-key` | 逗号分隔的组密钥 |
| `TIMESTAMP_TOLERANCE` | `300` | 已注册设备时间戳容差（秒） |
| `MQTT_ENDPOINT` | `mqtt://localhost:1883` | 返回给设备的 MQTT 地址 |
| `WEBSOCKET_URL` | `wss://api.example.com/ws/` | 返回给设备的 WebSocket 地址 |
| `DEVICES_FILE` | 无（仅内存） | 设备记录持久化 JSON 文件 |
| `RATE_LIMIT` | `600` | 每 IP 10 分钟 OTA 请求上限 |

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
