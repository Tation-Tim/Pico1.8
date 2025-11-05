#ifndef _PIN_CONFIG_H_
#define _PIN_CONFIG_H_

// --------- LCD SPI (QSPI) ---------
#define LCD_CS     1
#define LCD_SCLK   2
#define LCD_SDIO0  3
#define LCD_SDIO1  4
#define LCD_SDIO2  5
#define LCD_SDIO3  6

// --------- I2C for FT3168 + AXP2101 ---------
#define IIC_SDA    4
#define IIC_SCL    5

// --------- Optional Audio / Power / Buttons ---------
#define PA         10
#define TP_INT     11

// (adjust these to your actual board pinout if different)
#endif
