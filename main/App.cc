#include "App.h"
#include <esp_log.h>
#include <cJSON.h>
#include "board.h"

#define TAG "app"

App::App() {
    event_group_ = xEventGroupCreate();
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            App* app = (App*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true   //如果上一次定时器回调还没执行完毕，新触发的周期事件会被直接跳过 / 丢弃，不会堆积
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
    auto& board = Board::GetInstance();

    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        std::lock_guard<std::mutex> lock(network_mutex_);
        network_state_.Update(event, data);
        xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK);
    });
    board.StartNetwork();
}

void App::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_NETWORK
        ;
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_NETWORK) {
            HandleNetworkEvent();
        }
    }
    
}

void App::HandleNetworkEvent() {
    NetworkState state;
    {
        std::lock_guard<std::mutex> lock(network_mutex_);
        if(!network_state_.pending) return; //没有待处理就退出
        state = network_state_;
        network_state_.Reset(); //重置 pending=false
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

    xTaskCreate([](void* arg) {
        App* app = static_cast<App*>(arg);
        app->ActivationTask();
        app->activation_task_handle_ = nullptr;
        vTaskDelete(NULL);
    }, "activation", 4096 * 2, this, 2, &activation_task_handle_);

    /** 执行一些联网成功显示的一些操作 */
}

void App::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}