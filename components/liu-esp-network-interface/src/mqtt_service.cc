#include "mqtt_service.h"

#include <esp_log.h>
#include <algorithm>

#define TAG "MqttService"

MqttService::MqttService() {}

MqttService::~MqttService() {
    Stop();
}

void MqttService::SetMqttFactory(std::function<std::unique_ptr<Mqtt>()> factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    mqtt_factory_ = std::move(factory);
}

// ============ 连接管理 ============

bool MqttService::TryConnect() {
    std::unique_ptr<Mqtt> mqtt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!mqtt_factory_) {
            ESP_LOGE(TAG, "MQTT factory not set");
            return false;
        }
        mqtt = mqtt_factory_();
    }
    if (!mqtt) {
        ESP_LOGE(TAG, "CreateMqtt failed");
        return false;
    }

    mqtt->SetKeepAlive(config_.keep_alive);
    mqtt->OnConnected([this]() { OnConnected(); });
    mqtt->OnMessage([this](const std::string& topic, const std::string& payload) {
        DispatchMessage(topic, payload);
    });
    mqtt->OnDisconnected([this]() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
        }
        ESP_LOGW(TAG, "MQTT disconnected");
        if (on_disconnected_cb_) {
            on_disconnected_cb_();
        }
    });

    // 先挂实例再 Connect：连接事件回调（OnConnected 里要恢复订阅）可能早于 Connect 返回触发
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mqtt_ = std::move(mqtt);
    }

    bool ok = mqtt_->Connect(config_.host, config_.port, config_.client_id,
                             config_.username, config_.password);
    if (!ok) {
        ESP_LOGW(TAG, "MQTT connect failed");
        // 连接失败：销毁实例（移到局部变量，锁外析构），由重连循环稍后重建
        std::unique_ptr<Mqtt> dead;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dead = std::move(mqtt_);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = true;
    }
    return true;
}

void MqttService::OnConnected() {
    ESP_LOGI(TAG, "MQTT connected");
    std::lock_guard<std::mutex> lock(mutex_);
    if (mqtt_) {
        for (const auto& sub : subscriptions_) {
            mqtt_->Subscribe(sub.filter, sub.qos);
        }
    }
    if (on_connected_cb_) {
        on_connected_cb_();
    }
}

void MqttService::ConnectLoop() {
    static constexpr int kMaxReconnectDelaySec = 60;
    int retry_delay_sec = 1;
    int heartbeat_elapsed_ms = 0;

    while (run_) {
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected = connected_;
        }

        if (!connected) {
            if (TryConnect()) {
                retry_delay_sec = 1;
            } else {
                ESP_LOGW(TAG, "MQTT connect failed, retry in %ds", retry_delay_sec);
                for (int i = 0; i < retry_delay_sec && run_; i++) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                retry_delay_sec = std::min(retry_delay_sec * 2, kMaxReconnectDelaySec);
                continue;
            }
        }

        // 心跳回调（内容由使用方决定，如业务状态上报）
        if (config_.heartbeat_interval_ms > 0) {
            heartbeat_elapsed_ms += 1000;
            if (heartbeat_elapsed_ms >= config_.heartbeat_interval_ms) {
                heartbeat_elapsed_ms = 0;
                std::function<void()> cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cb = heartbeat_cb_;
                }
                if (cb) {
                    cb();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void MqttService::TaskEntry(void* arg) {
    auto* self = static_cast<MqttService*>(arg);
    self->ConnectLoop();
    vTaskDelete(NULL);
}

bool MqttService::Start(const Config& config) {
    if (run_) {
        return true;
    }
    if (config.host.empty() || config.client_id.empty()) {
        ESP_LOGE(TAG, "MQTT config incomplete (host/client_id required)");
        return false;
    }
    config_ = config;
    run_ = true;
    if (xTaskCreate(TaskEntry, "mqtt_service", config_.task_stack_size, this,
                    config_.task_priority, &task_handle_) != pdPASS) {
        run_ = false;
        ESP_LOGE(TAG, "Failed to create MQTT task");
        return false;
    }
    return true;
}

void MqttService::Stop() {
    run_ = false;
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mqtt_) {
            mqtt_->Disconnect();
            mqtt_.reset();
        }
        connected_ = false;
    }
}

bool MqttService::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

// ============ 订阅 / 发布 ============

bool MqttService::Subscribe(
    const std::string& topic_filter, int qos,
    std::function<void(const std::string&, const std::string&)> handler) {
    if (topic_filter.empty() || !handler) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 同 filter 重复订阅则替换 handler
        for (auto& sub : subscriptions_) {
            if (sub.filter == topic_filter) {
                sub.qos = qos;
                sub.handler = std::move(handler);
                return true;
            }
        }
        subscriptions_.push_back({topic_filter, qos, std::move(handler)});
    }
    // 已连接则立即订阅
    std::lock_guard<std::mutex> lock(mutex_);
    if (mqtt_ && connected_) {
        mqtt_->Subscribe(topic_filter, qos);
    }
    return true;
}

bool MqttService::Unsubscribe(const std::string& topic_filter) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = subscriptions_.begin(); it != subscriptions_.end(); ++it) {
        if (it->filter == topic_filter) {
            if (mqtt_ && connected_) {
                mqtt_->Unsubscribe(topic_filter);
            }
            subscriptions_.erase(it);
            return true;
        }
    }
    return false;
}

bool MqttService::Publish(const std::string& topic, const std::string& payload, int qos) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mqtt_ || !connected_) {
        return false;
    }
    return mqtt_->Publish(topic, payload, qos);
}

// ============ 消息分发 ============

void MqttService::DispatchMessage(const std::string& topic, const std::string& payload) {
    std::vector<Subscription> matched;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& sub : subscriptions_) {
            if (TopicMatches(sub.filter, topic)) {
                matched.push_back(sub);
            }
        }
    }
    for (const auto& sub : matched) {
        if (sub.handler) {
            sub.handler(topic, payload);
        }
    }
}

// 支持 MQTT 通配符：'+' 匹配单级，'#' 匹配剩余所有级（含多级）
bool MqttService::TopicMatches(const std::string& filter, const std::string& topic) {
    size_t fi = 0, ti = 0;
    while (true) {
        if (fi < filter.size() && filter[fi] == '#') {
            return true;
        }
        size_t fslash = filter.find('/', fi);
        size_t tslash = topic.find('/', ti);
        std::string fseg = (fslash == std::string::npos)
                               ? filter.substr(fi)
                               : filter.substr(fi, fslash - fi);
        std::string tseg = (tslash == std::string::npos)
                               ? topic.substr(ti)
                               : topic.substr(ti, tslash - ti);
        if (fseg != "+" && fseg != tseg) {
            return false;
        }
        if (fslash == std::string::npos && tslash == std::string::npos) {
            return true;
        }
        if (fslash == std::string::npos || tslash == std::string::npos) {
            return false;
        }
        fi = fslash + 1;
        ti = tslash + 1;
    }
}

void MqttService::SetOnConnected(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_connected_cb_ = std::move(cb);
}

void MqttService::SetOnDisconnected(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_disconnected_cb_ = std::move(cb);
}

void MqttService::SetHeartbeatCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    heartbeat_cb_ = std::move(cb);
}
