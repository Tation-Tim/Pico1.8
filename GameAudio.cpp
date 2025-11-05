/*
 * GameAudio.cpp - Modular Sound System Implementation
 */

#include "GameAudio.h"
#include "DEV_Config.h"
#include "audio_pio.h"
#include "es8311.h"
#include "hardware/pio.h"

// Audio state
static bool audio_initialized = false;
static bool audio_muted = false;
static int current_volume = 70;

// Alarm state
struct AlarmState {
  uint8_t hour;
  uint8_t minute;
  bool enabled;
  bool ringing;
  uint32_t ring_start_time;
} alarm_state = {7, 0, false, false, 0};

// Initialize the ES8311 audio codec
bool audio_init() {
  if (audio_initialized) return true;
  
  // Initialize hardware
  DEV_Module_Init();
  
  // Set audio parameters (6.144 MHz MCLK, 24kHz sample rate - the working config!)
  pico_audio.mclk_freq = 24000 * 256;  // 6.144 MHz
  pico_audio.sample_freq = 24000;
  
  // Initialize clocks
  mclk_pio_init();
  delay(50);
  set_mclk_frequency(pico_audio.mclk_freq);
  delay(100);
  
  // Initialize ES8311
  es8311_init(pico_audio);
  delay(100);
  
  // Configure sample frequency
  es8311_sample_frequency_config(pico_audio.mclk_freq, pico_audio.sample_freq);
  delay(50);
  
  // Configure microphone (required even for playback)
  es8311_microphone_config();
  delay(50);
  
  // Set volume
  es8311_voice_volume_set(current_volume);
  delay(50);
  
  // Unmute
  es8311_voice_mute(false);
  delay(50);
  
  // Set mic gain
  es8311_microphone_gain_set(ES8311_MIC_GAIN_18DB);
  delay(50);
  
  // Initialize I2S output
  dout_pio_init();
  delay(100);
  
  audio_initialized = true;
  return true;
}

// Play a single tone
void audio_play_tone(int frequency_hz, int duration_ms) {
  if (!audio_initialized || audio_muted) return;
  
  const int sample_rate = 24000;
  int total_samples = (sample_rate * duration_ms) / 1000;
  
  for (int i = 0; i < total_samples; i++) {
    float t = (float)i / sample_rate;
    int16_t sample = (int16_t)(sin(2.0f * PI * frequency_hz * t) * 16000.0f);
    
    // Stereo: left and right channels
    int32_t stereo_sample = (int32_t)sample * 65536 + sample;
    
    pio_sm_put_blocking(pico_audio.pio_2, pico_audio.sm_dout, stereo_sample);
  }
}

// Play a sound effect
void audio_play_sfx(SoundEffect sfx) {
  if (!audio_initialized || audio_muted) return;
  
  switch (sfx) {
    case SFX_BEEP:
      audio_play_tone(800, 50);
      break;
      
    case SFX_SELECT:
      audio_play_tone(1200, 50);
      delay(20);
      audio_play_tone(1500, 50);
      break;
      
    case SFX_BACK:
      audio_play_tone(1000, 50);
      delay(20);
      audio_play_tone(600, 50);
      break;
      
    case SFX_ERROR:
      audio_play_tone(200, 150);
      break;
      
    case SFX_COIN:
      audio_play_tone(1000, 50);
      delay(20);
      audio_play_tone(1500, 50);
      delay(20);
      audio_play_tone(2000, 100);
      break;
      
    case SFX_JUMP:
      for (int f = 400; f < 800; f += 50) {
        audio_play_tone(f, 10);
      }
      break;
      
    case SFX_SHOOT:
      for (int f = 1500; f > 500; f -= 100) {
        audio_play_tone(f, 15);
      }
      break;
      
    case SFX_EXPLODE:
      for (int i = 0; i < 3; i++) {
        audio_play_tone(100 + (i * 50), 50);
        delay(20);
      }
      break;
      
    case SFX_GAME_OVER:
      audio_play_tone(800, 150);
      delay(50);
      audio_play_tone(600, 150);
      delay(50);
      audio_play_tone(400, 300);
      break;
      
    case SFX_LEVEL_UP: {
      int notes[] = {523, 659, 784, 1047};  // C5, E5, G5, C6
      for (int i = 0; i < 4; i++) {
        audio_play_tone(notes[i], 100);
        delay(50);
      }
      break;
    }
      
    case SFX_ALARM:
      // Alternating high-low beeps
      for (int i = 0; i < 3; i++) {
        audio_play_tone(1200, 200);
        delay(100);
        audio_play_tone(800, 200);
        delay(100);
      }
      break;
      
    case SFX_NOTIFICATION:
      audio_play_tone(1000, 100);
      delay(50);
      audio_play_tone(1200, 100);
      break;
  }
}

// Set volume (0-100)
void audio_set_volume(int volume) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  current_volume = volume;
  
  if (audio_initialized) {
    es8311_voice_volume_set(volume);
  }
}

// Mute/unmute
void audio_mute(bool mute) {
  audio_muted = mute;
  if (audio_initialized) {
    es8311_voice_mute(mute);
  }
}

// Check if audio is ready
bool audio_is_ready() {
  return audio_initialized;
}

// ========== ALARM FUNCTIONS ==========

void alarm_set(uint8_t hour, uint8_t minute, bool enabled) {
  alarm_state.hour = hour;
  alarm_state.minute = minute;
  alarm_state.enabled = enabled;
  alarm_state.ringing = false;
}

void alarm_check_and_play() {
  if (!alarm_state.enabled || !audio_initialized) return;
  
  // Get current time from your RTC (you'll need to implement this based on your RTC)
  // For now, this is a placeholder - you need to add your actual time reading code
  // Example: uint8_t current_hour = rtc.getHour();
  //          uint8_t current_minute = rtc.getMinute();
  
  // Placeholder - replace with actual time from your RTC
  static uint8_t current_hour = 0;
  static uint8_t current_minute = 0;
  
  // Check if it's alarm time
  if (current_hour == alarm_state.hour && current_minute == alarm_state.minute) {
    if (!alarm_state.ringing) {
      alarm_state.ringing = true;
      alarm_state.ring_start_time = millis();
    }
  }
  
  // Play alarm sound if ringing
  if (alarm_state.ringing) {
    // Ring for up to 1 minute or until stopped
    if (millis() - alarm_state.ring_start_time < 60000) {
      audio_play_sfx(SFX_ALARM);
      delay(1000);  // Wait 1 second between alarm cycles
    } else {
      alarm_state.ringing = false;  // Auto-stop after 1 minute
    }
  }
}

bool alarm_is_ringing() {
  return alarm_state.ringing;
}

void alarm_stop() {
  alarm_state.ringing = false;
}
