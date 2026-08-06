#pragma once
#ifndef _MQTT_SERVICE_H_
#define _MQTT_SERVICE_H_

#include <string>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "mqtt.h"

/**
 * MQTT 设备服务
 *
 * 负责与 EMQX 建立连接（device_<MAC> + 派生密码）、订阅 command/config、
 * 上报 status/location/event、应答命令（command_ack）以及断线退避重连。
 * 消息格式见 docs/device-integration.md。
 */
class MqttService {
public:
    MqttService();
    ~MqttService();

    /** 启动服务（从 NVS 读取 MQTT 凭据并创建工作任务），失败返回 false */
    bool Start();

    /** 停止服务并断开连接 */
    void Stop();

    // 上报接口
    bool PublishStatus(const std::string& json);
    bool PublishEvent(const std::string& json);

private:
    static constexpr int kHeartbeatIntervalSec = 60;
    static constexpr int kMaxReconnectDelaySec = 60;

    std::string device_id_;
    std::string device_secret_;
    std::string broker_host_;
    int broker_port_ = 1883;
    std::string client_id_;
    std::string username_;
    std::string password_;
    bool run_ = false;
    TaskHandle_t task_handle_ = nullptr;
    std::unique_ptr<Mqtt> mqtt_;
    bool connected_ = false;

    bool LoadConfig();
    static std::string DeriveMqttPassword(const std::string& device_secret);

    void ConnectLoop();
    bool TryConnect();
    void OnConnected();
    void OnMessage(const std::string& topic, const std::string& payload);
    void HandleCommand(const std::string& topic, const std::string& payload);
    void SendCommandAck(const std::string& request_id, bool ok, const std::string& error);
    std::string BuildStatusJson();

    static void TaskEntry(void* arg);
};

#endif // _MQTT_SERVICE_H_
