# esp32_common

基于 ESP-IDF 的通用 ESP32 设备固件框架。以 ESP32-S3（esp32s3n16r8，16MB Flash / 8MB PSRAM）为默认目标，内置 WiFi 配网、设备激活、OTA 升级、MQTT/HTTP/TCP/UDP/SSL/WebSocket 网络能力，并抽象了 ESP 原生网络与 ML307 / EC801E 蜂窝 AT 模组两种传输路径。

> 本框架由具体产品项目（OTA 设备）沉淀而来，`main/` 中的业务代码可直接作为起点裁剪。

## 特性

- **WiFi 配网**：AP 热点 + Web 配置页，可配置 WiFi、组引导密钥（group_key）、OTA 地址、设备密钥
- **设备激活与 OTA**：
  - 首次激活：携带 `X-Provision-Key`（组密钥）换取 `device_secret`
  - 签名：`SHA256(secret + timestamp + device_id + SHA256(body))`，时间戳不校验新鲜度（时钟由 OTA 响应的 `server_time` 校准）
  - A/B 分区升级：流式下载、进度回调、固件 SHA256 / 大小完整性校验、升级后上报 `upgrade_completed`
  - 密钥轮换：OTA 响应可下发新密钥
- **MQTT**：
  - 统一 `MqttService`：首次建连、断线指数退避重连（1s→60s）、订阅注册与重连自动恢复、通配符消息分发、心跳
  - 三种传输实现：ESP 原生 MQTT / ML307 AT / EC801E AT
  - 设备业务层：订阅 `command` / `config`，支持 `reboot` / `upgrade` / `lock` / `unlock` 命令，状态/事件上报
- **网络抽象**：`NetworkInterface` 统一 ESP 与蜂窝模组的 TCP / UDP / SSL / HTTP / MQTT / WebSocket
- **NVS 设置封装**：`Settings` 读写字符串/整数/布尔，支持单字段擦除
- **板级抽象**：`Board` 单例，提供网络、UUID、功耗控制，更换板卡只改 board 实现

## 目录结构

```
.
├── main/                        # 应用层
│   ├── App.cc                   # 主流程：网络事件 / 激活 / OTA / MQTT 编排
│   ├── ota.cc / ota.h           # 激活、版本检查、签名、固件下载与升级
│   ├── device_mqtt.cc/.h        # 设备 MQTT 业务层
│   ├── board/                   # 板级抽象（wifi_board / my_wifi_pcb_board）
│   ├── common/                  # Settings（NVS）、SystemInfo（MAC/版本/时间）
│   └── OTA.md                   # OTA 协议说明
├── components/
│   ├── liu-esp-network-interface/   # 网络组件：统一接口 + ESP/ML307/EC801E 实现 + MqttService
│   └── liu-esp-wifi-connect/        # WiFi 配网组件（AP + Web 配置页）
├── partitions/16m.csv           # 16MB Flash 分区表（nvs / otadata / ota_0 / ota_1 / assets）
├── test-server/                 # Python OTA 测试服务器（与生产后端行为对齐）
├── tools/nvs_partition_parse.py # NVS 分区解析/生成测试分区工具
├── sdkconfig.defaults*          # 各目标芯片默认配置（sdkconfig 为生成文件，不入库）
└── CMakeLists.txt               # 项目版本（PROJECT_VER）
```

## 环境要求与快速开始

- 芯片：ESP32-S3（默认目标，`idf.py set-target esp32s3`）
- ESP-IDF：6.0.2

```bash
# 激活 ESP-IDF 环境（本机）
source $HOME/.espressif/tools/activate_idf_v6.0.2.sh

# 设置目标芯片（首次或切换芯片时）
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录并监视日志（首次使用需将当前用户加入 dialout 组，或临时授权串口）
idf.py -p /dev/ttyACM0 flash monitor
```

> `sdkconfig` 为构建生成文件，不入库；各芯片默认配置由 `sdkconfig.defaults.*` 提供。

## 配置

- **芯片配置**：`sdkconfig.defaults.esp32s3`（16MB Flash / QIO / 8MB PSRAM 八线 80MHz / 240MHz / WiFi）
- **分区表**：`partitions/16m.csv`
  - `nvs` 0x9000 / `otadata` 0xd000 / `phy_init` 0xf000
  - `ota_0` / `ota_1` 各 3.9MB
  - `assets`（spiffs）8MB
- **OTA 地址**：WiFi 配网页高级选项可设置 `ota_url`；代码内置默认地址见 `main/ota.cc` 的 `CONFIG_OTA_URL`

## 核心架构

```
┌─────────────────────────────────────────────────────┐
│  App（编排层）                                        │
│  网络事件 → 激活任务 → OTA 检查 → 启动 MQTT → 升级调度  │
└──────────────┬──────────────────┬───────────────────┘
               │                  │
               ▼                  ▼
        ┌───────────┐      ┌───────────────┐
        │    Ota    │      │  DeviceMqtt   │  业务层
        │ 激活/升级  │      │ 命令/状态/事件 │
        └───────────┘      └──────┬────────┘
                                  │
                    ┌─────────────▼─────────────┐
                    │       MqttService         │  通用服务层
                    │ 重连/订阅恢复/分发/心跳     │
                    └─────────────┬─────────────┘
                                  │
                    ┌─────────────▼─────────────┐
                    │  Mqtt（传输接口）           │  传输层
                    │ ESP 原生 / ML307 / EC801E  │
                    └───────────────────────────┘
```

分层原则：传输层只做连接；`MqttService` 统一管理连接生命周期与消息分发；`DeviceMqtt` 只承载设备业务语义；`App` 负责编排，通过回调/工厂注入解耦。

## OTA 流程

1. 设备联网后执行版本检查（`POST /api/device_manage/ota`）
2. 未激活：携带 `X-Provision-Key` + 组密钥签名 → 服务器返回 `device_secret`、MQTT/WebSocket 配置、`server_time`、固件信息
3. 已激活：用 `device_secret` 签名 → 服务器返回最新固件版本 / URL / `checksum` / `file_size`
4. 有新版本：下载固件到另一分区（A/B），边下载边做 SHA256，校验大小与哈希后切换启动分区并重启
5. 新固件启动后通过 MQTT 上报 `upgrade_completed`；失败会上报 `upgrade_failed` 并继续运行旧固件

详细协议见 [main/OTA.md](main/OTA.md) 与 `test-server/docs/`。

## MQTT 主题约定

| 主题 | 方向 | 说明 |
|---|---|---|
| `device/{mac}/command` | 服务器 → 设备 | 控制命令（reboot / upgrade / lock / unlock） |
| `device/{mac}/config` | 服务器 → 设备 | 配置下发（预留） |
| `device/{mac}/status` | 设备 → 服务器 | 状态上报 / 心跳 |
| `device/{mac}/event` | 设备 → 服务器 | 事件上报（升级等） |
| `device/{mac}/command_ack` | 设备 → 服务器 | 命令应答 |
| `device/{mac}/location` | 设备 → 服务器 | 位置上报 |

`{mac}` 为设备 MAC 地址（如 `11:22:33:44:55:66`）。

## 测试服务器

`test-server/server.py` 是 Python 实现的 OTA 测试服务器，与生产后端行为对齐：

```bash
cd test-server
python server.py 8080
```

支持首次激活/OTA 检查验签、固件列表/下载（含 checksum 与 file_size）、MQTT/WebSocket/server_time 下发。详见 [test-server/README.md](test-server/README.md)。

## 工具

```bash
# 解析 NVS 分区镜像 / 离线生成示例分区 / 从设备只读导出 NVS
python tools/nvs_partition_parse.py parse <nvs.bin>
python tools/nvs_partition_parse.py make-test
python tools/nvs_partition_parse.py read -p /dev/ttyACM0
```

## 依赖

- `espressif/mqtt`（^1.1.0）—— ESP-MQTT 客户端（IDF 6.0 起为托管组件）
- `espressif/cjson`（^1.7.19）—— cJSON（IDF 6.0 起 `json` 组件更名为托管组件）

依赖由 IDF Component Manager 自动拉取，清单见各组件 `idf_component.yml`。
