# liu-esp-wifi-connect 前端页面 & API 文档

ESP32 WiFi 配置门户的前端页面，提供 WiFi 扫描/连接、已保存网络管理、高级参数配置等功能。

## 页面

| 文件 | 说明 |
|------|------|
| `wifi_configuration.html` | 主配置页面（WiFi 连接 + 高级选项） |
| `wifi_configuration_done.html` | 配置成功提示页，1 秒后自动退出配置模式 |

语言支持：中文（默认）、English。

## 文件清单

| 文件 | 说明 | 大小 |
|------|------|------|
| `wifi_configuration.html` | 主配置页面（已压缩） | ~13 KB |
| `wifi_configuration_done.html` | 配置成功页（已压缩） | ~2.3 KB |
| `wifi_configuration_base.html` | 主配置页原始版本（含 37 种语言） | ~27 KB |
| `wifi_configuration_done_base.html` | 成功页原始版本 | ~3.8 KB |

`*_base.html` 为未压缩的完整版本，保留备用。

## 精简操作

针对嵌入式 Flash 空间有限的场景，对页面做了以下压缩：

**CSS**
- 合并为单行，去除注释和空格
- `background-color` → `background`，`border-radius: 3px` → `border-radius:3px` 等

**JavaScript**
- 删除所有 `// =====` 分隔注释和 JSDoc 注释
- 翻译从 37 种语言精简为 2 种（zh-CN、en-US）
- `languageMap` 只保留中英文映射
- 函数体内的换行和缩进压缩为单行

**HTML**
- 去除多余空行和缩进
- 合并短标签行

压缩效果：主页面 **-52%**，成功页 **-39%**，功能完全不变。

---

## API

### 1. 扫描 WiFi

```
GET /scan
```

**响应**

```json
{
  "support_5g": true,
  "aps": [
    { "ssid": "MyWiFi", "rssi": -45, "authmode": 4 },
    { "ssid": "OpenNet", "rssi": -70, "authmode": 0 }
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `support_5g` | bool | 设备是否支持 5GHz |
| `aps[].ssid` | string | 网络名称 |
| `aps[].rssi` | int | 信号强度（dBm） |
| `aps[].authmode` | int | 加密类型，0 = 开放，其他 = 加密 |

---

### 2. 连接 WiFi

```
POST /submit
Content-Type: application/json
```

**请求体**

```json
{ "ssid": "MyWiFi", "password": "12345678" }
```

**响应**

```json
{ "success": true }
```

连接失败时：

```json
{ "success": false, "error": "Connection failed" }
```

---

### 3. 获取已保存的 WiFi 列表

```
GET /saved/list
```

**响应**

```json
["HomeWiFi", "OfficeWiFi", "PhoneHotspot"]
```

数组顺序即优先级，第一项为默认网络。

---

### 4. 删除已保存的 WiFi

```
GET /saved/delete?index=<N>
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `index` | int | 要删除的网络索引（从 0 开始） |

**响应**

```json
{}
```

---

### 5. 设置默认 WiFi

```
GET /saved/set_default?index=<N>
```

将指定索引的网络移到列表首位（设为默认）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `index` | int | 目标网络索引 |

**响应**

```json
{}
```

---

### 6. 保存高级配置

```
POST /advanced/submit
Content-Type: application/json
```

**请求体**

```json
{
  "ota_url": "https://example.com/firmware.bin",
  "device_secret": "abc123"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `ota_url` | string | 自定义 OTA 固件地址 |
| `device_secret` | string | 设备密钥 |

**响应**

```json
{ "success": true }
```

---

### 7. 获取高级配置

```
GET /advanced/config
```

**响应**

```json
{
  "ota_url": "https://example.com/firmware.bin",
  "device_secret": "abc123"
}
```

字段为当前保存的值，未配置的字段可能为空或不返回。

---

### 8. 退出配置模式

```
POST /exit
```

WiFi 配置成功后由前端自动调用，通知设备退出 AP 配置模式、切换到正常运行状态。

**响应**

HTTP 200 即表示成功，无特定响应体要求。
