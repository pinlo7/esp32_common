#ifndef ASSETS_H
#define ASSETS_H
#include "esp_partition.h"
#include <map>
#include <string>
#include "lvgl_font.h"
#include <memory>

struct Asset {
    size_t size;
    size_t offset;
};


class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets();
    bool GetAssetData(const std::string& name, void*& ptr, size_t& size);
    bool Apply();
    const lv_font_t* text_font = nullptr;
    
private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;
    bool InitializePartition();
    static uint32_t CalculateChecksum(const char* data, uint32_t length);

    const esp_partition_t* partition_ = nullptr;
    bool partition_valid_ = false;
    std::map<std::string, Asset> assets_;
    const char* mmap_root_ = nullptr;
    bool checksum_valid_ = false;       
    esp_partition_mmap_handle_t mmap_handle_ = 0;
};

#endif
