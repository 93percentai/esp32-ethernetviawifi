#pragma once

/*
 * LilyGO T-Dongle-S3 (with LCD) board map
 *
 * Confirmed from LilyGO docs, factory examples, Zephyr board support,
 * and ESPHome device profiles. USB-A D+/D- are hard-wired to the ESP32-S3
 * USB-OTG PHY (GPIO20 / GPIO19). TinyUSB gadget mode must use that PHY;
 * USB-Serial/JTAG is unavailable while OTG is active.
 */

#include "driver/gpio.h"

/* ST7735 0.96" IPS, 80x160 native; we run landscape 160x80 */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_PIN_MOSI      GPIO_NUM_3
#define BOARD_LCD_PIN_SCLK      GPIO_NUM_5
#define BOARD_LCD_PIN_CS        GPIO_NUM_4
#define BOARD_LCD_PIN_DC        GPIO_NUM_2
#define BOARD_LCD_PIN_RST       GPIO_NUM_1
#define BOARD_LCD_PIN_BL        GPIO_NUM_38
#define BOARD_LCD_BL_ON_LEVEL   0   /* active-low backlight */
#define BOARD_LCD_H_RES         160
#define BOARD_LCD_V_RES         80
#define BOARD_LCD_GAP_X         1   /* landscape gap from LilyGO lcd example */
#define BOARD_LCD_GAP_Y         26

/* APA102 RGB LED */
#define BOARD_LED_PIN_CLK       GPIO_NUM_39
#define BOARD_LED_PIN_DATA      GPIO_NUM_40

/* Boot / user button */
#define BOARD_BTN_PIN           GPIO_NUM_0

/* Hidden TF slot (SDMMC) — unused by this firmware, documented for reference */
#define BOARD_SD_PIN_CLK        GPIO_NUM_12
#define BOARD_SD_PIN_CMD        GPIO_NUM_16
#define BOARD_SD_PIN_D0         GPIO_NUM_14
#define BOARD_SD_PIN_D1         GPIO_NUM_17
#define BOARD_SD_PIN_D2         GPIO_NUM_21
#define BOARD_SD_PIN_D3         GPIO_NUM_18

/* USB-OTG PHY (fixed on ESP32-S3) */
#define BOARD_USB_DP            GPIO_NUM_20
#define BOARD_USB_DM            GPIO_NUM_19
