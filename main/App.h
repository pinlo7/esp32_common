#pragma once
#ifndef _APP_H_
#define _APP_H_
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include "board.h"
#include "ota.h"
#include "mqtt_service.h"

#define MAIN_EVENT_CLOCK_TICK           BIT0
#define MAIN_EVENT_NETWORK              BIT1


class App {
private:
    App(/* args */);
    ~App();

    EventGroupHandle_t event_group_;
    esp_timer_handle_t clock_timer_handle_;
    //网络状态 保存最新的状态,通知后在主任务中执行,避免在网络事件回调中执行
    NetworkState network_state_;
    std::mutex network_mutex_;
    TaskHandle_t activation_task_handle_;
    std::unique_ptr<Ota> ota_;
    std::unique_ptr<MqttService> mqtt_service_;

    void HandleNetworkEvent();
    void HandleNetworkConnectedEvent();
    void ActivationTask();
    void CheckNewVersion();
    bool UpgradeFirmware(const std::string& url, const std::string& version);

public:
    static App& GetInstance() {
        static App instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /**
     * 初始化应用程序  
     * 用于设置显示、音频、网络回调等  
     * 网络连接异步启动
     */
    void Initialize();
    /**
     * 运行主事件循环  
     * 该函数在主线程中运行，永远不会返回。  
     * 它处理所有事件，包括网络事件、状态变化和用户交互。
     */
    void Run();  
};

#endif
