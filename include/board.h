// Pin della LilyGO T-Deck / T-Deck Plus
// Fonte: examples/UnitTest/utilities.h del repository ufficiale Xinyuan-LilyGO/T-Deck
#pragma once

// Alimentazione delle periferiche: va messo HIGH prima di usarle
#define BOARD_POWERON 10

// I2C (touch GT911 e tastiera ESP32-C3)
#define BOARD_I2C_SDA 18
#define BOARD_I2C_SCL 8
#define BOARD_TOUCH_INT 16
#define BOARD_KEYBOARD_ADDR 0x55

// SPI condiviso da display, SD e LoRa
#define BOARD_SPI_SCK  40
#define BOARD_SPI_MISO 38
#define BOARD_SPI_MOSI 41
#define BOARD_TFT_CS   12
#define BOARD_TFT_DC   11
#define BOARD_TFT_BACKLIGHT 42
#define BOARD_SDCARD_CS 39
#define BOARD_RADIO_CS  9

// Trackball: ogni scatto cambia lo stato del pin
#define BOARD_TBOX_RIGHT 2  // G02
#define BOARD_TBOX_UP    3  // G01
#define BOARD_TBOX_LEFT  1  // G04
#define BOARD_TBOX_DOWN  15 // G03
#define BOARD_BOOT_PIN   0  // pressione della trackball

// GPS integrato (T-Deck Plus)
#define BOARD_GPS_RX_PIN 44
#define BOARD_GPS_TX_PIN 43

// Radio LoRa SX1262
#define BOARD_RADIO_DIO1 45
#define BOARD_RADIO_RST  17
#define BOARD_RADIO_BUSY 13

// Tensione della batteria: partitore 1:2 su un ingresso analogico
#define BOARD_BAT_ADC 4
