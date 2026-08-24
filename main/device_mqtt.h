#pragma once
#ifndef _DEVICE_MQTT_H_
#define _DEVICE_MQTT_H_

#include <string>
#include <memory>
#include <functional>

#include "mqtt_service.h"

/**
 * 设备业务 MQTT 服务
 *
 * 承载设备相关的 MQTT 业务：从 NVS 加载凭据并派生密码、订阅 command/config、
 * 命令处理与 ACK 应答、状态/事件上报、心跳、升级命令回调。
 * 底层连接/重连/订阅管理/消息分发由通用 MqttService（组件层）提供。
 */
class DeviceMqtt {
public:
    DeviceMqtt();
    ~DeviceMqtt();

    /** 从 NVS 加载配置并启动（失败返回 false） */
    bool Start();

    /** 停止并断开连接 */
    void Stop();

    bool IsConnected() const;

    /** 升级命令回调（收到 command=upgrade 时触发，参数 force 是否强制升级） */
    void SetOnUpgradeRequested(std::function<void(bool force)> callback);

    // 上报接口
    bool PublishStatus(const std::string& json);
    bool PublishEvent(const std::string& json);

private:
    static constexpr int kHeartbeatIntervalMs = 60000;

    std::unique_ptr<MqttService> mqtt_service_;
    std::string device_id_;
    std::string username_;
    std::string password_;
    std::function<void(bool force)> on_upgrade_requested_;

    bool LoadConfig();
    static std::string DeriveMqttPassword(const std::string& device_secret);

    void OnCommand(const std::string& topic, const std::string& payload);
    void OnConfig(const std::string& topic, const std::string& payload);
    void SendCommandAck(const std::string& request_id, bool ok, const std::string& error);
    std::string BuildStatusJson();
};

#endif // _DEVICE_MQTT_H_
