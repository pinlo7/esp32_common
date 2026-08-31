#ifndef DISPLAY_H
#define DISPLAY_H
#include "esp_lv_adapter.h"

class Display {
public:
    Display();
    ~Display();

    void Init();
private:
    void InitSpi();
    void initDisplay();
    // void InitGpio();
};




#endif