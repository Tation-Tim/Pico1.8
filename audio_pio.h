/*****************************************************************************
* | File      	:   audio_pio.c
* | Author      :   Waveshare Team
* | Function    :   ES8311 control related PIO interface
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2025-02-26
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#ifndef _PICO_AUDIO_PIO_H
#define _PICO_AUDIO_PIO_H

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#define AUDIO_PIO __CONCAT(pio, PICO_AUDIO_PIO)
#define GPIO_FUNC_PIOx __CONCAT(GPIO_FUNC_PIO, PICO_AUDIO_PIO)

#define PICO_MCLK_FREQ      24000 * 256
#define PICO_SAMPLE_FREQ    24000
#define PICO_AUDIO_VOLUME   73
#define PICO_AUDIO_COUNT    1
#define PICO_AUDIO_RES_IN   16
#define PICO_AUDIO_RES_OUT  16
#define PICO_AUDIO_MIC_GAIN 3
#define PICO_AUDIO_COUNT    1
#define PICO_AUDIO_DOUT     20
#define PICO_AUDIO_DIN      21
#define PICO_AUDIO_MCLK     22
#define PICO_AUDIO_LRCLK    23
#define PICO_AUDIO_BCLK     24
#define PICO_AUDIO_PIO_1    0
#define PICO_AUDIO_PIO_2    0
#define PICO_AUDIO_SM_DOUT  0
#define PICO_AUDIO_SM_DIN   1
#define PICO_AUDIO_SM_MCLK  2

typedef struct pico_audio_struct 
{
    uint32_t mclk_freq;  
    uint32_t sample_freq;    
    uint8_t  res_in;
    uint8_t  res_out;    
    uint8_t  mic_gain;   
    uint8_t  volume;  
    uint8_t  channel_count; 
	uint8_t  audio_dout;
	uint8_t  audio_din;
	uint8_t  audio_mclk;
	uint8_t  audio_lrclk;
	uint8_t  audio_bclk;
	PIO	     pio_1;
	PIO	     pio_2;
	uint8_t  sm_dout; 
	uint8_t  sm_din; 
	uint8_t  sm_mclk; 
}pico_audio_t;

static pico_audio_t pico_audio = {
    .mclk_freq = PICO_MCLK_FREQ,        // 1st: Master clock frequency
    .sample_freq = PICO_SAMPLE_FREQ,    // 2nd: Sample frequency
    .res_in = PICO_AUDIO_RES_IN,        // 3rd: Input resolution
    .res_out = PICO_AUDIO_RES_OUT,      // 4th: Output resolution
    .mic_gain = PICO_AUDIO_MIC_GAIN,    // 5th: Mic gain
    .volume = PICO_AUDIO_VOLUME,        // 6th: Volume
    .channel_count = PICO_AUDIO_COUNT,  // 7th: Channel count
    .audio_dout = PICO_AUDIO_DOUT,      // 8th: Data output pin
    .audio_din = PICO_AUDIO_DIN,        // 9th: Data input pin
    .audio_mclk = PICO_AUDIO_MCLK,      // 10th: Master clock pin
    .audio_lrclk = PICO_AUDIO_LRCLK,    // 11th: Left/Right clock pins
    .audio_bclk = PICO_AUDIO_BCLK,      // 12th: Bit clock pin
    .pio_1 = pio1,                      // 13th: PIO1 instance
    .pio_2 = pio1,                      // 14th: PIO1 instance (not pio2!)
    .sm_dout = PICO_AUDIO_SM_DOUT,      // 15th: Output state machine
    .sm_din = PICO_AUDIO_SM_DIN,        // 16th: Input state machine
    .sm_mclk = PICO_AUDIO_SM_MCLK       // 17th: Clock state machine
};

#ifdef __cplusplus
extern "C" {
#endif

void pico_audio_write(pico_audio_t config, uint8_t* data, size_t len);

void dout_pio_init() ;
void din_pio_init();
void mclk_pio_init();
void set_mclk_frequency(uint32_t frequency);
int32_t* data_treating(const int16_t *audio , uint32_t len) ;
void audio_out(int32_t *samples, int32_t len);
//void Happy_birthday_out();
//void Sine_440hz_out();
void Loopback_test();
//void Music_out();

#ifdef __cplusplus
}
#endif

#endif //_PICO_AUDIO_PIO_H
