# OTA 模块设计

## 一、概述

OTA（Over-The-Air）模块负责设备固件的空中升级，包括：

- 设备自动注册
- 版本检查
- 固件下载
- 密钥轮换

---

## 二、流程

### 1. 设备首次注册

```
设备请求 POST /api/device_manage/ota
  Headers:
    Device-Id: 11:22:33:44:55:66
    Client-Id: uuid
    X-Timestamp: 1719900000
    User-Agent: wifi/1.0.0
  Body:
    { application: {...}, board: {...} }

      │
      ▼

服务端处理：
  1. 检查 device_id 是否存在
  2. 不存在 → 生成 device_secret
  3. 创建设备记录
  4. 返回响应 + device_secret

      │
      ▼

设备收到响应：
  1. 保存 device_secret 到 NVS
  2. 保存 MQTT/WebSocket 配置
  3. 同步服务器时间
```

### 2. 版本检查

```
设备请求 POST /api/device_manage/ota
  Headers:
    Device-Id: 11:22:33:44:55:66
    Authorization: Bearer {签名}
    ...

      │
      ▼

服务端处理：
  1. 验证签名
  2. 查找设备记录
  3. 比较固件版本
  4. 返回是否有新版本

      │
      ▼

设备收到响应：
  1. 如果有新版本 → 下载固件
  2. 更新设备信息（ssid, rssi 等）
```

### 3. 设备被踢出

```
管理员在后台踢出设备
  → 删除设备记录

设备下次请求：
  服务端找不到设备记录
  → 返回 { error: "device removed" }

设备处理：
  → 清除本地 device_secret
  → 重新请求（无 Authorization）
  → 自动重新注册
```

---

## 三、签名算法

### 签名原文格式

```
text = device_secret + timestamp_str + device_id + body_sha256
```

| 组成部分 | 说明 | 示例 |
|---------|------|------|
| device_secret | 设备密钥（hex） | `a1b2c3d4e5f6...` |
| timestamp_str | Unix 秒级时间戳 | `"1719900000"` |
| device_id | Device-Id 头的值 | `"11:22:33:44:55:66"` |
| body_sha256 | 请求体 JSON 的 SHA256 | `"e3b0c44298fc..."` |

### 最终签名

```
signature = SHA256(device_secret + timestamp_str + device_id + body_sha256)
Authorization: Bearer {signature}
```

### 服务端验证（Node.js）

```typescript
import crypto from 'crypto';

function verifySignature(
  deviceSecret: string,
  timestamp: string,
  deviceId: string,
  bodyJson: string,
  signature: string
): boolean {
  // 1. 检查时间戳 ±5 分钟
  const now = Math.floor(Date.now() / 1000);
  if (Math.abs(now - parseInt(timestamp)) > 300) {
    return false;
  }

  // 2. 计算请求体 hash
  const bodyHash = crypto.createHash('sha256').update(bodyJson).digest('hex');

  // 3. 拼接签名原文
  const text = deviceSecret + timestamp + deviceId + bodyHash;

  // 4. 计算签名
  const expected = crypto.createHash('sha256').update(text).digest('hex');

  // 5. 常量时间比较，防止时序攻击
  return crypto.timingSafeEqual(
    Buffer.from(signature, 'hex'),
    Buffer.from(expected, 'hex')
  );
}
```

### ESP32 设备端（C++）

```cpp
#include "mbedtls/sha256.h"

std::string Sha256Hex(const std::string& input) {
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)input.c_str(), input.length(), hash, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
    return std::string(hex);
}

std::string CalculateSignature(
    const std::string& deviceSecret,
    const std::string& timestamp,
    const std::string& deviceId,
    const std::string& bodyJson
) {
    std::string bodyHash = Sha256Hex(bodyJson);
    std::string text = deviceSecret + timestamp + deviceId + bodyHash;
    return Sha256Hex(text);
}
```

---

## 四、接口实现

### POST /api/device_manage/ota

```typescript
// routes/device_manage.ts
import { Router } from 'express';
import { prisma } from '../db';
import crypto from 'crypto';
import { verifySignature } from '../utils/crypto';

const router = Router();

router.post('/ota', async (req, res) => {
  const deviceId = req.headers['device-id'] as string;
  const clientId = req.headers['client-id'] as string;
  const timestamp = req.headers['x-timestamp'] as string;
  const auth = req.headers['authorization'] as string;
  const userAgent = req.headers['user-agent'] as string;

  if (!deviceId || !timestamp) {
    return res.status(400).json({ error: 'missing required headers' });
  }

  // 解析请求体
  const bodyJson = JSON.stringify(req.body);
  const { version: currentVersion, board_type } = req.body?.application || {};
  const boardInfo = req.body?.board || {};

  // 查找设备
  let device = await prisma.device.findUnique({
    where: { deviceId }
  });

  // 设备不存在，自动注册
  if (!device) {
    const deviceSecret = crypto.randomBytes(32).toString('hex');

    device = await prisma.device.create({
      data: {
        deviceId,
        deviceSecret,
        boardType: boardInfo.type,
        boardName: boardInfo.name,
        firmwareVersion: currentVersion,
        ssid: boardInfo.ssid,
        rssi: boardInfo.rssi,
        ipAddress: boardInfo.ip,
        isOnline: true,
        lastSeenAt: new Date()
      }
    });

    // 返回 device_secret
    const firmware = await getLatestFirmware(boardInfo.type);

    return res.json({
      activation: { code: '', message: '设备已注册' },
      device_secret: deviceSecret,
      mqtt: getMqttConfig(deviceId),
      websocket: getWebsocketConfig(),
      server_time: getServerTime(),
      firmware: firmware || { version: currentVersion, url: '', rotate_key: '' }
    });
  }

  // 设备存在，验证签名
  if (!auth || !auth.startsWith('Bearer ')) {
    return res.status(401).json({ error: 'missing authorization' });
  }

  const signature = auth.substring(7);
  if (!verifySignature(device.deviceSecret, timestamp, deviceId, bodyJson, signature)) {
    return res.status(401).json({ error: 'signature invalid' });
  }

  // 更新设备信息
  await prisma.device.update({
    where: { deviceId },
    data: {
      ssid: boardInfo.ssid,
      rssi: boardInfo.rssi,
      ipAddress: boardInfo.ip,
      firmwareVersion: currentVersion,
      isOnline: true,
      lastSeenAt: new Date()
    }
  });

  // 获取最新固件
  const firmware = await getLatestFirmware(boardInfo.type);
  const hasNewVersion = isNewVersionAvailable(currentVersion, firmware?.version);

  return res.json({
    activation: { code: '', message: '' },
    mqtt: getMqttConfig(deviceId),
    websocket: getWebsocketConfig(),
    server_time: getServerTime(),
    firmware: {
      version: firmware?.version || currentVersion,
      url: hasNewVersion ? firmware?.url : '',
      rotate_key: ''
    }
  });
});

// 辅助函数
function getMqttConfig(deviceId: string) {
  return {
    endpoint: process.env.MQTT_ENDPOINT || 'mqtt.example.com',
    client_id: `GID_${deviceId}`,
    username: `device_${deviceId.substring(0, 8)}`,
    password: crypto.randomBytes(16).toString('hex'),
    publish_topic: `device/${deviceId}/status`
  };
}

function getWebsocketConfig() {
  return {
    url: process.env.WEBSOCKET_URL || 'wss://api.example.com/ws/',
    token: 'xxx'
  };
}

function getServerTime() {
  return {
    timestamp: Date.now(),
    timezone: 'Asia/Shanghai',
    timezone_offset: 480
  };
}

async function getLatestFirmware(boardType: string) {
  return prisma.firmwareVersion.findFirst({
    where: { boardType, isActive: true },
    orderBy: { createdAt: 'desc' }
  });
}

function isNewVersionAvailable(current: string, latest: string): boolean {
  if (!current || !latest) return false;
  const currentParts = current.split('.').map(Number);
  const latestParts = latest.split('.').map(Number);
  for (let i = 0; i < Math.max(currentParts.length, latestParts.length); i++) {
    const c = currentParts[i] || 0;
    const l = latestParts[i] || 0;
    if (l > c) return true;
    if (l < c) return false;
  }
  return false;
}

export default router;
```

---

## 五、密钥轮换

当服务端需要更换设备密钥时：

```typescript
// 在 OTA 响应中设置 rotate_key
firmware: {
  version: '2.0.0',
  url: 'https://...',
  rotate_key: 'new_secret_hex'  // 非空表示需要轮换
}
```

设备端处理：
```cpp
// ota.cc - ParseResponse 中
if (cJSON_IsString(rotate_key) && strlen(rotate_key->valuestring) > 0) {
    rotate_key_ = rotate_key->valuestring;
    has_rotated_key_ = true;
}

// CheckVersion 中
if (has_rotated_key_ && !rotate_key_.empty()) {
    SaveDeviceSecret(rotate_key_);  // 保存新密钥
}
```

---

## 六、安全说明

| 攻击场景 | 防护方式 |
|---------|---------|
| 无密钥伪造请求 | 没有 device_secret 算不出正确签名 |
| 截获请求重放 | 时间戳 ±5 分钟过期 |
| 篡改请求体 | 签名包含 body_sha256 |
| 篡改请求头 | 签名绑定 timestamp + device_id |
| 暴力破解 | 设备密钥 64 字符 hex，256 位熵 |
| 密钥泄露 | 后台可踢出设备或重置密钥 |
