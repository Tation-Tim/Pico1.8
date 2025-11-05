/*
 * GameAudio.h - Modular Sound System for Pico Watch Games
 * 
 * This provides simple sound effects using the ES8311 audio codec.
 * Sounds are generated procedurally to save memory.
 */

#ifndef GAME_AUDIO_H
#define GAME_AUDIO_H

#include <Arduino.h>

// Sound types
enum SoundEffect {
  SFX_BEEP,           // Generic beep
  SFX_SELECT,         // Menu selection
  SFX_BACK,           // Back/cancel
  SFX_ERROR,          // Error/invalid
  SFX_COIN,           // Collect item/score
  SFX_JUMP,           // Jump/move up
  SFX_SHOOT,          // Fire/shoot
  SFX_EXPLODE,        // Explosion/destroy
  SFX_GAME_OVER,      // Game over
  SFX_LEVEL_UP,       // Level up/win
  SFX_ALARM,          // Alarm sound
  SFX_NOTIFICATION    // Gentle notification
};

// Initialize audio system
bool audio_init();

// Play a sound effect
void audio_play_sfx(SoundEffect sfx);

// Play a tone (for custom sounds)
void audio_play_tone(int frequency_hz, int duration_ms);

// Set volume (0-100)
void audio_set_volume(int volume);

// Mute/unmute
void audio_mute(bool mute);

// Check if audio is initialized
bool audio_is_ready();

// Alarm functions
void alarm_set(uint8_t hour, uint8_t minute, bool enabled);
void alarm_check_and_play();  // Call this in your main loop
bool alarm_is_ringing();
void alarm_stop();

#endif // GAME_AUDIO_H
