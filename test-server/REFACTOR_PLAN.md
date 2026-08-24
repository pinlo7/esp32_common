# IoT 设备管理平台重构计划

## 一、技术栈

```
前端：Vue3 + Element Plus
后端：Express + TypeScript
数据库：PostgreSQL + Prisma
MQTT Broker：EMQX
部署：Docker Compose（云服务器）
```

---

## 二、核心功能

| 功能 | 说明 |
|------|------|
| 用户认证 | JWT 登录 |
| 设备管理 | 设备注册、状态监控、踢出设备 |
| OTA 升级 | 对接 esp32_common OTA API |
| MQTT | 设备状态接收、命令下发 |
| 固件管理 | 版本上传、设备升级 |

---

## 三、设备激活流程

### 自动注册 + 后台踢出

```
1. 设备首次请求 OTA（无 Authorization）
2. 服务端自动生成 device_secret，创建设备记录
3. 响应中返回 device_secret
4. 设备保存 device_secret 到 NVS
5. 后续请求用 device_secret 签名
6. 管理员可在后台踢出设备（删除设备或重置密钥）
```

### 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                        设备首次请求                              │
│  POST /api/device_manage/ota                                    │
│  Headers: Device-Id: 11:22:33:44:55:66 (无 Authorization)       │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        服务端                                    │
│  1. 检查设备是否存在                                              │
│  2. 不存在 → 自动生成 device_secret                              │
│  3. 创建设备记录，关联 device_secret                             │
│  4. 返回响应 + device_secret                                    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        设备端                                    │
│  1. 收到响应，保存 device_secret 到 NVS                          │
│  2. 后续请求用 device_secret 签名                                │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        管理后台                                  │
│  1. 查看已注册设备列表                                            │
│  2. 可选择踢出设备（删除设备或重置密钥）                           │
│  3. 被踢设备下次请求会返回 401，需重新注册                        │
└─────────────────────────────────────────────────────────────────┘
```

---

## 四、API 设备接口

所有设备相关接口统一在 `/api/device_manage/` 下。

### POST /api/device_manage/ota

设备端调用，检查版本 / 注册设备。

**请求头**：
```
Device-Id: MAC 地址
Client-Id: UUID
X-Timestamp: Unix 时间戳
Authorization: Bearer {签名}（首次可省略）
User-Agent: 设备名/版本
```

**请求体**：
```json
{
  "application": { "version": "1.0.0", "elf_sha256": "..." },
  "board": { "type": "wifi", "name": "...", "ssid": "...", "rssi": -55 }
}
```

**响应（首次注册）**：
```json
{
  "activation": {
    "code": "",
    "message": "设备已注册"
  },
  "device_secret": "a1b2c3d4e5f6...",
  "mqtt": { "endpoint": "...", "username": "...", "password": "..." },
  "websocket": { "url": "...", "token": "..." },
  "server_time": { "timestamp": 1719900000000, "timezone": "Asia/Shanghai", "timezone_offset": 480 },
  "firmware": { "version": "1.0.0", "url": "", "rotate_key": "" }
}
```

**响应（已注册，有新版本）**：
```json
{
  "activation": { "code": "", "message": "" },
  "mqtt": { "endpoint": "...", "username": "...", "password": "..." },
  "websocket": { "url": "...", "token": "..." },
  "server_time": { "timestamp": 1719900000000, "timezone": "Asia/Shanghai", "timezone_offset": 480 },
  "firmware": { "version": "2.0.0", "url": "https://...", "rotate_key": "" }
}
```

**响应（被踢出）**：
```json
{
  "error": "device removed",
  "message": "设备已被移除，请重新注册"
}
```

### POST /api/device_manage/command

发送命令到设备。

**请求**：`{ "command": "reboot", "params": {} }`

### GET /api/device_manage/devices

获取设备列表（管理后台）。

### GET /api/device_manage/devices/:id

获取设备详情。

### DELETE /api/device_manage/devices/:id

踢出设备（删除设备记录，设备需重新注册）。

### POST /api/device_manage/devices/:id/reset-secret

重置设备密钥（设备需重新注册）。

---

## 五、签名验证

```
签名原文 = device_secret + timestamp + device_id + body_sha256
签名 = SHA256(签名原文)
```

```typescript
import crypto from 'crypto';

function verifySignature(deviceSecret: string, timestamp: string, deviceId: string, bodyJson: string, signature: string) {
  // 1. 时间戳 ±5 分钟
  if (Math.abs(Date.now() / 1000 - parseInt(timestamp)) > 300) return false;

  // 2. 计算 body hash
  const bodyHash = crypto.createHash('sha256').update(bodyJson).digest('hex');

  // 3. 拼接签名原文
  const text = deviceSecret + timestamp + deviceId + bodyHash;

  // 4. 计算签名
  const expected = crypto.createHash('sha256').update(text).digest('hex');

  // 5. 常量时间比较
  return crypto.timingSafeEqual(Buffer.from(signature, 'hex'), Buffer.from(expected, 'hex'));
}
```

---

## 六、数据库设计

```sql
-- 设备表（每个设备一个独立密钥）
CREATE TABLE devices (
  id SERIAL PRIMARY KEY,
  device_id VARCHAR(32) UNIQUE NOT NULL,      -- MAC 地址
  device_secret VARCHAR(64) NOT NULL,         -- 设备密钥（hex）
  board_type VARCHAR(64),                     -- 型号
  board_name VARCHAR(128),                    -- 设备名称
  firmware_version VARCHAR(32),               -- 固件版本
  ssid VARCHAR(64),                           -- WiFi SSID
  rssi INTEGER,                               -- 信号强度
  ip_address VARCHAR(45),                     -- IP 地址
  is_online BOOLEAN DEFAULT FALSE,            -- 是否在线
  last_seen_at TIMESTAMPTZ,                   -- 最后上线
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- 固件版本表
CREATE TABLE firmware_versions (
  id SERIAL PRIMARY KEY,
  version VARCHAR(32) NOT NULL,               -- 版本号
  url TEXT NOT NULL,                          -- 下载地址
  board_type VARCHAR(64),                     -- 适用型号
  is_active BOOLEAN DEFAULT TRUE,
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- 设备状态表
CREATE TABLE device_status (
  id SERIAL PRIMARY KEY,
  device_id VARCHAR(32) NOT NULL,
  data JSONB NOT NULL,                        -- 状态数据
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- 索引
CREATE INDEX idx_devices_device_id ON devices(device_id);
CREATE INDEX idx_device_status_device_id ON device_status(device_id);
```

### 说明

- 每个设备独立一个 `device_secret`，简化管理
- 踢出设备 = 删除记录，设备需重新注册获取新密钥
- 重置密钥 = 更新 `device_secret`，设备需重新获取

---

## 七、MQTT Topic

```
# 设备 → 服务器
device/{device_id}/status      # 状态上报
device/{device_id}/location    # 位置上报
device/{device_id}/event       # 事件上报

# 服务器 → 设备
device/{device_id}/command     # 控制指令
device/{device_id}/config      # 配置下发
```

---

## 八、API 清单

### 认证

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/auth/login` | 用户登录 |

### 设备管理（/api/device_manage/）

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/device_manage/ota` | OTA 检查 / 设备注册 |
| GET | `/api/device_manage/devices` | 设备列表 |
| GET | `/api/device_manage/devices/:id` | 设备详情 |
| DELETE | `/api/device_manage/devices/:id` | 踢出设备 |
| POST | `/api/device_manage/devices/:id/reset-secret` | 重置密钥 |
| POST | `/api/device_manage/devices/:id/command` | 发送命令 |

### 固件管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/firmware` | 固件列表 |
| POST | `/api/firmware` | 上传固件 |
| DELETE | `/api/firmware/:id` | 删除固件 |

---

## 九、前端页面

| 页面 | 功能 |
|------|------|
| 登录页 | JWT 登录 |
| Dashboard | 设备总数、在线数 |
| 设备列表 | 搜索、筛选、踢出设备、重置密钥 |
| 设备详情 | 状态、OTA 升级、发送命令 |
| 固件管理 | 版本列表、上传 |

---

## 十、项目结构

```
my-devices-project/
├── backend/
│   ├── src/
│   │   ├── index.ts
│   │   ├── config.ts
│   │   ├── db.ts
│   │   ├── routes/
│   │   │   ├── auth.ts
│   │   │   ├── device_manage.ts    # 设备管理（ota、devices、command）
│   │   │   └── firmware.ts
│   │   ├── services/
│   │   │   ├── ota.ts
│   │   │   ├── mqtt.ts
│   │   │   └── auth.ts
│   │   ├── middleware/
│   │   │   └── auth.ts
│   │   └── utils/
│   │       └── crypto.ts
│   ├── prisma/
│   │   └── schema.prisma
│   └── package.json
│
├── frontend/
│   ├── src/
│   │   ├── views/
│   │   ├── components/
│   │   ├── stores/
│   │   └── router/
│   └── package.json
│
├── docker-compose.yml
└── .env
```

---

## 十一、Docker Compose

```yaml
services:
  postgres:
    image: postgres:16-alpine
    environment:
      POSTGRES_USER: ${POSTGRES_USER}
      POSTGRES_PASSWORD: ${POSTGRES_PASSWORD}
      POSTGRES_DB: ${POSTGRES_DB}
    volumes:
      - postgres_data:/var/lib/postgresql/data

  emqx:
    image: emqx/emqx:5
    environment:
      EMQX_DASHBOARD__DEFAULT_USERNAME: ${EMQX_USERNAME:-admin}
      EMQX_DASHBOARD__DEFAULT_PASSWORD: ${EMQX_PASSWORD:-public}
    ports:
      - "1883:1883"
      - "8083:8083"
      - "18083:18083"

  backend:
    build: ./backend
    environment:
      DATABASE_URL: postgresql://${POSTGRES_USER}:${POSTGRES_PASSWORD}@postgres:5432/${POSTGRES_DB}
      JWT_SECRET: ${JWT_SECRET}
      MQTT_URL: mqtt://emqx:1883
    depends_on:
      - postgres
      - emqx
    ports:
      - "3000:3000"

  frontend:
    build: ./frontend
    ports:
      - "80:80"
    depends_on:
      - backend

volumes:
  postgres_data:
```

---

## 十二、ESP32 设备端对接

设备端已实现（`ota.h` / `ota.cc`），配置：

```cpp
Settings settings("wifi", true);
settings.SetString("ota_url", "http://your-server.com/api/device_manage/ota");
```

### 设备端流程

1. 首次请求 OTA（无 Authorization）
2. 服务端返回 `device_secret` + 固件信息
3. 设备保存 `device_secret` 到 NVS
4. 后续请求用 `device_secret` 签名
5. 服务端验证签名，返回固件信息 + MQTT 配置

### 设备端代码改动

需要在 `ota.h` / `ota.cc` 中：

1. 检查响应中是否有 `device_secret` 字段
2. 如果有，保存到 NVS（`SaveDeviceSecret()`）
3. 后续请求自动签名

---

## 十三、实施步骤

| 阶段 | 时间 | 内容 |
|------|------|------|
| 一 | 2 天 | Express + Prisma + 用户认证 + 设备 CRUD |
| 二 | 1.5 天 | OTA 接口 + 自动注册 + 签名验证 |
| 三 | 1 天 | EMQX 集成 + MQTT 状态接收 + 命令下发 |
| 四 | 1.5 天 | 前端精简 + 设备管理页面 |
| 五 | 1 天 | 云服务器部署 + ESP32 联调 |

总计约 7 天。
