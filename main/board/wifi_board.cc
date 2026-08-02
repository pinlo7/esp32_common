#include "wifi_board.h"

#include "system_info.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <utility>
// #include "Cjson"


#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
// #include "afsk_demod.h"


static const char *TAG = "WifiBoard";
#define BOARD_TYPE "wifi_board"

// 连接超时最大时间  （秒）
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // 创建连接超时定时器
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    wifi_manager.Initialize(config);

    // 设置统一的事件回调——转发至包含SSID数据的NetworkEvent
    wifi_manager.SetEventCallback([this, &wifi_manager](WifiEvent event) {
        std::string ssid = wifi_manager.GetSsid();
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, ssid);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, ssid);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // 开始连接尝试并设置超时
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // 未配置SSID，进入配置模式
        // 等待显示板子版本
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // 停止超时计时器
            esp_timer_stop(connect_timer_);
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // 如果已设置，通知外部回调
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();   
    board->StartWifiConfigMode();
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    auto& wifi_manager = WifiManager::GetInstance();
    wifi_manager.StartConfigAp();
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    // 停止正在进行的连接尝试
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();
    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}
