#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /**
     * Handle network event (called from WiFi manager callbacks)
     * @param event The network event type
     * @param data Additional data (e.g., SSID for Connecting/Connected events)
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * Start WiFi connection attempt
     */
    void TryWifiConnect();

    /**
     * Enter WiFi configuration mode
     */
    void StartWifiConfigMode();

    /**
     * WiFi connection timeout callback
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * 异步启动网络连接  
     * 此函数立即返回。网络事件通过 SetNetworkEventCallback() 设置的回调函数进行通知。
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    void EnterWifiConfigMode();
    bool IsInWifiConfigMode() const;
};

#endif // WIFI_BOARD_H
