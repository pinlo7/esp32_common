# MQTT 模块设计

## 一、概述

MQTT 模块负责设备与服务器之间的实时通信：

- 设备状态上报
- 设备位置上报
- 服务器命令下发
- 配置下发

---

## 二、EMQX 配置

### Docker Compose

```yaml
emqx:
  image: emqx/emqx:5
  environment:
    EMQX_NAME: devices_emqx
    EMQX_DASHBOARD__DEFAULT_USERNAME: ${EMQX_USERNAME:-admin}
    EMQX_DASHBOARD__DEFAULT_PASSWORD: ${EMQX_PASSWORD:-public}
  ports:
    - "1883:1883"       # MQTT TCP
    - "8083:8083"       # MQTT WebSocket
    - "18083:18083"     # Dashboard
  volumes:
    - emqx_data:/opt/emqx/data
    - emqx_log:/opt/emqx/log
```

### Dashboard

访问 `http://your-server:18083`，使用配置的用户名密码登录。

功能：
- 查看连接的客户端
- 查看 Topic 订阅
- 发布测试消息
- 查看统计信息

---

## 三、Topic 设计

### 命名规范

```
device/{device_id}/{type}
```

| Topic | 方向 | 说明 |
|-------|------|------|
| `device/{device_id}/status` | 设备 → 服务器 | 状态上报 |
| `device/{device_id}/location` | 设备 → 服务器 | 位置上报 |
| `device/{device_id}/event` | 设备 → 服务器 | 事件上报 |
| `device/{device_id}/command` | 服务器 → 设备 | 控制指令 |
| `device/{device_id}/config` | 服务器 → 设备 | 配置下发 |

### 示例

```
device/11:22:33:44:55:66/status
device/11:22:33:44:55:66/command
```

---

## 四、数据格式

### 状态上报

```json
{
  "engine_status": "on",
  "battery_voltage": 12.5,
  "signal_strength": 85,
  "lock_status": "locked",
  "temperature": 25.5,
  "humidity": 60
}
```

### 位置上报

```json
{
  "latitude": 22.5431,
  "longitude": 114.0579,
  "speed": 60.5,
  "direction": 180,
  "altitude": 50
}
```

### 事件上报

```json
{
  "event": "alarm",
  "level": "warning",
  "message": "电池电量低",
  "data": {
    "voltage": 10.5
  }
}
```

### 命令下发

```json
{
  "command": "reboot",
  "request_id": "uuid",
  "params": {}
}
```

### 配置下发

```json
{
  "report_interval": 60,
  "location_interval": 30,
  "alarm_threshold": 11.0
}
```

---

## 五、后端实现

### MQTT 服务

```typescript
// services/mqtt.ts
import mqtt from 'mqtt';
import { prisma } from '../db';

class MqttService {
  private client: mqtt.MqttClient | null = null;

  connect() {
    const url = process.env.MQTT_URL || 'mqtt://localhost:1883';
    const username = process.env.MQTT_USERNAME || 'backend';
    const password = process.env.MQTT_PASSWORD || 'backend_password';

    this.client = mqtt.connect(url, {
      username,
      password,
      clientId: `backend_${Date.now()}`
    });

    this.client.on('connect', () => {
      console.log('MQTT connected');
      this.client!.subscribe('device/+/status');
      this.client!.subscribe('device/+/location');
      this.client!.subscribe('device/+/event');
    });

    this.client.on('message', (topic, message) => {
      this.handleMessage(topic, message);
    });

    this.client.on('error', (err) => {
      console.error('MQTT error:', err);
    });
  }

  private async handleMessage(topic: string, message: Buffer) {
    const parts = topic.split('/');
    if (parts.length < 3) return;

    const deviceId = parts[1];
    const type = parts[2];

    try {
      const data = JSON.parse(message.toString());

      switch (type) {
        case 'status':
          await this.handleStatus(deviceId, data);
          break;
        case 'location':
          await this.handleLocation(deviceId, data);
          break;
        case 'event':
          await this.handleEvent(deviceId, data);
          break;
      }
    } catch (err) {
      console.error(`Failed to handle MQTT message: ${err}`);
    }
  }

  private async handleStatus(deviceId: string, data: any) {
    // 保存状态数据
    await prisma.deviceStatus.create({
      data: {
        deviceId,
        data
      }
    });

    // 更新设备在线状态
    await prisma.device.update({
      where: { deviceId },
      data: {
        isOnline: true,
        lastSeenAt: new Date()
      }
    });

    // 通过 WebSocket 推送到前端
    // this.notifyFrontend('device.status', { deviceId, data });
  }

  private async handleLocation(deviceId: string, data: any) {
    // 保存位置数据
    await prisma.deviceStatus.create({
      data: {
        deviceId,
        data: { type: 'location', ...data }
      }
    });
  }

  private async handleEvent(deviceId: string, data: any) {
    // 保存事件数据
    await prisma.deviceStatus.create({
      data: {
        deviceId,
        data: { type: 'event', ...data }
      }
    });

    // 可以触发告警通知
  }

  // 发送命令到设备
  sendCommand(deviceId: string, command: string, params: any = {}) {
    if (!this.client) {
      throw new Error('MQTT not connected');
    }

    const topic = `device/${deviceId}/command`;
    const message = JSON.stringify({
      command,
      request_id: Date.now().toString(),
      params
    });

    this.client.publish(topic, message);
  }

  // 发送配置到设备
  sendConfig(deviceId: string, config: any) {
    if (!this.client) {
      throw new Error('MQTT not connected');
    }

    const topic = `device/${deviceId}/config`;
    this.client.publish(topic, JSON.stringify(config));
  }
}

export const mqttService = new MqttService();
```

### 命令接口

```typescript
// routes/device_manage.ts
router.post('/devices/:id/command', async (req, res) => {
  const { id } = req.params;
  const { command, params } = req.body;

  const device = await prisma.device.findUnique({
    where: { id: parseInt(id) }
  });

  if (!device) {
    return res.status(404).json({ error: 'device not found' });
  }

  if (!device.isOnline) {
    return res.status(400).json({ error: 'device offline' });
  }

  try {
    mqttService.sendCommand(device.deviceId, command, params);
    return res.json({ success: true, message: '命令已发送' });
  } catch (err) {
    return res.status(500).json({ error: 'failed to send command' });
  }
});
```

---

## 六、设备端实现

### ESP32 MQTT 客户端

```cpp
// mqtt_client.cc
#include <mqtt_client.h>
#include "settings.h"

class MqttClient {
public:
    void Connect(const std::string& endpoint, const std::string& clientId,
                 const std::string& username, const std::string& password) {
        esp_mqtt_client_config_t config = {};
        config.uri = endpoint.c_str();
        config.client_id = clientId.c_str();
        config.username = username.c_str();
        config.password = password.c_str();

        client_ = esp_mqtt_client_init(&config);
        esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, EventHandler, this);
        esp_mqtt_client_start(client_);
    }

    void PublishStatus(const std::string& deviceId, const std::string& data) {
        std::string topic = "device/" + deviceId + "/status";
        esp_mqtt_client_publish(client_, topic.c_str(), data.c_str(), 0, 1, 0);
    }

    void PublishLocation(const std::string& deviceId, const std::string& data) {
        std::string topic = "device/" + deviceId + "/location";
        esp_mqtt_client_publish(client_, topic.c_str(), data.c_str(), 0, 1, 0);
    }

private:
    esp_mqtt_client_handle_t client_;

    static void EventHandler(void* handler_args, esp_event_base_t base,
                             int32_t event_id, void* event_data) {
        auto* event = (esp_mqtt_event_handle_t)event_data;
        switch (event->event_id) {
            case MQTT_EVENT_CONNECTED:
                ESP_LOGI("MQTT", "Connected");
                // 订阅命令 topic
                break;
            case MQTT_EVENT_DATA:
                // 处理命令
                break;
        }
    }
};
```

---

## 七、离线检测

### 设备离线判断

```typescript
// 定时任务：每分钟检查设备在线状态
setInterval(async () => {
  const timeout = 5 * 60 * 1000; // 5 分钟超时
  const threshold = new Date(Date.now() - timeout);

  await prisma.device.updateMany({
    where: {
      isOnline: true,
      lastSeenAt: { lt: threshold }
    },
    data: { isOnline: false }
  });
}, 60 * 1000);
```

### 设备端心跳

设备定期发送状态数据作为心跳：

```cpp
// 每 60 秒发送一次状态
void SendHeartbeat() {
    cJSON* status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "engine_status", "on");
    cJSON_AddNumberToObject(status, "battery_voltage", GetBatteryVoltage());
    cJSON_AddNumberToObject(status, "signal_strength", GetSignalStrength());

    char* json = cJSON_PrintUnformatted(status);
    PublishStatus(device_id_, json);
    cJSON_free(json);
    cJSON_Delete(status);
}
```