#include "App.h"
#include "board.h"
#include "ota.h"
#include <cJSON.h>
#include <esp_log.h>
#include <system_info.h>
#include "settings.h"
#include <esp_sntp.h>
#include <esp_app_desc.h>
#include <time.h>

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

    // 首次激活前必须先同步系统时间：服务器对 OTA 请求做时间戳校验（±300s）
    SyncTime();

    // 未激活且配网时保存了引导密钥：进入首次激活流程
    if (!ota_->HasDeviceSecret()) {
        Settings wifi_settings("wifi", false);
        std::string group_key = wifi_settings.GetString("group_key");
        if (!group_key.empty()) {
            ota_->SetProvisionKey(group_key);
            ESP_LOGI(TAG, "First activation with provision key");
        } else {
            ESP_LOGW(TAG, "No device secret and no provision key, activation not possible");
        }
    }

    CheckNewVersion();
    ESP_LOGI(TAG, "当前版本 %s", ota_->GetCurrentVersion().c_str());

    // 激活/OTA 检查完成后启动 MQTT 服务（升级成功会重启，不会走到这里）
    if (ota_->HasDeviceSecret() && !mqtt_service_) {
        mqtt_service_ = std::make_unique<MqttService>();
        mqtt_service_->SetOnUpgradeRequested([this](bool force) { UpgradeByCommand(force); });
        if (mqtt_service_->Start()) {
            ESP_LOGI(TAG, "MQTT service started");

            // 升级后首次启动：上报升级完成并清除标记
            Settings device_settings("device", false);
            if (!device_settings.GetString("upgrade_pending").empty()) {
                auto app_desc = esp_app_get_description();
                std::string event = std::string(R"({"event":"upgrade_completed","version":")")
                                   + app_desc->version + "\"}";
                mqtt_service_->PublishEvent(event);
                Settings device_rw("device", true);
                device_rw.EraseKey("upgrade_pending");
                ESP_LOGI(TAG, "Upgrade completed reported (version=%s)", app_desc->version);
            }
        } else {
            ESP_LOGW(TAG, "MQTT service start failed");
        }
    }
}

void App::UpgradeByCommand(bool force) {
    ESP_LOGI(TAG, "Upgrade command received (force=%d), starting in dedicated task", force ? 1 : 0);

    // 在独立任务中执行，避免阻塞 MQTT 事件回调（CheckVersion/下载都会耗时）
    struct UpgradeCmdArg {
        App* app;
        bool force;
    };
    auto* arg = new UpgradeCmdArg{this, force};
    xTaskCreate(
        [](void* arg) {
            auto* a = static_cast<UpgradeCmdArg*>(arg);
            auto* app = a->app;

            esp_err_t err = app->ota_->CheckVersion();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Upgrade command: CheckVersion failed");
                if (app->mqtt_service_) {
                    app->mqtt_service_->PublishEvent(
                        R"({"event":"upgrade_failed","message":"check version failed"})");
                }
                vTaskDelete(NULL);
                delete a;
                return;
            }

            if (!app->ota_->HasNewVersion() && !a->force) {
                ESP_LOGI(TAG, "Upgrade command: already up to date");
                delete a;
                vTaskDelete(NULL);
                return;
            }

            // 服务器没有可下载固件（如型号不匹配或未上传）时明确失败，避免空 URL 下载
            std::string firmware_url = app->ota_->GetFirmwareUrl();
            if (firmware_url.empty()) {
                ESP_LOGE(TAG, "Upgrade command: no firmware available on server");
                if (app->mqtt_service_) {
                    app->mqtt_service_->PublishEvent(
                        R"({"event":"upgrade_failed","message":"no firmware available"})");
                }
                delete a;
                vTaskDelete(NULL);
                return;
            }

            // 上报升级开始 + 打持久化标记（新固件启动后据此上报升级完成）
            if (app->mqtt_service_) {
                std::string event = std::string(R"({"event":"upgrade_started","version":")")
                                   + app->ota_->GetFirmwareVersion() + "\"}";
                app->mqtt_service_->PublishEvent(event);
            }
            Settings upgrade_mark("device", true);
            upgrade_mark.SetString("upgrade_pending", "1");

            bool ok = app->UpgradeFirmware(firmware_url, app->ota_->GetFirmwareVersion());
            if (!ok && app->mqtt_service_) {
                app->mqtt_service_->PublishEvent(
                    R"({"event":"upgrade_failed","message":"upgrade failed"})");
            }
            // 升级失败：清除标记，设备继续正常运行（升级成功会重启，不会走到这里）
            upgrade_mark.EraseKey("upgrade_pending");
            delete a;
            vTaskDelete(NULL);
        },
        "upgrade_cmd", 4096 * 4, arg, 3, nullptr);
}

void App::SyncTime() {
    ESP_LOGI(TAG, "Syncing system time via SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_init();

    // 等待同步成功（最多 20 秒）；以时间超过 2020 年作为成功判据
    int waited_ms = 0;
    while (waited_ms < 20000) {
        time_t now = 0;
        time(&now);
        if (now > 1600000000) {
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
            ESP_LOGI(TAG, "Time synced: %s", buf);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        waited_ms += 100;
    }
    ESP_LOGW(TAG, "SNTP sync timeout, continue with system time");
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
