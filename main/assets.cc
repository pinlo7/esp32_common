#include "assets.h"
#include "esp_log.h"
#include "spi_flash_mmap.h"
#include <esp_timer.h>
#include "cJSON.h"
#include "lvgl_theme.h"

#define TAG "Assets"
#define PARTITION_LABEL "assets"
struct mmap_assets_table {
    char asset_name[32];          /*!< Name of the asset */
    uint32_t asset_size;          /*!< Size of the asset */
    uint32_t asset_offset;        /*!< Offset of the asset */
    uint16_t asset_width;         /*!< Width of the asset */
    uint16_t asset_height;        /*!< Height of the asset */
};

Assets::Assets() {
    ESP_LOGI(TAG, "init partition checksum is %d", InitializePartition());
}
Assets::~Assets() {
    if (mmap_handle_ != 0) {
        esp_partition_munmap(mmap_handle_);
        mmap_handle_ = 0;
        mmap_root_ = nullptr;
    }
    checksum_valid_ = false;
    assets_.clear();
}
bool Assets::InitializePartition() {
    partition_valid_ = false;
    assets_.clear();

    partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
    if (partition_ == nullptr) {
        ESP_LOGE(TAG, "Failed to find partition with label %s", PARTITION_LABEL);
        return false;
    }
    int free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    uint32_t storage_size = free_pages * 64 * 1024;
    ESP_LOGI(TAG, "The storage free size is %ld KB", storage_size / 1024);
    ESP_LOGI(TAG, "The partition size is %ld KB", partition_->size / 1024);
    if (storage_size < partition_->size) {
        ESP_LOGE(TAG, "The free size %ld KB is less than assets partition required %ld KB", storage_size / 1024, partition_->size / 1024);
        return false;
    }
    esp_err_t err = esp_partition_mmap(partition_, 0, partition_->size, ESP_PARTITION_MMAP_DATA, (const void**)&mmap_root_, &mmap_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap assets partition: %s", esp_err_to_name(err));
        return false;
    }
    partition_valid_ = true;

    uint32_t stored_files = *(uint32_t*)(mmap_root_ + 0);
    uint32_t stored_chksum = *(uint32_t*)(mmap_root_ + 4);
    uint32_t stored_len = *(uint32_t*)(mmap_root_ + 8);

    if (stored_len > partition_->size - 12) {
        ESP_LOGD(TAG, "The stored_len (0x%lx) is greater than the partition size (0x%lx) - 12", stored_len, partition_->size);
        return false;
    }

    auto start_time = esp_timer_get_time();
    uint32_t calculated_checksum = CalculateChecksum(mmap_root_ + 12, stored_len);
    auto end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "The checksum calculation time is %d ms", int((end_time - start_time) / 1000));

    if (calculated_checksum != stored_chksum) {
        ESP_LOGE(TAG, "The calculated checksum (0x%lx) does not match the stored checksum (0x%lx)", calculated_checksum, stored_chksum);
        return false;
    }

    checksum_valid_ = true;
    // ESP_LOGI(TAG, "文件数量:%d", stored_files);
    for (uint32_t i = 0; i < stored_files; i++) {
        auto item = (const mmap_assets_table*)(mmap_root_ + 12 + i * sizeof(mmap_assets_table));
        // ESP_LOGI(TAG, "%d : %s, %d, %d, %d, %d", i, item->asset_name, item->asset_height, item->asset_offset, item->asset_size, item->asset_width);
        auto asset = Asset{
            .size = static_cast<size_t>(item->asset_size),
            .offset = static_cast<size_t>(12 + sizeof(mmap_assets_table) * stored_files + item->asset_offset)
        };
        assets_[item->asset_name] = asset;
    }
    return checksum_valid_;
}
uint32_t Assets::CalculateChecksum(const char* data, uint32_t length) {
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum & 0xFFFF;
}
bool Assets::GetAssetData(const std::string& name, void*& ptr, size_t& size) {
    auto asset = assets_.find(name);
    if (asset == assets_.end()) {
        return false;
    }
    auto data = (const char*)(mmap_root_ + asset->second.offset);
    if (data[0] != 'Z' || data[1] != 'Z') {
        ESP_LOGE(TAG, "The asset %s is not valid with magic %02x%02x", name.c_str(), data[0], data[1]);
        return false;
    }

    ptr = static_cast<void*>(const_cast<char*>(data + 2));
    size = asset->second.size;
    return true;
}
bool Assets::Apply() {
    void* ptr = nullptr;
    size_t size = 0;
    if (!GetAssetData("index.json", ptr, size)) {
        ESP_LOGE(TAG, "The index.json file is not found");
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "The index.json file is not valid");
        return false;
    }
    // ESP_LOGI(TAG, "The index.json file is %s", cJSON_Print(root));
    auto& theme_manager = LvglThemeManager::GetInstance();
    cJSON* font = cJSON_GetObjectItem(root, "text_font");
    if (cJSON_IsString(font)) {
        std::string fonts_text_file = font->valuestring;
        if (GetAssetData(fonts_text_file, ptr, size)) {
            auto text_font = std::make_shared<LvglCBinFont>(ptr);
            if (text_font->font() == nullptr) {
                ESP_LOGE(TAG, "Failed to load fonts.bin");
                return false;
            } 
            theme_manager.GetTheme("default")->set_text_font(text_font);
        } else {
            ESP_LOGE(TAG, "The font file %s is not found", fonts_text_file.c_str());
        }
    }
    cJSON_Delete(root);
    return true;
}