#pragma once
#ifndef _OTA_H_
#define _OTA_H_

#include <string>
#include <functional>
#include <vector>
#include <esp_err.h>

/**
 * OTA 更新管理类
 *
 * 实现设备激活、固件版本检查、固件升级和密钥轮换功能。
 * 签名算法：SHA256(device_key + timestamp + device_id + body_sha256)
 */
class Ota {
public:
    Ota();
    ~Ota();

    /**
     * 设置首次激活引导密钥（group_key）。
     * 设备尚未激活（无 device_secret）时，OTA 请求使用该密钥签名并携带 X-Provision-Key。
     */
    void SetProvisionKey(const std::string& key) { provision_key_ = key; }
    bool HasProvisionKey() const { return !provision_key_.empty(); }
    void ClearProvisionKey() { provision_key_.clear(); }

    /**
     * 检查固件版本并获取 OTA 响应
     * @return ESP_OK 成功，其他表示错误
     */
    esp_err_t CheckVersion();

    /**
     * 开始固件升级
     * @param callback 进度回调 (progress%, speed_bytes_per_sec)
     * @return true 成功，false 失败
     */
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);

    /**
     * 静态方法：执行固件升级
     * @param firmware_url 固件下载 URL
     * @param expected_checksum 期望的固件 SHA256（hex），空串则跳过校验
     * @param expected_file_size 期望的固件字节数，<=0 则跳过大小核对
     * @param callback 进度回调
     * @return true 成功，false 失败
     */
    static bool Upgrade(const std::string& firmware_url,
                        const std::string& expected_checksum,
                        long long expected_file_size,
                        std::function<void(int progress, size_t speed)> callback);

    /**
     * 标记当前固件为有效（取消回滚）
     */
    void MarkCurrentVersionValid();

    // 状态查询
    bool HasActivationCode() const { return has_activation_code_; }
    bool HasNewVersion() const { return has_new_version_; }
    bool HasMqttConfig() const { return has_mqtt_config_; }
    bool HasWebsocketConfig() const { return has_websocket_config_; }
    bool HasServerTime() const { return has_server_time_; }
    bool HasRotatedKey() const { return has_rotated_key_; }
    bool HasDeviceSecret() const;

    // 数据访问
    const std::string& GetActivationCode() const { return activation_code_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetFirmwareChecksum() const { return firmware_checksum_; }
    long long GetFirmwareFileSize() const { return firmware_file_size_; }
    const std::string& GetCurrentVersion() const { return current_version_; }

private:
    // NVS 密钥管理
    std::string GetOtaUrl();
    std::string GetDeviceSecret();
    bool SaveDeviceSecret(const std::string& key);

    // 签名计算
    std::string CalculateSignature(const std::string& body_json, const std::string& timestamp);
    std::string Sha256Hex(const std::string& input);
    std::string GetTimestamp();

    // HTTP 请求
    std::string BuildRequestBody();
    esp_err_t ParseResponse(const std::string& response);

    // 状态标志
    bool has_activation_code_ = false;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_rotated_key_ = false;

    // 数据
    std::string activation_code_;
    std::string activation_message_;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string firmware_checksum_;   // 固件 SHA256（hex），下载后完整性校验
    long long firmware_file_size_ = 0;  // 固件字节数，下载后核对
    std::string device_key_;  // 从 NVS 加载的设备密钥（hex）
    std::string rotate_key_;  // OTA 响应下发的新密钥
    std::string provision_key_;  // 首次激活引导密钥（仅激活阶段使用，激活成功后清除）

    // 版本比较
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
};

#endif // _OTA_H_
