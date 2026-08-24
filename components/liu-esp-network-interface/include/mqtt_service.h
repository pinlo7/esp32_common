#pragma once
#ifndef _MQTT_SERVICE_H_
#define _MQTT_SERVICE_H_

#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "mqtt.h"

/**
 * 通用 MQTT 服务
 *
 * 负责连接生命周期（连接/断线指数退避重连）、订阅管理与消息分发、发布、
 * 可选心跳回调。不包含任何业务逻辑（主题语义、消息格式、凭据来源均由使用方决定）。
 *
 * 重连由本服务统一负责（每次断线按 1s 起指数退避，重建传输实例），
 * 各传输实现（esp-mqtt / ML307 / EC801E）的客户端内部自动重连应关闭，避免双重连接管理。
 *
 * 用法：
 *   1. 注入 Mqtt 实例工厂（如从 NetworkInterface::CreateMqtt 创建，每次连接/重连时调用）
 *   2. Start(config) 启动后台任务
 *   3. Subscribe(topic, qos, handler) 注册订阅（连接建立/重连后自动恢复）
 *   4. Publish(topic, payload, qos) 发布
 */
class MqttService {
public:
    struct Config {
        std::string host;
        int port = 1883;
        std::string client_id;
        std::string username;
        std::string password;
        int keep_alive = 60;
        // 心跳回调触发间隔（毫秒），0 表示不启用
        int heartbeat_interval_ms = 0;
        int task_stack_size = 4096;
        int task_priority = 2;
    };

    MqttService();
    ~MqttService();

    /** 注入 Mqtt 实例工厂（调用前必须设置；Start 后每次重连都会调用） */
    void SetMqttFactory(std::function<std::unique_ptr<Mqtt>()> factory);

    bool Start(const Config& config);
    void Stop();
    bool IsConnected() const;

    /** 注册订阅：连接建立/重连后自动订阅；收到消息按 topic 匹配分发到 handler */
    bool Subscribe(const std::string& topic_filter, int qos,
                   std::function<void(const std::string& topic, const std::string& payload)> handler);
    bool Unsubscribe(const std::string& topic_filter);

    /** 发布消息 */
    bool Publish(const std::string& topic, const std::string& payload, int qos = 0);

    /** 连接状态回调（可选） */
    void SetOnConnected(std::function<void()> cb);
    void SetOnDisconnected(std::function<void()> cb);
    /** 心跳回调：连接状态下按 heartbeat_interval_ms 周期调用（内容由使用方决定） */
    void SetHeartbeatCallback(std::function<void()> cb);

private:
    struct Subscription {
        std::string filter;
        int qos;
        std::function<void(const std::string&, const std::string&)> handler;
    };

    Config config_;
    bool run_ = false;
    TaskHandle_t task_handle_ = nullptr;
    std::unique_ptr<Mqtt> mqtt_;
    bool connected_ = false;
    mutable std::mutex mutex_;

    std::function<std::unique_ptr<Mqtt>()> mqtt_factory_;
    std::vector<Subscription> subscriptions_;
    std::function<void()> on_connected_cb_;
    std::function<void()> on_disconnected_cb_;
    std::function<void()> heartbeat_cb_;

    void ConnectLoop();
    bool TryConnect();
    void OnConnected();
    void DispatchMessage(const std::string& topic, const std::string& payload);

    static bool TopicMatches(const std::string& filter, const std::string& topic);
    static void TaskEntry(void* arg);
};

#endif // _MQTT_SERVICE_H_
