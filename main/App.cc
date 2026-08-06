#include "App.h"
#include "board.h"
#include "ota.h"
#include <cJSON.h>
#include <esp_log.h>
#include <system_info.h>

#define TAG "app"

App::App() {
    event_group_ = xEventGroupCreate();
    esp_timer_create_args_t clock_timer_args = {
        .callback =
            [](void *arg) {
                App *app = (App *)arg;
                xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true // 如果上一次定时器回调还没执行完毕，新触发的周期事件会被直接跳过
                                      // / 丢弃，不会堆积
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

App::~App() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
}

void App::Initialize() {
    // Initialize and run the application
    auto &board = Board::GetInstance();

    board.SetNetworkEventCallback(
        [this](NetworkEvent event, const std::string &data) {
            std::lock_guard<std::mutex> lock(network_mutex_);
            network_state_.Update(event, data);
            xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK);
        });
    board.StartNetwork();
}

void App::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_NETWORK;
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE,
                                        pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_NETWORK) {
            HandleNetworkEvent();
        }
    }
}

void App::HandleNetworkEvent() {
    NetworkState state;
    {
        std::lock_guard<std::mutex> lock(network_mutex_);
        if (!network_state_.pending)
            return; // 没有待处理就退出
        state = network_state_;
        network_state_.Reset(); // 重置 pending=false
    }

    switch (network_state_.event) {
    case NetworkEvent::Scanning:
        ESP_LOGI(TAG, "Scanning...");
        // display->SetStatus("Scanning...");
        break;

    case NetworkEvent::Connecting:
        ESP_LOGI(TAG, "Connecting to %s", network_state_.ssid.c_str());
        // display->SetStatus("Connecting...");
        break;

    case NetworkEvent::Connected:
        HandleNetworkConnectedEvent();
        break;

    case NetworkEvent::Disconnected:
        ESP_LOGW(TAG, "Disconnected");
        // display->SetStatus("Disconnected");
        // display->SetNetworkIcon("wifi_off");
        break;

    case NetworkEvent::ModemErrorNoSim:
        ESP_LOGE(TAG, "No SIM card");
        // Alert("Error", "No SIM card", "warning");
        break;

    case NetworkEvent::ModemErrorRegDenied:
        ESP_LOGE(TAG, "Registration denied");
        // Alert("Error", "Registration denied", "warning");
        break;

    case NetworkEvent::ModemErrorInitFailed:
        ESP_LOGE(TAG, "Modem init failed");
        // Alert("Error", "Modem init failed", "warning");
        break;

    default:
        break;
    }
}

void App::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "network connected!");

    // 创建任务激活设备,OTA等待操作
    if (activation_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Activation task already running");
        return;
    }

    xTaskCreate(
        [](void *arg) {
            App *app = static_cast<App *>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        },
        "activation", 4096 * 2, this, 2, &activation_task_handle_);

    /** 执行一些联网成功显示的一些操作 */
}

void App::ActivationTask() { 
    ota_ = std::make_unique<Ota>();
    CheckNewVersion();
    ESP_LOGI(TAG, "测试....全新版本 %s", ota_->GetCurrentVersion().c_str());
}
void App::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds
    auto &board = Board::GetInstance();
    SystemInfo::PrintSystemDateTime();
    while (true) {
        // auto display = board.GetDisplay();
        // display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }
            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay
        
        
        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(),
                                ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        break;
    }
}
bool App::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    // auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    // if (protocol_ && protocol_->IsAudioChannelOpened()) {
        // ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        // protocol_->CloseAudioChannel();
    // }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    // Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    // SetDeviceState(kDeviceStateUpgrading);

    // std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    // display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    // audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [](int progress, size_t speed) {
        ;
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        // audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::BALANCED); // Restore power save level
        // Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        // display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        esp_restart();
        return true;
    }
}