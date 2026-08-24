#ifndef _MY_WIFI_PCB_BOARD_H_
#define _MY_WIFI_PCB_BOARD_H_
#include "wifi_board.h"
#include <string>

#define TAG "My_Wifi_Pcb_Board"

class MyWifiPcbBoard : public WifiBoard {
private:
    //初始化外设在这里
public:
    MyWifiPcbBoard(/* args */);
    ~MyWifiPcbBoard();

    std::string GetBoardType() override;
};

MyWifiPcbBoard::MyWifiPcbBoard(/* args */) {
}

MyWifiPcbBoard::~MyWifiPcbBoard() {
}

std::string MyWifiPcbBoard::GetBoardType() {
    return "MyWifiPcbBoard";
}

DECLARE_BOARD(MyWifiPcbBoard);

#endif