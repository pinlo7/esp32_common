#include "mqtt_service.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <mbedtls/md.h>
#include <algorithm>
#include <cstring>

#define TAG "MqttService"

MqttService::MqttService() {}

MqttService::~MqttService() {
    Stop();
}

// ============ NVS 配置加载 ============

bool MqttService::LoadConfig() {
    device_id_ = SystemInfo::GetMacAddress();

    Settings device_settings("device", false);
    device_secret_ = device_settings.GetString("device_key");
    if (device_secret_.empty()) {
        ESP_LOGW(TAG, "No device secret, MQTT cannot start");
        return false;
    }

    Settings mqtt_settings("mqtt", false);
    std::string endpoint = mqtt_settings.GetString("endpoint");
    client_id_ = mqtt_settings.GetString("client_id");
    username_ = mqtt_settings.GetString("username");
    password_ = mqtt_settings.GetString("password");

    if (endpoint.empty() || username_.empty() || password_.empty()) {
        ESP_LOGW(TAG, "Incomplete MQTT config, MQTT cannot start");
        return false;
    }

    // 解析 endpoint：mqtt://host:port（mqtts:// 或 ssl:// 视为 TLS，默认 8883）
    std::string host = endpoint;
    bool ssl = false;
    auto scheme_end = endpoint.find("://");
    if (scheme_end != std::string::npos) {
        std::string proto = endpoint.substr(0, scheme_end);
        ssl = (proto == "mqtts" || proto == "ssl" || proto == "tls");
        host = endpoint.substr(scheme_end + 3);
    }
    auto colon = host.rfind(':');
    if (colon != std::string::npos) {
        broker_host_ = host.substr(0, colon);
        broker_port_ = atoi(host.substr(colon + 1).c_str());
        if (broker_port_ <= 0) broker_port_ = ssl ? 8883 : 1883;
    } else {
        broker_host_ = host;
        broker_port_ = ssl ? 8883 : 1883;
    }

    if (client_id_.empty()) {
        client_id_ = "GID_" + device_id_;
    }

    ESP_LOGI(TAG, "MQTT endpoint: %s://%s:%d, client: %s",
             ssl ? "mqtts" : "mqtt", broker_host_.c_str(), broker_port_, client_id_.c_str());
    return !broker_host_.empty();
}

// ============ 密码派生 ============

std::string MqttService::DeriveMqttPassword(const std::string& device_secret) {
    const char* context = "mqtt:v1";
    unsigned char mac[32];
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md_info,
                        (const unsigned char*)device_secret.data(), device_secret.size(),
                        (const unsigned char*)context, strlen(context), mac) != 0) {
        return "";
    }
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", mac[i]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

// ============ 连接管理 ============

bool MqttService::TryConnect() {
    mqtt_ = Board::GetInstance().GetNetwork()->CreateMqtt(0);
    if (!mqtt_) {
        ESP_LOGE(TAG, "CreateMqtt failed");
        return false;
    }

    mqtt_->SetKeepAlive(60);
    mqtt_->OnConnected([this]() { OnConnected(); });
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) { OnMessage(topic, payload); });
    mqtt_->OnDisconnected([this]() {
        connected_ = false;
        ESP_LOGW(TAG, "MQTT disconnected");
    });

    if (!mqtt_->Connect(broker_host_, broker_port_, client_id_, username_, password_)) {
        ESP_LOGW(TAG, "MQTT connect failed");
        mqtt_.reset();
        return false;
    }
    connected_ = true;
    return true;
}

void MqttService::OnConnected() {
    connected_ = true;
    ESP_LOGI(TAG, "MQTT connected");
    mqtt_->Subscribe("device/" + device_id_ + "/command", 1);
    mqtt_->Subscribe("device/" + device_id_ + "/config", 1);
}

void MqttService::ConnectLoop() {
    int retry_delay_sec = 1;
    while (run_) {
        if (!connected_) {
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
        ESP_LOGI(TAG, "MQTT task stack high water: %u",
        uxTaskGetStackHighWaterMark(task_handle_));
        // 心跳：定期上报状态
        if (connected_) {
            PublishStatus(BuildStatusJson());
        }
        for (int i = 0; i < kHeartbeatIntervalSec && run_; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void MqttService::TaskEntry(void* arg) {
    auto* self = static_cast<MqttService*>(arg);
    self->ConnectLoop();
    vTaskDelete(NULL);
}

bool MqttService::Start() {
    if (run_) {
        return true;
    }
    if (!LoadConfig()) {
        return false;
    }
    run_ = true;
    if (xTaskCreate(TaskEntry, "mqtt_service", 4096 * 3, this, 2, &task_handle_) != pdPASS) {
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
    if (mqtt_) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
    connected_ = false;
}

// ============ 消息收发 ============

bool MqttService::PublishStatus(const std::string& json) {
    if (!mqtt_ || !connected_) return false;
    return mqtt_->Publish("device/" + device_id_ + "/status", json, 1);
}

bool MqttService::PublishEvent(const std::string& json) {
    if (!mqtt_ || !connected_) return false;
    return mqtt_->Publish("device/" + device_id_ + "/event", json, 1);
}

std::string MqttService::BuildStatusJson() {
    auto app_desc = esp_app_get_description();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "engine_status", "on");
    cJSON_AddNumberToObject(root, "signal_strength", 0);
    cJSON_AddStringToObject(root, "firmware_version", app_desc->version);
    char* json = cJSON_PrintUnformatted(root);
    std::string result(json);
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

void MqttService::OnMessage(const std::string& topic, const std::string& payload) {
    if (topic.find("/command") != std::string::npos) {
        HandleCommand(topic, payload);
        return;
    }
    // config 下发暂未接入业务逻辑，仅记录
    if (topic.find("/config") != std::string::npos) {
        ESP_LOGI(TAG, "Config received: %s", payload.c_str());
    }
}

void MqttService::HandleCommand(const std::string& topic, const std::string& payload) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid command payload: %s", payload.c_str());
        return;
    }

    cJSON* request_id = cJSON_GetObjectItem(root, "request_id");
    cJSON* command = cJSON_GetObjectItem(root, "command");
    std::string request_id_str = cJSON_IsString(request_id) ? request_id->valuestring : "";
    std::string command_str = cJSON_IsString(command) ? command->valuestring : "";

    if (request_id_str.empty() || command_str.empty()) {
        ESP_LOGW(TAG, "Command missing request_id/command");
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Command received: %s (request_id=%s)", command_str.c_str(), request_id_str.c_str());

    if (command_str == "reboot") {
        SendCommandAck(request_id_str, true, "");
        ESP_LOGI(TAG, "Rebooting in 500ms...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (command_str == "upgrade") {
        // 升级是长操作（下载可能几分钟），先回 ACK，再交给 App 在独立任务中执行
        bool force = false;
        cJSON* params = cJSON_GetObjectItem(root, "params");
        cJSON* force_item = params ? cJSON_GetObjectItem(params, "force") : NULL;
        if (cJSON_IsBool(force_item)) {
            force = cJSON_IsTrue(force_item);
        }
        SendCommandAck(request_id_str, true, "");
        ESP_LOGI(TAG, "Upgrade command received (force=%d), dispatching", force ? 1 : 0);
        if (on_upgrade_requested_) {
            on_upgrade_requested_(force);
        }
    } else if (command_str == "lock" || command_str == "unlock") {
        // TODO: 接入实际锁定/解锁硬件逻辑
        ESP_LOGI(TAG, "Command %s executed (no hardware action bound yet)", command_str.c_str());
        SendCommandAck(request_id_str, true, "");
    } else {
        ESP_LOGW(TAG, "Unsupported command: %s", command_str.c_str());
        SendCommandAck(request_id_str, false, "unsupported_command");
    }

    cJSON_Delete(root);
}

void MqttService::SendCommandAck(const std::string& request_id, bool ok, const std::string& error) {
    if (request_id.empty() || !mqtt_ || !connected_) return;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "request_id", request_id.c_str());
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", ok);
    if (!error.empty()) {
        cJSON_AddStringToObject(result, "error", error.c_str());
    }
    cJSON_AddItemToObject(root, "result", result);
    char* json = cJSON_PrintUnformatted(root);
    mqtt_->Publish("device/" + device_id_ + "/command_ack", std::string(json), 1);
    cJSON_free(json);
    cJSON_Delete(root);
}
