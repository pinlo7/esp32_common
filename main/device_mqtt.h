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
 * 本类属于应用"业务层"，承载设备相关的 MQTT 协议语义。分层关系：
 *
 *   Mqtt（传输接口）→ MqttService（通用服务）→ DeviceMqtt（本类，业务层）
 *
 * - 底层传输（ESP 原生 / ML307 / EC801E AT 模组）由 Mqtt 接口抽象，
 *   通过 SetMqttFactory() 由 App 装配注入，本类不依赖具体传输实现，也不触碰 Board。
 * - 连接生命周期、断线指数退避重连、订阅注册与自动恢复、通配符消息分发
 *   均由组件层 MqttService 提供；esp-mqtt 客户端内部自动重连已关闭，
 *   避免两层重连叠加。本类不包含这些基础设施逻辑。
 * - 本类负责业务语义：从 NVS 加载 MQTT 凭据并派生密码、订阅
 *   device/{device_id}/command 与 device/{device_id}/config、
 *   消息处理（OnCommand / OnConfig）、命令 ACK 应答、状态/事件上报、心跳内容。
 *
 * 与 App 的协作：
 * - App 负责编排（启动时机、升级流程调度），不直接处理 MQTT 协议细节。
 * - 通过 SetOnUpgradeRequested() 回调把"升级命令"转交给 App 执行，
 *   本类不直接依赖 Ota。
 *
 * 使用顺序：SetMqttFactory() → SetOnUpgradeRequested() → Start()。
 */
class DeviceMqtt {
public:
    DeviceMqtt();
    ~DeviceMqtt();

    /** 注入底层 Mqtt 传输工厂（必须在 Start 前调用，转发给 MqttService） */
    void SetMqttFactory(std::function<std::unique_ptr<Mqtt>()> factory);
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
