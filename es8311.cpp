/*
 * ESPRESSIF MIT License - Modified for RP2350
 * 
 * ES8311 Audio Codec Driver
 * Fixed for Arduino RP2350 compatibility
 * 
 * NOTE: This file MUST be named es8311.cpp (not .c) to compile correctly
 */

#include "es8311.h"
#include "DEV_Config.h"
#include <string.h>
#include <Arduino.h>

#ifndef BIT
#define BIT(nr)             (1 << (nr))
#endif

/* ES8311 I2C address */
#define ES8311_ADDR         0x18

/* Clock source configuration */
#define FROM_MCLK_PIN       1
#define FROM_SCLK_PIN       0

/* Clock inversion */
#define INVERT_MCLK         0
#define INVERT_SCLK         0

#define IS_DMIC             0  // Digital microphone

/* Clock coefficient structure */
struct _coeff_div {
    uint32_t mclk;
    uint32_t rate;
    uint8_t pre_div;
    uint8_t pre_multi;
    uint8_t adc_div;
    uint8_t dac_div;
    uint8_t fs_mode;
    uint8_t lrck_h;
    uint8_t lrck_l;
    uint8_t bclk_div;
    uint8_t adc_osr;
    uint8_t dac_osr;
};

/* Codec clock divider coefficients */
static const struct _coeff_div coeff_div[] = {
    /* 8k */
    {12288000, 8000, 0x06, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 8000, 0x03, 0x01, 0x03, 0x03, 0x00, 0x05, 0xff, 0x18, 0x10, 0x10},
    {16384000, 8000, 0x08, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000, 8000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    /* 16k */
    {12288000, 16000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 16000, 0x03, 0x01, 0x03, 0x03, 0x00, 0x02, 0xff, 0x0c, 0x10, 0x10},
    {16384000, 16000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    /* 44.1k */
    {11289600, 44100, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    /* 48k */
    {12288000, 48000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
};

static int get_coeff(uint32_t mclk, uint32_t rate)
{
    for (int i = 0; i < (sizeof(coeff_div) / sizeof(coeff_div[0])); i++) {
        if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk)
            return i;
    }
    return -1;
}

void es8311_write_reg(uint8_t reg_addr, uint8_t data)
{
    DEV_I2C_Write_Byte(ES8311_ADDR, reg_addr, data);
}

uint8_t es8311_read_reg(uint8_t reg_addr)
{
    return DEV_I2C_Read_Byte(ES8311_ADDR, reg_addr);
}

int es8311_sample_frequency_config(int mclk_frequency, int sample_frequency)
{
    uint8_t regv;

    int coeff = get_coeff(mclk_frequency, sample_frequency);

    if (coeff < 0) {
        Serial.print("Unable to configure sample rate ");
        Serial.print(sample_frequency);
        Serial.print("Hz with ");
        Serial.print(mclk_frequency);
        Serial.println("Hz MCLK");
        return -1;
    }

    const struct _coeff_div *const selected_coeff = &coeff_div[coeff];

    /* Configure clock registers */
    regv = es8311_read_reg(ES8311_CLK_MANAGER_REG02);
    regv &= 0x07;
    regv |= (selected_coeff->pre_div - 1) << 5;
    regv |= selected_coeff->pre_multi << 3;
    es8311_write_reg(ES8311_CLK_MANAGER_REG02, regv);

    const uint8_t reg03 = (selected_coeff->fs_mode << 6) | selected_coeff->adc_osr;
    es8311_write_reg(ES8311_CLK_MANAGER_REG03, reg03);

    es8311_write_reg(ES8311_CLK_MANAGER_REG04, selected_coeff->dac_osr);

    const uint8_t reg05 = ((selected_coeff->adc_div - 1) << 4) | (selected_coeff->dac_div - 1);
    es8311_write_reg(ES8311_CLK_MANAGER_REG05, reg05);

    regv = es8311_read_reg(ES8311_CLK_MANAGER_REG06);
    regv &= 0xE0;
    if (selected_coeff->bclk_div < 19) {
        regv |= (selected_coeff->bclk_div - 1) << 0;
    } else {
        regv |= (selected_coeff->bclk_div) << 0;
    }
    es8311_write_reg(ES8311_CLK_MANAGER_REG06, regv);

    regv = es8311_read_reg(ES8311_CLK_MANAGER_REG07);
    regv &= 0xC0;
    regv |= selected_coeff->lrck_h << 0;
    es8311_write_reg(ES8311_CLK_MANAGER_REG07, regv);

    es8311_write_reg(ES8311_CLK_MANAGER_REG08, selected_coeff->lrck_l);

    return 0;
}

static void es8311_clock_config(pico_audio_t pico_audio)
{
    uint8_t reg06;
    uint8_t reg01 = 0x3F;
    int mclk_hz;

    if (FROM_MCLK_PIN) {
        mclk_hz = pico_audio.mclk_freq;
    } else {
        mclk_hz = pico_audio.sample_freq * (int)pico_audio.res_out * 2;
        reg01 |= BIT(7);
    }

    if (INVERT_MCLK) {
        reg01 |= BIT(6);
    }
    es8311_write_reg(ES8311_CLK_MANAGER_REG01, reg01);

    reg06 = es8311_read_reg(ES8311_CLK_MANAGER_REG06);
    if (INVERT_SCLK) {
        reg06 |= BIT(5);
    } else {
        reg06 &= ~BIT(5);
    }
    reg06 |= 0x03;
    es8311_write_reg(ES8311_CLK_MANAGER_REG06, reg06);

    es8311_sample_frequency_config(mclk_hz, pico_audio.sample_freq);
}

static int es8311_resolution_config(const es8311_resolution_t res, uint8_t *reg)
{
    switch (res) {
    case ES8311_RESOLUTION_16:
        *reg |= (3 << 2);
        break;
    case ES8311_RESOLUTION_18:
        *reg |= (2 << 2);
        break;
    case ES8311_RESOLUTION_20:
        *reg |= (1 << 2);
        break;
    case ES8311_RESOLUTION_24:
        *reg |= (0 << 2);
        break;
    case ES8311_RESOLUTION_32:
        *reg |= (4 << 2);
        break;
    default:
        return -1;
    }
    return 0;
}

static int es8311_fmt_config(pico_audio_t pico_audio)
{
    uint8_t reg09 = 0;
    uint8_t reg0a = 0;

    Serial.println("ES8311 in Master mode and I2S format");
    uint8_t reg00 = es8311_read_reg(ES8311_RESET_REG00);
    reg00 |= 0x40;
    es8311_write_reg(ES8311_RESET_REG00, reg00);

    es8311_resolution_config((es8311_resolution_t)pico_audio.res_in, &reg09);
    es8311_resolution_config((es8311_resolution_t)pico_audio.res_out, &reg0a);

    es8311_write_reg(ES8311_SDPIN_REG09, reg09);
    es8311_write_reg(ES8311_SDPOUT_REG0A, reg0a);

    return 0;
}

void es8311_microphone_config()
{
    uint8_t reg14 = 0x1A;

    if (IS_DMIC) {
        reg14 |= BIT(6);
    }
    es8311_write_reg(ES8311_ADC_REG17, 0xFF);
    es8311_write_reg(ES8311_SYSTEM_REG14, reg14);
}

void es8311_init(pico_audio_t pico_audio)
{
    /* Reset ES8311 */
    es8311_write_reg(ES8311_RESET_REG00, 0x1F);
    DEV_Delay_ms(20);
    es8311_write_reg(ES8311_RESET_REG00, 0x00);
    es8311_write_reg(ES8311_RESET_REG00, 0x80);

    /* Setup clock */
    es8311_clock_config(pico_audio);

    /* Setup format */
    es8311_fmt_config(pico_audio);

    /* Power up analog circuitry */
    es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01);
    es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02);
    es8311_write_reg(ES8311_SYSTEM_REG12, 0x00);
    es8311_write_reg(ES8311_SYSTEM_REG13, 0x10);
    es8311_write_reg(ES8311_ADC_REG1C, 0x6A);
    es8311_write_reg(ES8311_DAC_REG37, 0x08);
}

int es8311_voice_volume_set(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    int reg32;
    if (volume == 0) {
        reg32 = 0;
    } else {
        reg32 = ((volume) * 256 / 100) - 1;
    }

    es8311_write_reg(ES8311_DAC_REG32, reg32);
    return volume;
}

int es8311_voice_volume_get()
{
    uint8_t reg32 = es8311_read_reg(ES8311_DAC_REG32);
    uint8_t volume;

    if (reg32 == 0) {
        volume = 0;
    } else {
        volume = ((reg32 * 100) / 256) + 1;
    }
    return volume;
}

void es8311_voice_mute(bool mute)
{
    uint8_t reg31 = es8311_read_reg(ES8311_DAC_REG31);

    if (mute) {
        reg31 |= BIT(6) | BIT(5);
    } else {
        reg31 &= ~(BIT(6) | BIT(5));
    }

    es8311_write_reg(ES8311_DAC_REG31, reg31);
}

void es8311_microphone_gain_set(es8311_mic_gain_t gain_db)
{
    es8311_write_reg(ES8311_ADC_REG16, gain_db);
}

void es8311_voice_fade(const es8311_fade_t fade)
{
    uint8_t reg37 = es8311_read_reg(ES8311_DAC_REG37);
    reg37 &= 0x0F;
    reg37 |= (fade << 4);
    es8311_write_reg(ES8311_DAC_REG37, reg37);
}

void es8311_microphone_fade(const es8311_fade_t fade)
{
    uint8_t reg15 = es8311_read_reg(ES8311_ADC_REG15);
    reg15 &= 0x0F;
    reg15 |= (fade << 4);
    es8311_write_reg(ES8311_ADC_REG15, reg15);
}

void es8311_register_dump()
{
    for (int reg = 0; reg < 0x4A; reg++) {
        uint8_t value = es8311_read_reg(reg);
        Serial.print("REG:");
        Serial.print(reg, HEX);
        Serial.print(": ");
        Serial.println(value, HEX);
    }
}

uint16_t es8311_read_id()
{
    uint8_t chip_id_LSB = es8311_read_reg(0xFD);
    uint8_t chip_id_MSB = es8311_read_reg(0xFE);
    uint16_t chip_id = (chip_id_MSB << 8) + chip_id_LSB;
    return chip_id;
}
