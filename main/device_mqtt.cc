#include "device_mqtt.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <psa/crypto.h>
#include <cstring>

#define TAG "DeviceMqtt"

DeviceMqtt::DeviceMqtt() : mqtt_service_(std::make_unique<MqttService>()) {}

DeviceMqtt::~DeviceMqtt() {
    Stop();
}

// ============ NVS 配置加载 ============

bool DeviceMqtt::LoadConfig() {
    device_id_ = SystemInfo::GetMacAddress();

    Settings device_settings("device", false);
    std::string device_secret = device_settings.GetString("device_key");
    if (device_secret.empty()) {
        ESP_LOGW(TAG, "No device secret, MQTT cannot start");
        return false;
    }

    Settings mqtt_settings("mqtt", false);
    std::string endpoint = mqtt_settings.GetString("endpoint");
    std::string client_id = mqtt_settings.GetString("client_id");
    username_ = mqtt_settings.GetString("username");
    password_ = mqtt_settings.GetString("password");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint missing, MQTT cannot start");
        return false;
    }

    MqttService::Config config;

    // 解析 endpoint：mqtt://host:port（mqtts:// / ssl:// / tls:// 视为 TLS，默认 8883）
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
        config.host = host.substr(0, colon);
        config.port = atoi(host.substr(colon + 1).c_str());
        if (config.port <= 0) config.port = ssl ? 8883 : 1883;
    } else {
        config.host = host;
        config.port = ssl ? 8883 : 1883;
    }

    config.client_id = client_id.empty() ? "GID_" + device_id_ : client_id;
    // 用户名/密码以 OTA 下发为准；缺省按协议派生
    config.username = username_.empty() ? "device_" + device_id_ : username_;
    config.password = password_.empty() ? DeriveMqttPassword(device_secret) : password_;
    config.heartbeat_interval_ms = kHeartbeatIntervalMs;

    ESP_LOGI(TAG, "MQTT endpoint: %s://%s:%d, client: %s",
             ssl ? "mqtts" : "mqtt", config.host.c_str(), config.port, config.client_id.c_str());

    mqtt_service_->SetMqttFactory([]() {
        return Board::GetInstance().GetNetwork()->CreateMqtt(0);
    });
    mqtt_service_->Subscribe("device/" + device_id_ + "/command", 1,
                             [this](const std::string& t, const std::string& p) { OnCommand(t, p); });
    mqtt_service_->Subscribe("device/" + device_id_ + "/config", 1,
                             [this](const std::string& t, const std::string& p) { OnConfig(t, p); });
    mqtt_service_->SetHeartbeatCallback([this]() {
        PublishStatus(BuildStatusJson());
    });

    return mqtt_service_->Start(config);
}

// ============ 密码派生 ============

std::string DeviceMqtt::DeriveMqttPassword(const std::string& device_secret) {
    const char* context = "mqtt:v1";
    unsigned char mac[32];
    size_t mac_len = 0;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(
        &attributes,
        reinterpret_cast<const uint8_t*>(device_secret.data()), device_secret.size(),
        &key_id);
    if (status != PSA_SUCCESS) {
        return "";
    }

    status = psa_mac_compute(
        key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
        reinterpret_cast<const uint8_t*>(context), strlen(context),
        mac, sizeof(mac), &mac_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS) {
        return "";
    }
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", mac[i]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

// ============ 生命周期 ============

bool DeviceMqtt::Start() {
    return LoadConfig();
}

void DeviceMqtt::Stop() {
    if (mqtt_service_) {
        mqtt_service_->Stop();
    }
}

bool DeviceMqtt::IsConnected() const {
    return mqtt_service_ && mqtt_service_->IsConnected();
}

void DeviceMqtt::SetOnUpgradeRequested(std::function<void(bool force)> callback) {
    on_upgrade_requested_ = std::move(callback);
}

// ============ 上报 ============

bool DeviceMqtt::PublishStatus(const std::string& json) {
    return mqtt_service_->Publish("device/" + device_id_ + "/status", json, 1);
}

bool DeviceMqtt::PublishEvent(const std::string& json) {
    return mqtt_service_->Publish("device/" + device_id_ + "/event", json, 1);
}

std::string DeviceMqtt::BuildStatusJson() {
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

// ============ 订阅处理 ============

void DeviceMqtt::OnConfig(const std::string& topic, const std::string& payload) {
    // config 下发暂未接入业务逻辑，仅记录
    ESP_LOGI(TAG, "Config received: %s", payload.c_str());
}

void DeviceMqtt::OnCommand(const std::string& topic, const std::string& payload) {
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

void DeviceMqtt::SendCommandAck(const std::string& request_id, bool ok, const std::string& error) {
    if (request_id.empty()) return;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "request_id", request_id.c_str());
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", ok);
    if (!error.empty()) {
        cJSON_AddStringToObject(result, "error", error.c_str());
    }
    cJSON_AddItemToObject(root, "result", result);
    char* json = cJSON_PrintUnformatted(root);
    mqtt_service_->Publish("device/" + device_id_ + "/command_ack", std::string(json), 1);
    cJSON_free(json);
    cJSON_Delete(root);
}
