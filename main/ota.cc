#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "board.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <sys/time.h>
#include <cstring>
#include <sstream>
#include <algorithm>

#define TAG "Ota"
#define CONFIG_OTA_URL "http://192.168.3.185:3001/api/device_manage/ota"

// 提取 URL 的 origin（scheme://host[:port]），用于拼接固件下载地址
static std::string ExtractOrigin(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return "";
    auto path_start = url.find('/', scheme_end + 3);
    return url.substr(0, path_start == std::string::npos ? url.length() : path_start);
}

Ota::Ota() {
    // device_key_ = "test_secret";
    device_key_ = GetDeviceSecret();
    if (device_key_.empty()) {
        ESP_LOGW(TAG, "No device key found, activation required");
    }
}

Ota::~Ota() {
}

// ============ NVS 密钥管理 ============

std::string Ota::GetOtaUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::string Ota::GetDeviceSecret() {
    Settings settings("device", false);
    return settings.GetString("device_key");
}

bool Ota::SaveDeviceSecret(const std::string& key) {
    Settings settings("device", true);
    settings.SetString("device_key", key);
    device_key_ = key;
    ESP_LOGI(TAG, "Device key saved");
    return true;
}

bool Ota::HasDeviceSecret() const {
    return !device_key_.empty();
}

// ============ 签名计算 ============

std::string Ota::Sha256Hex(const std::string& input) {
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)input.c_str(), input.length(), hash, 0);

    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hex + i * 2, "%02x", hash[i]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

std::string Ota::GetTimestamp() {
    time_t now;
    time(&now);
    return std::to_string(now);
}

std::string Ota::CalculateSignature(const std::string& body_json, const std::string& timestamp) {
    // 激活阶段用引导密钥签名；已激活后用 device_secret 签名
    const std::string& secret = HasProvisionKey() ? provision_key_ : device_key_;
    if (secret.empty()) {
        return "";
    }

    // 1. 计算请求体 hash
    std::string body_hash = Sha256Hex(body_json);

    // 2. 拼接签名原文: device_key + timestamp + device_id + body_sha256
    std::string device_id = SystemInfo::GetMacAddress();
    std::string text = secret + timestamp + device_id + body_hash;

    // 3. 计算签名
    return Sha256Hex(text);
}

// ============ HTTP 请求 ============

std::string Ota::BuildRequestBody() {
    auto app_desc = esp_app_get_description();
    auto& board = Board::GetInstance();

    cJSON* root = cJSON_CreateObject();

    // application
    cJSON* app = cJSON_CreateObject();
    cJSON_AddStringToObject(app, "version", app_desc->version);
    cJSON_AddStringToObject(app, "elf_sha256", "");
    cJSON_AddItemToObject(root, "application", app);

    // board - 使用 GetBoardJson() 获取完整 board 信息
    std::string board_json_str = board.GetBoardJson();
    if (!board_json_str.empty()) {
        cJSON* board_obj = cJSON_Parse(board_json_str.c_str());
        if (board_obj) {
            cJSON_AddItemToObject(root, "board", board_obj);
        }
    }

    char* json = cJSON_PrintUnformatted(root);
    std::string result(json);
    cJSON_free(json);
    cJSON_Delete(root);

    return result;
}

esp_err_t Ota::ParseResponse(const std::string& response) {
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 重置状态
    has_activation_code_ = false;
    has_new_version_ = false;
    has_mqtt_config_ = false;
    has_websocket_config_ = false;
    has_server_time_ = false;
    has_rotated_key_ = false;

    // 1. 解析 device_secret
    cJSON* device_secret = cJSON_GetObjectItem(root, "device_secret");
    if (cJSON_IsString(device_secret)) {
        Settings settings("device", true);
        settings.SetString("device_key", device_secret->valuestring);
        device_key_ = device_secret->valuestring;
        // 激活成功，清除引导密钥（仅激活阶段使用）
        ClearProvisionKey();
        ESP_LOGI(TAG, "Activation success, device secret saved");
    }

    // 2. 解析 mqtt
    cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                settings.SetString(item->string, item->valuestring);
            } else if (cJSON_IsNumber(item)) {
                settings.SetInt(item->string, item->valueint);
            }
        }
        has_mqtt_config_ = true;
        ESP_LOGI(TAG, "MQTT config saved");
    }

    // 3. 解析 websocket
    cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                settings.SetString(item->string, item->valuestring);
            } else if (cJSON_IsNumber(item)) {
                settings.SetInt(item->string, item->valueint);
            }
        }
        has_websocket_config_ = true;
        ESP_LOGI(TAG, "WebSocket config saved");
    }

    // 4. 解析 server_time
    cJSON* server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON* timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON* timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");

        if (cJSON_IsNumber(timestamp)) {
            double ts = timestamp->valuedouble;
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000);
            }

            struct timeval tv;
            tv.tv_sec = (time_t)(ts / 1000);
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;
            settimeofday(&tv, NULL);
            has_server_time_ = true;
            ESP_LOGI(TAG, "Server time synced");
        }
    }

    // 5. 解析 firmware
    cJSON* firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON* version = cJSON_GetObjectItem(firmware, "version");
        cJSON* url = cJSON_GetObjectItem(firmware, "url");
        cJSON* rotate_key = cJSON_GetObjectItem(firmware, "rotate_key");

        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        if (cJSON_IsString(url)) {
            // 服务器返回的是相对路径（如 /firmware/1.0.0.bin），拼接 OTA 地址的 origin
            std::string origin = ExtractOrigin(GetOtaUrl());
            if (!origin.empty() && url->valuestring[0] == '/') {
                firmware_url_ = origin + url->valuestring;
            } else {
                firmware_url_ = url->valuestring;
            }
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version: %s -> %s", current_version_.c_str(), firmware_version_.c_str());
            }
        }

        // 密钥轮换
        if (cJSON_IsString(rotate_key) && strlen(rotate_key->valuestring) > 0) {
            rotate_key_ = rotate_key->valuestring;
            has_rotated_key_ = true;
            ESP_LOGI(TAG, "Key rotation requested");
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ============ 核心功能 ============

esp_err_t Ota::CheckVersion() {
    ESP_LOGI(TAG, "CheckVersion enter");
    auto app_desc = esp_app_get_description();
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetOtaUrl();
    ESP_LOGI(TAG, "OTA URL: %s", url.c_str());
    if (url.length() < 10) {
        ESP_LOGE(TAG, "OTA URL not set");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "BuildRequestBody...");
    std::string body = BuildRequestBody();
    ESP_LOGI(TAG, "Body: %s", body.c_str());
    std::string timestamp = GetTimestamp();

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    ESP_LOGI(TAG, "CreateHttp...");
    auto http = network->CreateHttp(0);

    // 设置请求头
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    http->SetHeader("X-Timestamp", timestamp.c_str());
    http->SetHeader("Accept-Language", "zh-CN");
    http->SetHeader("Content-Type", "application/json");

    // 首次激活：携带引导密钥并以该密钥签名；已激活：以 device_secret 签名
    if (HasProvisionKey()) {
        ESP_LOGI(TAG, "Activation request with provision key");
        http->SetHeader("X-Provision-Key", provision_key_.c_str());
    }
    if (HasDeviceSecret() || HasProvisionKey()) {
        std::string signature = CalculateSignature(body, timestamp);
        if (!signature.empty()) {
            http->SetHeader("Authorization", ("Bearer " + signature).c_str());
        }
    }

    http->SetContent(std::move(body));
    ESP_LOGI(TAG, "HTTP Open...");
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "HTTP request failed");
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "HTTP status: %d", status_code);
    if (status_code != 200) {
        ESP_LOGE(TAG, "OTA request failed, status: %d", status_code);
        http->Close();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ReadAll...");
    std::string response = http->ReadAll();
    ESP_LOGI(TAG, "Response len: %d", response.length());
    http->Close();

    ESP_LOGI(TAG, "ParseResponse...");
    esp_err_t ret = ParseResponse(response);
    ESP_LOGI(TAG, "ParseResponse done: %d", ret);
    if (ret != ESP_OK) {
        return ret;
    }

    // 处理密钥轮换
    if (has_rotated_key_ && !rotate_key_.empty()) {
        SaveDeviceSecret(rotate_key_);
        ESP_LOGI(TAG, "Device key rotated");
    }

    ESP_LOGI(TAG, "CheckVersion return OK");
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get partition state");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading from %s", firmware_url.c_str());

    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open firmware URL");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Firmware download failed, status: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Empty firmware");
        return false;
    }

    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();

    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Read error");
            return false;
        }

        recent_read += ret;
        total_read += ret;

        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) break;

        if (!image_header_checked) {
            image_header.append(buffer, ret);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));
                ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "OTA begin failed");
                    return false;
                }
                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        esp_err_t err = esp_ota_write(update_handle, buffer, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            return false;
        }
    }
    http->Close();

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    if (firmware_url_.empty()) {
        ESP_LOGE(TAG, "No firmware URL");
        return false;
    }
    return Upgrade(firmware_url_, callback);
}

// ============ 版本比较 ============

std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> numbers;
    std::stringstream ss(version);
    std::string segment;

    while (std::getline(ss, segment, '.')) {
        try {
            numbers.push_back(std::stoi(segment));
        } catch (...) {
            numbers.push_back(0);
        }
    }
    return numbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);

    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) return true;
        if (newer[i] < current[i]) return false;
    }
    return newer.size() > current.size();
}
