# 数据库设计

## 一、ER 图

```
┌─────────────┐       ┌─────────────────┐       ┌─────────────────┐
│    users    │       │     devices     │       │    firmware     │
├─────────────┤       ├─────────────────┤       ├─────────────────┤
│ id (PK)     │       │ id (PK)         │       │ id (PK)         │
│ username    │       │ device_id (UK)  │       │ version         │
│ password    │       │ device_secret   │       │ url             │
│ role        │       │ board_type      │       │ board_type      │
│ created_at  │       │ firmware_version│       │ is_active       │
└─────────────┘       │ is_online       │       │ created_at      │
                      │ last_seen_at    │       └─────────────────┘
                      │ created_at      │
                      └────────┬────────┘
                               │
                               │ 1:N
                               ▼
                      ┌─────────────────┐
                      │ device_status   │
                      ├─────────────────┤
                      │ id (PK)         │
                      │ device_id (FK)  │
                      │ data (JSONB)    │
                      │ created_at      │
                      └─────────────────┘
```

---

## 二、表结构

### users - 用户表

```sql
CREATE TABLE users (
  id SERIAL PRIMARY KEY,
  username VARCHAR(64) UNIQUE NOT NULL,
  password VARCHAR(128) NOT NULL,           -- bcrypt 加密
  role VARCHAR(32) DEFAULT 'viewer',        -- admin / operator / viewer
  created_at TIMESTAMPTZ DEFAULT NOW()
);
```

| 字段 | 类型 | 说明 |
|------|------|------|
| id | SERIAL | 主键 |
| username | VARCHAR(64) | 用户名，唯一 |
| password | VARCHAR(128) | bcrypt 加密密码 |
| role | VARCHAR(32) | 角色：admin / operator / viewer |
| created_at | TIMESTAMPTZ | 创建时间 |

---

### devices - 设备表

```sql
CREATE TABLE devices (
  id SERIAL PRIMARY KEY,
  device_id VARCHAR(32) UNIQUE NOT NULL,    -- MAC 地址
  device_secret VARCHAR(64) NOT NULL,       -- 设备密钥（hex）
  board_type VARCHAR(64),                   -- 设备型号
  board_name VARCHAR(128),                  -- 设备名称
  firmware_version VARCHAR(32),             -- 当前固件版本
  ssid VARCHAR(64),                         -- WiFi SSID
  rssi INTEGER,                             -- 信号强度（dBm）
  ip_address VARCHAR(45),                   -- IP 地址
  is_online BOOLEAN DEFAULT FALSE,          -- 是否在线
  last_seen_at TIMESTAMPTZ,                 -- 最后上线时间
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_devices_device_id ON devices(device_id);
CREATE INDEX idx_devices_is_online ON devices(is_online);
```

| 字段 | 类型 | 说明 |
|------|------|------|
| id | SERIAL | 主键 |
| device_id | VARCHAR(32) | 设备 MAC 地址，唯一 |
| device_secret | VARCHAR(64) | 设备密钥（hex），用于签名验证 |
| board_type | VARCHAR(64) | 设备型号（如 "wifi"） |
| board_name | VARCHAR(128) | 设备名称 |
| firmware_version | VARCHAR(32) | 当前固件版本 |
| ssid | VARCHAR(64) | 当前连接的 WiFi SSID |
| rssi | INTEGER | WiFi 信号强度（dBm） |
| ip_address | VARCHAR(45) | 设备 IP 地址 |
| is_online | BOOLEAN | 是否在线 |
| last_seen_at | TIMESTAMPTZ | 最后上线时间 |
| created_at | TIMESTAMPTZ | 注册时间 |

---

### firmware_versions - 固件版本表

```sql
CREATE TABLE firmware_versions (
  id SERIAL PRIMARY KEY,
  version VARCHAR(32) NOT NULL,             -- 版本号
  url TEXT NOT NULL,                        -- 下载地址
  board_type VARCHAR(64),                   -- 适用型号
  file_size INTEGER,                        -- 文件大小（字节）
  checksum VARCHAR(64),                     -- SHA256 校验
  is_active BOOLEAN DEFAULT TRUE,           -- 是否启用
  created_at TIMESTAMPTZ DEFAULT NOW()
);
```

| 字段 | 类型 | 说明 |
|------|------|------|
| id | SERIAL | 主键 |
| version | VARCHAR(32) | 版本号（如 "2.0.0"） |
| url | TEXT | 固件下载 URL |
| board_type | VARCHAR(64) | 适用设备型号 |
| file_size | INTEGER | 文件大小（字节） |
| checksum | VARCHAR(64) | SHA256 校验和 |
| is_active | BOOLEAN | 是否启用 |
| created_at | TIMESTAMPTZ | 创建时间 |

---

### device_status - 设备状态表

```sql
CREATE TABLE device_status (
  id SERIAL PRIMARY KEY,
  device_id VARCHAR(32) NOT NULL,           -- 设备 MAC
  data JSONB NOT NULL,                      -- 状态数据
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_device_status_device_id ON device_status(device_id);
CREATE INDEX idx_device_status_created_at ON device_status(created_at);
```

| 字段 | 类型 | 说明 |
|------|------|------|
| id | SERIAL | 主键 |
| device_id | VARCHAR(32) | 设备 MAC 地址 |
| data | JSONB | 状态数据（JSON） |
| created_at | TIMESTAMPTZ | 记录时间 |

**data 示例**：
```json
{
  "engine_status": "on",
  "battery_voltage": 12.5,
  "signal_strength": 85,
  "lock_status": "locked",
  "latitude": 22.5431,
  "longitude": 114.0579
}
```

---

## 三、Prisma Schema

```prisma
generator client {
  provider = "prisma-client-js"
}

datasource db {
  provider = "postgresql"
  url      = env("DATABASE_URL")
}

model User {
  id        Int      @id @default(autoincrement())
  username  String   @unique
  password  String
  role      String   @default("viewer")
  createdAt DateTime @default(now()) @map("created_at")

  @@map("users")
}

model Device {
  id              Int       @id @default(autoincrement())
  deviceId        String    @unique @map("device_id")
  deviceSecret    String    @map("device_secret")
  boardType       String?   @map("board_type")
  boardName       String?   @map("board_name")
  firmwareVersion String?   @map("firmware_version")
  ssid            String?
  rssi            Int?
  ipAddress       String?   @map("ip_address")
  isOnline        Boolean   @default(false) @map("is_online")
  lastSeenAt      DateTime? @map("last_seen_at")
  createdAt       DateTime  @default(now()) @map("created_at")

  statuses DeviceStatus[]

  @@map("devices")
}

model FirmwareVersion {
  id        Int      @id @default(autoincrement())
  version   String
  url       String
  boardType String?  @map("board_type")
  fileSize  Int?     @map("file_size")
  checksum  String?
  isActive  Boolean  @default(true) @map("is_active")
  createdAt DateTime @default(now()) @map("created_at")

  @@map("firmware_versions")
}

model DeviceStatus {
  id        Int      @id @default(autoincrement())
  deviceId  String   @map("device_id")
  data      Json
  createdAt DateTime @default(now()) @map("created_at")

  device Device @relation(fields: [deviceId], references: [deviceId])

  @@index([deviceId])
  @@index([createdAt])
  @@map("device_status")
}
```

---

## 四、种子数据

```typescript
// prisma/seed.ts
import { PrismaClient } from '@prisma/client';
import bcrypt from 'bcrypt';

const prisma = new PrismaClient();

async function main() {
  // 创建管理员账号
  const hashedPassword = await bcrypt.hash('admin123', 10);
  await prisma.user.upsert({
    where: { username: 'admin' },
    update: {},
    create: {
      username: 'admin',
      password: hashedPassword,
      role: 'admin',
    },
  });

  // 创建示例固件版本
  await prisma.firmwareVersion.create({
    data: {
      version: '1.0.0',
      url: 'https://example.com/firmware/1.0.0.bin',
      boardType: 'wifi',
      isActive: true,
    },
  });

  console.log('Seed data created');
}

main()
  .catch(console.error)
  .finally(() => prisma.$disconnect());
```

---

## 五、数据库维护

### 备份

```bash
# 备份数据库
docker compose exec postgres pg_dump -U devices devices_db > backup.sql

# 恢复数据库
docker compose exec -T postgres psql -U devices devices_db < backup.sql
```

### 清理旧状态数据

```sql
-- 删除 30 天前的状态数据
DELETE FROM device_status WHERE created_at < NOW() - INTERVAL '30 days';
```