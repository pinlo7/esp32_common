# OTA 模块文档

## 一、概述

`Ota` 类实现设备固件的空中升级（Over-The-Air），包括：

- 设备激活与密钥绑定
- 固件版本检查
- 固件下载与升级
- 密钥轮换
- 服务端时间同步

---

## 二、类接口

### 头文件 `ota.h`

```cpp
class Ota {
public:
    Ota();   // 构造时从 NVS 加载 device_key
    ~Ota();

    // 核心功能
    esp_err_t CheckVersion();           // 检查版本并获取 OTA 响应
    bool StartUpgrade(callback);        // 开始升级
    static bool Upgrade(url, callback); // 静态方法：执行升级
    void MarkCurrentVersionValid();     // 标记固件有效（取消回滚）

    // 状态查询
    bool HasActivationCode();
    bool HasNewVersion();
    bool HasMqttConfig();
    bool HasWebsocketConfig();
    bool HasServerTime();
    bool HasRotatedKey();

    // 数据访问
    const std::string& GetActivationCode();
    const std::string& GetActivationMessage();
    const std::string& GetFirmwareVersion();
    const std::string& GetFirmwareUrl();
    const std::string& GetCurrentVersion();
};
```

### 依赖

| 依赖 | 用途 |
|------|------|
| `Board` | 获取设备信息、网络接口、UUID |
| `SystemInfo` | MAC 地址、User-Agent |
| `Settings` | NVS 读写 |
| `mbedtls` | SHA256 签名 |
| `cJSON` | JSON 解析 |

---

## 三、整体流程

### 1. 设备激活流程

```
设备上电 → Ota 构造函数
   │
   ▼
NVS 中有 device_key？
   │
   ├── 有 → 跳过激活，直接 OTA
   │
   └── 无 → 首次激活流程
              │
              ▼
        CheckVersion()  （无 Authorization 头）
              │
              ▼
        服务端返回 { activation: { code: "123456" } }
              │
              ▼
        屏幕显示激活码 "123456"
        用户在 APP 中输入激活码
              │
              ▼
        APP 调用 POST /api/bind-device/
        服务端验证激活码，生成 device_key
        返回 device_key 给 APP
              │
              ▼
        APP 通过蓝牙/局域网将 device_key 下发给设备
        设备调用 SaveDeviceSecret() 存入 NVS
              │
              ▼
        激活完成，进入 OTA 流程
```

### 2. OTA 请求流程

```
设备组装请求
   │
   ├── Header: Device-Id (MAC)
   ├── Header: Client-Id (UUID)
   ├── Header: X-Timestamp (Unix 秒)
   ├── Header: Authorization: Bearer {签名}
   ├── Header: User-Agent
   ├── Body: { application, board }
   │
   ▼
POST /api/ota/
   │
   ▼
服务端验证签名 → 通过 → 检查固件
   │
   ├── 有新固件 → 返回 firmware.url
   └── 无新固件 → 返回当前版本
```

### 3. 密钥轮换流程

```
OTA 响应中 firmware.rotate_key = "new_key_hex"
   │
   ▼
设备收到后用旧密钥完成本次更新
   │
   ▼
SaveDeviceSecret(rotate_key)  写入 NVS
   │
   ▼
下次 OTA 请求使用新密钥签名
```

---

## 四、签名算法

### 签名原文格式

```
text = device_key + timestamp_str + device_id + body_sha256
```

| 组成部分 | 说明 | 示例 |
|---------|------|------|
| `device_key` | 设备密钥（hex） | `a1b2c3d4e5f6...` |
| `timestamp_str` | Unix 秒级时间戳 | `"1719900000"` |
| `device_id` | Device-Id 头的值 | `"11:22:33:44:55:66"` |
| `body_sha256` | 请求体 JSON 的 SHA256 | `"e3b0c44298fc..."` |

### 最终签名

```
signature = SHA256(device_key + timestamp_str + device_id + body_sha256)
Authorization: Bearer {signature}
```

### C++ 实现

```cpp
std::string Ota::Sha256Hex(const std::string& input) {
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)input.c_str(), input.length(), hash, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
    return std::string(hex);
}

std::string Ota::CalculateSignature(const std::string& body_json, const std::string& timestamp) {
    std::string body_hash = Sha256Hex(body_json);
    std::string device_id = SystemInfo::GetMacAddress();
    std::string text = device_key_ + timestamp + device_id + body_hash;
    return Sha256Hex(text);
}
```

---

## 五、OTA 接口

### 请求

```
POST /api/ota/
Content-Type: application/json
Device-Id: 11:22:33:44:55:66
Client-Id: 7b94d69a-9808-4c59-9c9b-704333b38aff
X-Timestamp: 1719900000
Authorization: Bearer 3a7dbf4e8c2a1b5f...
User-Agent: xingzhi-cube-1.54tft-wifi/1.5.6
Accept-Language: zh-CN
```

**请求头**

| Header | 必需 | 说明 |
|--------|:---:|------|
| `Device-Id` | 是 | 设备 MAC 地址 |
| `Client-Id` | 是 | UUID v4，每次启动生成 |
| `User-Agent` | 是 | `设备名/固件版本` |
| `X-Timestamp` | 是 | Unix 时间戳（秒） |
| `Authorization` | 激活后 | `Bearer {签名}` |
| `Accept-Language` | 否 | 语言偏好 `zh-CN` |

**请求体**

```json
{
  "application": {
    "version": "1.5.6",
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

> `board` 字段由 `Board::GetBoardJson()` 提供，不同板型（WiFi/4G 等）返回不同内容。

### 成功响应（200）

```json
{
  "activation": {
    "code": "123456",
    "message": "请在 APP 中输入激活码"
  },
  "mqtt": {
    "endpoint": "mqtt.example.com",
    "client_id": "GID_test@@@device-id@@@uuid",
    "username": "device_12345",
    "password": "password",
    "publish_topic": "device-server"
  },
  "websocket": {
    "url": "wss://api.example.com/xiaozhi/v1/",
    "token": "test-token"
  },
  "server_time": {
    "timestamp": 1719900000000,
    "timezone": "Asia/Shanghai",
    "timezone_offset": -480
  },
  "firmware": {
    "version": "1.5.7",
    "url": "https://cdn.example.com/firmware/1.5.7.bin",
    "rotate_key": ""
  }
}
```

**响应字段说明**

| 字段 | 类型 | 说明 |
|------|------|------|
| `activation.code` | string | 首次激活时返回，6 位数字 |
| `activation.message` | string | 激活提示信息 |
| `mqtt.*` | object | MQTT 连接信息，存入 NVS `"mqtt"` 命名空间 |
| `websocket.*` | object | WebSocket 连接信息，存入 NVS `"websocket"` |
| `server_time.timestamp` | long | 服务端时间戳（毫秒），用于设备校时 |
| `server_time.timezone` | string | 时区名称 |
| `server_time.timezone_offset` | int | 与 UTC 的偏移（分钟） |
| `firmware.version` | string | 最新固件版本 |
| `firmware.url` | string | 固件下载 URL，无更新时为空 |
| `firmware.rotate_key` | string | 新设备密钥，非空时需更新 NVS |

### 错误响应

| 状态码 | 说明 |
|:---:|------|
| 400 | 缺少必需 Header 或请求体格式错误 |
| 401 | 签名无效 / 时间戳过期 / 设备未注册 |
| 404 | 设备未激活且无可用激活码 |
| 500 | 服务器内部错误 |

---

## 六、NVS 存储结构

| 命名空间 | 键 | 类型 | 说明 |
|---------|-----|------|------|
| `device` | `device_key` | string | 设备密钥（hex） |
| `wifi` | `ota_url` | string | OTA 服务地址 |
| `mqtt` | `endpoint` | string | MQTT 服务器 |
| `mqtt` | `username` | string | MQTT 用户名 |
| `mqtt` | `password` | string | MQTT 密码 |
| `websocket` | `url` | string | WebSocket 地址 |
| `websocket` | `token` | string | WebSocket Token |

---

## 七、使用示例

### 基本用法

```cpp
#include "ota.h"

Ota ota;

// 1. 检查版本
esp_err_t ret = ota.CheckVersion();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Check version failed");
    return;
}

// 2. 首次激活（无 device_key）
if (ota.HasActivationCode()) {
    ESP_LOGI(TAG, "Activation code: %s", ota.GetActivationCode().c_str());
    // 在屏幕上显示激活码，等待 APP 下发 device_key
    return;
}

// 3. 有新版本则升级
if (ota.HasNewVersion()) {
    ESP_LOGI(TAG, "New version: %s", ota.GetFirmwareVersion().c_str());
    bool ok = ota.StartUpgrade([](int progress, size_t speed) {
        ESP_LOGI(TAG, "Upgrade: %d%%, %u B/s", progress, speed);
    });
    if (ok) {
        esp_restart();
    }
}

// 4. 标记当前固件有效
ota.MarkCurrentVersionValid();
```

### 启动时标准流程

```cpp
void app_main() {
    Ota ota;

    // 标记固件有效（OTA 后首次启动）
    ota.MarkCurrentVersionValid();

    // 检查版本
    esp_err_t ret = ota.CheckVersion();
    if (ret != ESP_OK) return;

    // 处理激活
    if (ota.HasActivationCode()) {
        show_activation_code(ota.GetActivationCode());
        return;
    }

    // 处理升级
    if (ota.HasNewVersion()) {
        ota.StartUpgrade(on_upgrade_progress);
        esp_restart();
    }

    // 同步时间
    if (ota.HasServerTime()) {
        ESP_LOGI(TAG, "Time synced");
    }

    // 保存 MQTT/WebSocket 配置
    if (ota.HasMqttConfig()) {
        // MQTT 配置已自动存入 NVS
    }
}
```

---

## 八、安全说明

| 攻击场景 | 防护方式 |
|---------|---------|
| 无密钥伪造请求 | 没有 device_key 算不出正确签名 |
| 截获请求重放 | 时间戳 ±5 分钟过期 |
| 篡改请求体 | 签名包含 body_sha256 |
| 篡改请求头 | 签名绑定 timestamp + device_id |
| 暴力破解激活码 | 5 次错误锁定 + 5 分钟过期 |
| 密钥长期使用泄露 | 支持 OTA 响应下发 rotate_key 轮换 |

---

## 九、Board 类依赖

`Ota` 依赖 `Board` 提供以下接口：

```cpp
class Board {
public:
    static Board& GetInstance();
    std::string GetUuid();                    // Client-Id
    std::string GetBoardType();               // board.type
    std::string GetBoardName();               // board.name
    std::shared_ptr<Network> GetNetwork();    // HTTP 客户端
    std::shared_ptr<Wifi> GetWifi();          // WiFi 信息
};
```

如果 `Board` 类缺少这些接口，需要补充实现。