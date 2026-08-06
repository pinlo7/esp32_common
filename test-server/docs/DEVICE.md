# ESP32 设备端对接

## 一、概述

本文档说明 ESP32 设备如何对接 OTA 服务端。

设备端代码位于 `esp32_common/main/ota.h` 和 `ota.cc`。

---

## 二、配置

### OTA URL

在 NVS 中配置 OTA 服务地址：

```cpp
Settings settings("wifi", true);
settings.SetString("ota_url", "http://your-server.com/api/device_manage/ota");
```

或在代码中硬编码：

```cpp
#define CONFIG_OTA_URL "http://your-server.com/api/device_manage/ota"
```

---

## 三、设备端流程

### 首次注册

```
1. 设备启动，读取 NVS 中的 device_key
2. device_key 为空 → 首次请求
3. 请求 POST /api/device_manage/ota（无 Authorization）
4. 服务端返回 device_secret
5. 设备保存 device_secret 到 NVS
6. 后续请求自动签名
```

### 正常 OTA

```
1. 设备启动，读取 NVS 中的 device_key
2. device_key 存在 → 签名请求
3. 请求 POST /api/device_manage/ota（带 Authorization）
4. 服务端验证签名
5. 返回固件信息
6. 如果有新版本 → 下载并升级
```

### 被踢出

```
1. 设备签名请求
2. 服务端返回 401 + "device removed"
3. 设备清除本地 device_key
4. 重新请求（无 Authorization）
5. 自动重新注册
```

---

## 四、代码实现

### ota.h

```cpp
#pragma once
#ifndef _OTA_H_
#define _OTA_H_

#include <string>
#include <functional>
#include <vector>
#include <esp_err.h>

class Ota {
public:
    Ota();
    ~Ota();

    // 核心功能
    esp_err_t CheckVersion();
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    // 状态查询
    bool HasNewVersion() const { return has_new_version_; }
    bool HasMqttConfig() const { return has_mqtt_config_; }
    bool HasServerTime() const { return has_server_time_; }
    bool HasRotatedKey() const { return has_rotated_key_; }

    // 数据访问
    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetCurrentVersion() const { return current_version_; }

private:
    // NVS 密钥管理
    std::string GetOtaUrl();
    std::string GetDeviceSecret();
    bool SaveDeviceSecret(const std::string& key);
    bool HasDeviceSecret() const;

    // 签名计算
    std::string CalculateSignature(const std::string& body_json, const std::string& timestamp);
    std::string Sha256Hex(const std::string& input);
    std::string GetTimestamp();

    // HTTP 请求
    std::string BuildRequestBody();
    esp_err_t ParseResponse(const std::string& response);

    // 状态标志
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_server_time_ = false;
    bool has_rotated_key_ = false;

    // 数据
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string device_key_;
    std::string rotate_key_;

    // 版本比较
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
};

#endif // _OTA_H_
```

### ota.cc 关键部分

```cpp
// 处理 device_secret 下发
esp_err_t Ota::ParseResponse(const std::string& response) {
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 检查是否有 device_secret（首次注册）
    cJSON* device_secret = cJSON_GetObjectItem(root, "device_secret");
    if (cJSON_IsString(device_secret) && strlen(device_secret->valuestring) > 0) {
        // 保存到 NVS
        SaveDeviceSecret(device_secret->valuestring);
        ESP_LOGI(TAG, "Device secret saved");
    }

    // 检查是否被踢出
    cJSON* error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(error)) {
        if (strcmp(error->valuestring, "device removed") == 0) {
            // 清除本地密钥
            SaveDeviceSecret("");
            ESP_LOGW(TAG, "Device removed, clearing secret");
        }
    }

    // 解析固件信息
    cJSON* firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        // ... 解析版本和 URL
    }

    // 解析 MQTT 配置
    cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        // ... 保存到 NVS
        has_mqtt_config_ = true;
    }

    cJSON_Delete(root);
    return ESP_OK;
}
```

---

## 五、签名实现

```cpp
std::string Ota::Sha256Hex(const std::string& input) {
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)input.c_str(), input.length(), hash, 0);

    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hex + i * 2, "%02x", hash[i]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

std::string Ota::GetTimestamp() {
    time_t now;
    time(&now);
    return std::to_string(now);
}

std::string Ota::CalculateSignature(const std::string& body_json, const std::string& timestamp) {
    if (!HasDeviceSecret()) {
        return "";
    }

    // 1. 计算请求体 hash
    std::string body_hash = Sha256Hex(body_json);

    // 2. 拼接签名原文
    std::string device_id = SystemInfo::GetMacAddress();
    std::string text = device_key_ + timestamp + device_id + body_hash;

    // 3. 计算签名
    return Sha256Hex(text);
}
```

---

## 六、使用示例

```cpp
#include "ota.h"

void CheckAndUpdate() {
    Ota ota;

    // 标记当前固件有效
    ota.MarkCurrentVersionValid();

    // 检查版本
    esp_err_t ret = ota.CheckVersion();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Check version failed");
        return;
    }

    // 检查是否有新版本
    if (ota.HasNewVersion()) {
        ESP_LOGI(TAG, "New version: %s", ota.GetFirmwareVersion().c_str());

        // 开始升级
        bool ok = ota.StartUpgrade([](int progress, size_t speed) {
            ESP_LOGI(TAG, "Progress: %d%%, Speed: %u B/s", progress, speed);
        });

        if (ok) {
            ESP_LOGI(TAG, "Upgrade successful, restarting...");
            esp_restart();
        }
    }

    // MQTT 配置已自动保存到 NVS
    if (ota.HasMqttConfig()) {
        ESP_LOGI(TAG, "MQTT config updated");
    }
}
```

---

## 七、NVS 存储

| 命名空间 | 键 | 类型 | 说明 |
|---------|-----|------|------|
| device | device_key | string | 设备密钥（hex） |
| wifi | ota_url | string | OTA 服务地址 |
| mqtt | endpoint | string | MQTT 服务器 |
| mqtt | username | string | MQTT 用户名 |
| mqtt | password | string | MQTT 密码 |
| websocket | url | string | WebSocket 地址 |
| websocket | token | string | WebSocket Token |

---

## 八、调试

### 日志输出

```
I (5158) Ota: Current version: 1.0.0
I (5168) Ota: Device secret saved
I (5178) Ota: MQTT config updated
I (5188) Ota: New version: 2.0.0
I (5198) Ota: Upgrading from http://...
I (5208) Ota: Progress: 10%, Speed: 10240 B/s
I (5218) Ota: Upgrade successful
```

### 常见问题

| 问题 | 原因 | 解决 |
|------|------|------|
| 401 Unauthorized | 签名错误或密钥无效 | 检查 device_key 是否正确 |
| 设备反复注册 | NVS 丢失 | 检查 NVS 分区配置 |
| OTA 失败 | 固件 URL 无法访问 | 检查网络和 URL |
| 时间同步失败 | 服务器时间错误 | 检查 server_time 字段 |