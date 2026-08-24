# API 接口文档

## 基础信息

```
Base URL: http://your-server:3000/api
Content-Type: application/json
Authorization: Bearer {JWT Token}（管理后台接口）
```

---

## 一、认证接口

### POST /api/auth/login

用户登录，获取 JWT Token。

**请求**：
```json
{
  "username": "admin",
  "password": "admin123"
}
```

**响应**：
```json
{
  "access_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "user": {
    "id": 1,
    "username": "admin",
    "role": "admin"
  }
}
```

**错误响应**：
```json
{
  "error": "invalid credentials"
}
```

---

## 二、设备管理接口（/api/device_manage/）

### POST /api/device_manage/ota

设备端调用，检查版本 / 注册设备。

**请求头**：
```
Device-Id: 11:22:33:44:55:66（MAC 地址，必需）
Client-Id: 7b94d69a-9808-4c59-9c9b-704333b38aff（UUID，必需）
X-Timestamp: 1719900000（Unix 时间戳，必需）
Authorization: Bearer {签名}（首次可省略）
User-Agent: wifi/1.0.0（设备名/版本，必需）
Accept-Language: zh-CN（可选）
```

**请求体**：
```json
{
  "application": {
    "version": "1.0.0",
    "elf_sha256": "c8a8ecb6d6fbcda..."
  },
  "board": {
    "type": "wifi",
    "name": "wifi_board",
    "ssid": "卧室",
    "rssi": -55,
    "channel": 6,
    "ip": "192.168.1.100",
    "mac": "11:22:33:44:55:66"
  }
}
```

**响应（首次注册）**：
```json
{
  "activation": {
    "code": "",
    "message": "设备已注册"
  },
  "device_secret": "a1b2c3d4e5f67890...",
  "mqtt": {
    "endpoint": "mqtt.example.com",
    "client_id": "GID_xxx@@@11:22:33:44:55:66@@@uuid",
    "username": "device_112233",
    "password": "xxx",
    "publish_topic": "device/11:22:33:44:55:66/status"
  },
  "websocket": {
    "url": "wss://api.example.com/ws/",
    "token": "xxx"
  },
  "server_time": {
    "timestamp": 1719900000000,
    "timezone": "Asia/Shanghai",
    "timezone_offset": 480
  },
  "firmware": {
    "version": "1.0.0",
    "url": "",
    "rotate_key": ""
  }
}
```

**响应（已注册，有新版本）**：
```json
{
  "activation": {
    "code": "",
    "message": ""
  },
  "mqtt": { "..." },
  "websocket": { "..." },
  "server_time": { "..." },
  "firmware": {
    "version": "2.0.0",
    "url": "https://cdn.example.com/firmware/2.0.0.bin",
    "rotate_key": ""
  }
}
```

**响应（签名错误）**：
```json
{
  "error": "signature invalid"
}
```

**响应（设备被踢出）**：
```json
{
  "error": "device removed",
  "message": "设备已被移除，请重新注册"
}
```

---

### GET /api/device_manage/devices

获取设备列表（需要 JWT）。

**查询参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| page | number | 页码，默认 1 |
| limit | number | 每页数量，默认 20 |
| search | string | 搜索设备 ID 或名称 |
| is_online | boolean | 筛选在线状态 |

**响应**：
```json
{
  "devices": [
    {
      "id": 1,
      "device_id": "11:22:33:44:55:66",
      "board_type": "wifi",
      "board_name": "wifi_board",
      "firmware_version": "1.0.0",
      "ssid": "卧室",
      "rssi": -55,
      "is_online": true,
      "last_seen_at": "2024-01-01T00:00:00Z",
      "created_at": "2024-01-01T00:00:00Z"
    }
  ],
  "total": 100,
  "page": 1,
  "limit": 20
}
```

---

### GET /api/device_manage/devices/:id

获取设备详情（需要 JWT）。

**响应**：
```json
{
  "id": 1,
  "device_id": "11:22:33:44:55:66",
  "board_type": "wifi",
  "board_name": "wifi_board",
  "firmware_version": "1.0.0",
  "ssid": "卧室",
  "rssi": -55,
  "ip_address": "192.168.1.100",
  "is_online": true,
  "last_seen_at": "2024-01-01T00:00:00Z",
  "created_at": "2024-01-01T00:00:00Z",
  "recent_status": [
    {
      "data": { "battery": 85, "signal": 90 },
      "created_at": "2024-01-01T00:00:00Z"
    }
  ]
}
```

---

### DELETE /api/device_manage/devices/:id

踢出设备（需要 JWT）。

**响应**：
```json
{
  "success": true,
  "message": "设备已踢出"
}
```

说明：删除设备记录，设备下次请求会自动重新注册。

---

### POST /api/device_manage/devices/:id/reset-secret

重置设备密钥（需要 JWT）。

**响应**：
```json
{
  "success": true,
  "message": "密钥已重置，设备需重新注册"
}
```

说明：重置 `device_secret`，设备需重新获取。

---

### POST /api/device_manage/devices/:id/command

发送命令到设备（需要 JWT）。

**请求**：
```json
{
  "command": "reboot",
  "params": {}
}
```

**响应**：
```json
{
  "success": true,
  "message": "命令已发送"
}
```

支持的命令：
| 命令 | 说明 |
|------|------|
| reboot | 重启设备 |
| lock | 锁机 |
| unlock | 解锁 |
| config | 下发配置 |

---

## 三、固件管理接口

### GET /api/firmware

获取固件版本列表（需要 JWT）。

**响应**：
```json
{
  "firmwares": [
    {
      "id": 1,
      "version": "2.0.0",
      "url": "https://cdn.example.com/firmware/2.0.0.bin",
      "board_type": "wifi",
      "is_active": true,
      "created_at": "2024-01-01T00:00:00Z"
    }
  ]
}
```

---

### POST /api/firmware

上传新固件版本（需要 JWT）。

**请求**：`multipart/form-data`

| 字段 | 类型 | 说明 |
|------|------|------|
| version | string | 版本号 |
| board_type | string | 适用型号 |
| file | file | 固件文件 |

**响应**：
```json
{
  "id": 2,
  "version": "2.0.0",
  "url": "https://cdn.example.com/firmware/2.0.0.bin",
  "board_type": "wifi"
}
```

---

### DELETE /api/firmware/:id

删除固件版本（需要 JWT）。

**响应**：
```json
{
  "success": true
}
```

---

## 四、错误码

| 状态码 | 说明 |
|------|------|
| 200 | 成功 |
| 400 | 请求参数错误 |
| 401 | 未授权 / 签名错误 |
| 403 | 权限不足 |
| 404 | 资源不存在 |
| 409 | 资源冲突 |
| 500 | 服务器内部错误 |

**错误响应格式**：
```json
{
  "error": "error_code",
  "message": "错误描述"
}
```
