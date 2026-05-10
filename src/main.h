#ifndef __MAIN_H__
#define __MAIN_H__

#include <Arduino.h>

/* ------------------------- CUSTOM GPIO PIN MAPPING ------------------------- */

#define KEY_B1 9
#define KEY_B2 0
#define LED 10
#define OT 1

#define DIS_RST 4
#define DIS_DC 5
#define DIS_SCK 6
#define DIS_SDA 7
#define DIS_BL 8

#define HW_VERSION "1.2"

struct WifiConfig_t
{
  char stassid[32];//定义配网得到的WIFI名长度(最大32字节)
  char stapsw[64];//定义配网得到的WIFI密码长度(最大64字节)
};


extern String hw_ver;
extern String sw_ver;

//littlefs configuration file path
#define CONFIG_FILE_PATH "/config.json"

void HandleConfig();
void HandleNotFound();

#endif
