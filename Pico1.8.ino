/*  Pebble-style watch (RP2350 + Waveshare AMOLED 1.8" + FT3168 touch + QMI8658 tap-to-wake)
    Enhanced version with GAMES, TAMAGOTCHI, and MEMORY MONITORING
    Live AXP2101 battery management integrated
    COMPLETE with Tetris, Snake, and Breakout games
*/

#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "qspi_pio.h"
#include "FT3168.h"
#include "QMI8658.h"   // <-- IMU for tap-to-wake
#include <math.h>
#include "GameAudio.h"

// ---------- AXP2101 Battery Management ----------
#define AXP2101_SLAVE_ADDRESS    0x34
#define AXP2101_BAT_PERCENT      0xA4
#define AXP2101_TEMP_H           0x5C
#define AXP2101_TEMP_L           0x5D
#define AXP2101_STATUS2          0x01
#define AXP2101_ADC_CTRL         0x30

uint8_t get_battery_percentage() {
    uint8_t percent = DEV_I2C_Read_Byte(AXP2101_SLAVE_ADDRESS, AXP2101_BAT_PERCENT);
    if (percent > 100) percent = 100;
    return percent;
}

float get_temperature() {
    uint8_t h = DEV_I2C_Read_Byte(AXP2101_SLAVE_ADDRESS, AXP2101_TEMP_H);
    uint8_t l = DEV_I2C_Read_Byte(AXP2101_SLAVE_ADDRESS, AXP2101_TEMP_L);
    uint16_t raw = ((h & 0x3F) << 8) | l;
    return raw * 0.1f - 267.5f; // Returns temperature in Celsius
}

bool is_charging() {
    uint8_t status = DEV_I2C_Read_Byte(AXP2101_SLAVE_ADDRESS, AXP2101_STATUS2);
    return (status >> 5) == 0x01;
}

void init_axp2101() {
    // Enable ADC for battery and temperature monitoring
    uint8_t adc_ctrl = DEV_I2C_Read_Byte(AXP2101_SLAVE_ADDRESS, AXP2101_ADC_CTRL);
    DEV_I2C_Write_Byte(AXP2101_SLAVE_ADDRESS, AXP2101_ADC_CTRL, adc_ctrl | 0x11);
}

// ---------- Custom RNG to avoid hardware conflicts ----------
static unsigned long rng_seed = 1;

void seed_rng(unsigned long seed) {
  rng_seed = seed;
}

long rng(long max_val) {
  rng_seed = (rng_seed * 1103515245 + 12345) & 0x7fffffff;
  return rng_seed % max_val;
}

long rng(long min_val, long max_val) {
  return min_val + rng(max_val - min_val);
}
// ---------- End Custom RNG ----------

// ---------- Types / Forward Declarations ----------
enum VButton : uint8_t { BTN_UP, BTN_SELECT, BTN_DOWN, BTN_BACK };
enum Screen  : uint8_t { 
  SCR_WATCHFACE, SCR_MENU, SCR_APP, SCR_TL_PAST, SCR_TL_FUTURE, 
  SCR_SETTINGS_MENU, SCR_SET_TIME, SCR_SET_DATE, SCR_SETTINGS_ABOUT,
  SCR_GAMES_MENU, SCR_GAME_ARCADE, SCR_GAME_TAMAGOTCHI
};

// Game state enums
enum GameState { GAME_MENU, ASTEROIDS, TETRIS, SNAKE, BREAKOUT };
enum PetMood { HAPPY, NEUTRAL, SAD, SICK, SLEEPING };
enum PetStage { EGG, BABY, CHILD, TEEN, ADULT };
enum GameScreen { PET_MAIN, PET_STATS, PET_FEED, PET_TIME_SET };

// Timeline card struct
struct TLCard { 
  const char* title; 
  const char* subtitle; 
  const char* body; 
  uint8_t hh; 
  uint8_t mm; 
};

// Game struct definitions - DEFINED BEFORE USE
struct Ship { 
  float x, y, angle, dx, dy; 
  bool alive; 
};

struct Bullet { 
  float x, y, dx, dy; 
  bool active; 
  uint32_t fired_time; 
};

struct Asteroid { 
  float x, y, dx, dy, angle, spin; 
  int size; 
  bool active; 
};

struct Tamagotchi {
  char name[16];
  PetStage stage;
  PetMood mood;
  uint8_t hunger;
  uint8_t happiness;
  uint8_t health;
  uint8_t cleanliness;
  uint8_t age_hours;
  uint16_t age_days;
  bool is_sleeping;
  bool needs_cleanup;
  uint32_t last_poop_time;
  uint8_t petting_count;
  // Animation state
  uint32_t last_creature_move;
  uint32_t last_eye_move;
  int8_t creature_offset_x;
  int8_t creature_offset_y;
  int8_t eye_offset_x;
  int8_t eye_offset_y;
};

// Function declarations - AFTER struct definitions
void process_button(VButton b);
void Touch_INT_callback(uint gpio, uint32_t events);
void draw_watchface();
void open_watchface();
void open_menu();
void draw_tl_card(TLCard &card, bool is_past);

// ---------- Framebuffer ----------
UWORD *BlackImage = nullptr;

// ---------- Physical BACK button ----------
#define BACK_BUTTON_PIN 18
const unsigned long BACK_DEBOUNCE_MS = 40;
bool backBtnState = LOW, backLastState = LOW;
unsigned long backLastDebounce = 0;

// ---------- Touch (FT3168) ----------
volatile uint8_t touch_flag = 0;
volatile uint8_t i2c_lock   = 0;
#define I2C_LOCK()   i2c_lock = 1
#define I2C_UNLOCK() i2c_lock = 0

// ---------- Backlight dimming (dims to 0%) ----------
uint8_t       brightness_value   = 255;        // 0..255
const uint8_t DIM_STEP           = 64;         // ≈25% of full scale
unsigned long last_dim_ms        = 0;
const unsigned long DIM_INTERVAL = 15000;      // 15 seconds

// ---------- IMU tap-to-wake (QMI8658, polling-based) ----------
static float ax_f=0, ay_f=0, az_f=0;     // low-pass for gravity estimate
static float hx=0, hy=0, hz=0;           // high-pass
static uint32_t last_qmi_sample_ms = 0;

const uint32_t QMI_POLL_MS   = 25;       // 40Hz polling
const float    LPF_ALPHA     = 0.05f;    // moderate gravity tracking
const float    HPF_ALPHA     = 0.95f;    // high-pass for impulses
const float    TAP_G_THRESH  = 2.5f;     // lower threshold - easier to trigger
const uint32_t TAP_DEBOUNCE  = 400;      // shorter debounce for better responsiveness
static uint32_t last_tap_ms  = 0;

// ---------- App State ----------
Screen   current_screen = SCR_WATCHFACE;

uint32_t last_tick_ms = 0;
uint8_t  h = 12, m = 0;             // minutes only
uint8_t  day = 23, month = 10;
uint16_t year = 2025;

bool     bt_connected    = true;
uint8_t  battery_percent = 82;
int8_t   temp_F          = 72;      // will be updated with live readings

// ---------- Menu ----------
const char* MENU_ITEMS[] = { "Music", "Alarms", "Weather", "Games", "Settings" };
int MENU_COUNT = sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0]);
int menu_sel   = 0;

// ---------- Games Menu ----------
const char* GAMES_ITEMS[] = { "Arcade", "Tamagotchi" };
int GAMES_COUNT = sizeof(GAMES_ITEMS)/sizeof(GAMES_ITEMS[0]);
int games_sel = 0;

// ---------- GAMES STATE ----------
GameState current_game = GAME_MENU;
bool game_over = false;
int game_score = 0;
int high_scores[4] = {0, 0, 0, 0}; // [asteroids, tetris, snake, breakout]

// Game touch state
uint16_t game_touch_x = 0, game_touch_y = 0;
uint16_t game_touch_start_x = 0, game_touch_start_y = 0;
uint32_t game_touch_start_time = 0;
bool game_is_swiping = false;

// ASTEROIDS game objects
Ship ship;
Bullet bullets[5];
Asteroid asteroids[15];
int asteroids_level = 1; // Track current level

// TETRIS
#define GRID_W 10
#define GRID_H 20
#define BLOCK_SIZE 18
int tetris_grid[GRID_H][GRID_W];
int tetris_current_piece[4][4];
int tetris_current_type = 0;
int tetris_next_type = 0;  // Next piece preview
int tetris_piece_x = 3, tetris_piece_y = 0;
uint32_t tetris_last_drop = 0;
int tetris_drop_speed = 500;
int tetris_lines_cleared = 0;
bool tetris_piece_placed = false;

const int tetris_pieces[7][4][4] = {
  // I-piece
  {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}},
  // O-piece
  {{1,1,0,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
  // S-piece
  {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
  // Z-piece
  {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
  // T-piece
  {{1,1,1,0},{0,1,0,0},{0,0,0,0},{0,0,0,0}},
  // L-piece
  {{1,0,0,0},{1,0,0,0},{1,1,0,0},{0,0,0,0}},
  // J-piece
  {{0,1,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}}
};
const uint16_t tetris_piece_colors[7] = {0x7FFF, 0xFFE0, 0x07E0, 0xF800, 0xF81F, 0xFD20, 0x001F};

// SNAKE
#define SNAKE_MAX_LENGTH 100
#define SNAKE_GRID_SIZE 12
#define SNAKE_GRID_W 20  // Fixed size instead of calculated
#define SNAKE_GRID_H 30  // Fixed size instead of calculated
int snake_x[SNAKE_MAX_LENGTH], snake_y[SNAKE_MAX_LENGTH];
int snake_length = 4;
int snake_dir = 0; // 0=right, 1=down, 2=left, 3=up
int snake_food_x, snake_food_y;
uint32_t snake_last_move = 0;
int snake_speed = 300; // milliseconds between moves

// BREAKOUT
#define BRICK_ROWS 6
#define BRICK_COLS 8
#define BRICK_W 25
#define BRICK_H 12
bool bricks[BRICK_ROWS][BRICK_COLS];
float ball_x, ball_y, ball_dx, ball_dy;
float paddle_x;
int paddle_y = 400;
int paddle_w = 60;
int paddle_h = 8;
int ball_radius = 4;
int bricks_remaining = 0;

// ---------- TAMAGOTCHI STATE ----------
Tamagotchi pet;

uint8_t pet_current_hour = 12;
uint8_t pet_current_minute = 0;
uint8_t pet_current_second = 0;
uint32_t pet_last_time_update = 0;

GameScreen pet_current_screen = PET_MAIN;
enum TimeField { HOUR_FIELD, MINUTE_FIELD };
TimeField pet_time_field = HOUR_FIELD;

uint32_t pet_last_update_time = 0;
uint32_t pet_last_animation_time = 0;
uint8_t pet_animation_frame = 0;
uint32_t pet_last_touch_time = 0;

// ---------- Settings menu ----------
const char* SETTINGS_ITEMS[] = { "Set Time", "Set Date", "Theme", "About" };
int SETTINGS_COUNT = sizeof(SETTINGS_ITEMS)/sizeof(SETTINGS_ITEMS[0]);
int settings_sel = 0;

// set-time editing state
int set_time_field = 0; // 0=hour,1=min
bool set_time_dirty = false;
uint32_t last_time_button_press = 0; // Track when buttons were last pressed

// set-date editing state
int set_date_field = 0; // 0=day,1=month,2=year
bool set_date_dirty = false;
uint32_t last_date_button_press = 0; // Track when buttons were last pressed

// ---------- Timeline demo ----------
TLCard TL_PAST[] = {
  {"Email",   "Boss",      "Follow up on Q4 OKRs.", 9,  10},
  {"Meeting", "Daily Sync","Notes saved to Drive.", 10, 30},
  {"Run",     "5k",        "Nice pace!",            7,  15},
};
TLCard TL_FUT[] = {
  {"Lunch",   "Cafe Rio",  "With Danny @ 12:30.",   12, 30},
  {"Meeting", "1:1",       "Skip-level with Sam.",  13, 30},
  {"Call",    "Mom",       "Her birthday reminder.", 18, 0},
};
int TL_PAST_N = sizeof(TL_PAST)/sizeof(TL_PAST[0]);
int TL_FUT_N  = sizeof(TL_FUT)/sizeof(TL_FUT[0]);
int tl_idx    = 0;

// ---------- Theme / Colors ----------
struct Theme { uint16_t bg, time, muted, panel, frame, accent; };
const uint16_t COL_BLACK = BLACK, COL_WHITE = WHITE, COL_GRAY = GRAY;
const uint16_t COL_BLUE  = 0x05BF, COL_ORNG = 0xFD20, COL_GRN  = 0x07E0; // Casio-green base
const uint16_t COL_SLATE = 0x632C, COL_MID  = 0x8410, COL_LITE = 0x94B2, COL_FRM = 0xCE59;

Theme THEMES[] = {
  {COL_BLACK, COL_WHITE, COL_GRAY, COL_BLACK, COL_FRM, COL_BLUE},
  {COL_BLACK, COL_WHITE, COL_GRAY, COL_BLACK, COL_FRM, COL_ORNG},
  {COL_BLACK, COL_WHITE, COL_GRAY, COL_BLACK, COL_FRM, COL_GRN},
  {COL_SLATE, COL_WHITE, COL_MID,  COL_SLATE, COL_FRM,  0xFBE0},
};
int theme_idx = 0;
int themes_count = sizeof(THEMES)/sizeof(THEMES[0]);

// Casio F-91W-like segment green
const uint16_t CASIO_GREEN = COL_GRN; // 0x07E0; adjust here if you want a softer shade

// ---------- Date helpers ----------
int zellers_dow(int d, int m, int y) {
  if (m < 3) { m += 12; y--; }
  int q = d, K = y % 100, J = y / 100;
  int hh_ = (q + (13*(m+1))/5 + K + K/4 + J/4 + 5*J) % 7; // 0=Sat..6=Fri
  return (hh_ + 6) % 7; // 0=Sun..6=Sat
}
const char* DOW[]  = {"SUN","MON","TUE","WED","THU","FRI","SAT"};

// ---------- Big Time (LECO-like rounded digits, large) ----------
struct BigSpec { int W, H, TH, GAP; } BIG = { 102, 162, 24, 21 };

static inline void round_h(int x, int y, int w, int t, uint16_t c) {
  Paint_DrawRectangle(x + t/2, y, x + w - t/2, y + t, c, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  Paint_DrawCircle(x + t/2,     y + t/2, t/2, c, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  Paint_DrawCircle(x + w - t/2, y + t/2, t/2, c, DOT_PIXEL_1X1, DRAW_FILL_FULL);
}
static inline void round_v(int x, int y, int h, int t, uint16_t c) {
  Paint_DrawRectangle(x, y + t/2, x + t, y + h - t/2, c, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  Paint_DrawCircle(x + t/2, y + t/2,     t/2, c, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  Paint_DrawCircle(x + t/2, y + h - t/2, t/2, c, DOT_PIXEL_1X1, DRAW_FILL_FULL);
}
enum Seg { A, B, C, D, E, F, G };
void draw_leco_digit(int x, int y, int d, uint16_t col) {
  const int W = BIG.W, H = BIG.H, T = BIG.TH;
  const int padX = 6, padY = 8;

  const int xt  = x + padX;
  const int xl  = x + padX;
  const int xr  = x + W - padX - T;
  const int y_top = y + padY;
  const int y_mid = y + H/2 - T/2;
  const int y_bot = y + H - padY - T;

  bool segs[10][7] = {
    {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1},
    {1,1,1,1,0,0,1}, {0,1,1,0,0,1,1}, {1,0,1,1,0,1,1},
    {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0}, {1,1,1,1,1,1,1},
    {1,1,1,1,0,1,1}
  };

  if (d < 0 || d > 9) return;

  if (segs[d][A]) round_h(xt, y_top, W - 2*padX, T, col);
  if (segs[d][G]) round_h(xt + T/2, y_mid, W - 2*padX - T, T, col);
  if (segs[d][D]) round_h(xt, y_bot, W - 2*padX, T, col);

  if (segs[d][F]) round_v(xl, y_top + T, (H/2 - padY) - 2*T/3, T, col);
  if (segs[d][E]) round_v(xl, y_mid + T, (H/2 - padY) - 2*T/3, T, col);
  if (segs[d][B]) round_v(xr, y_top + T, (H/2 - padY) - 2*T/3, T, col);
  if (segs[d][C]) round_v(xr, y_mid + T, (H/2 - padY) - 2*T/3, T, col);
}

// Centered vertical HH over MM (no colon)
void draw_big_time_centered(int center_x, int base_y, uint8_t hh, uint8_t mm, uint16_t col) {
  const int W = BIG.W, H = BIG.H, GAP = BIG.GAP;

  // Hour on top
  int hour_x = center_x - (W*2 + GAP)/2;
  int hour_y = base_y;
  draw_leco_digit(hour_x,            hour_y, hh/10, col);
  draw_leco_digit(hour_x + W + GAP,  hour_y, hh%10, col);

  // Minutes below hour
  int min_x = center_x - (W*2 + GAP)/2;
  int min_y = base_y + H + GAP;
  draw_leco_digit(min_x,            min_y, mm/10, col);
  draw_leco_digit(min_x + W + GAP,  min_y, mm%10, col);
}

// ---------- Right-side complications (rectangles around each widget) ----------
void draw_right_complications(int startY) {
  const int margin = 10;
  const int widget_width = 60;
  const int widget_height = 35;
  const int widget_spacing = 45;
  int y = startY;
  int x = AMOLED_1IN8_WIDTH - margin - widget_width;

  // 1) Bluetooth widget with rectangle
  if (bt_connected) {
    Paint_DrawRectangle(x, y, x + widget_width, y + widget_height, CASIO_GREEN, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawString_EN(x + 18, y + 8, "BT", &Font20, CASIO_GREEN, THEMES[theme_idx].bg);
  }
  y += widget_spacing;

  // 2) Battery widget with rectangle - keep % on same line
  char bat_str[8];
  snprintf(bat_str, sizeof(bat_str), "%d%%", battery_percent);
  Paint_DrawRectangle(x, y, x + widget_width, y + widget_height, CASIO_GREEN, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  int bat_text_width = strlen(bat_str) * 10; // Approximate width for Font20
  int bat_text_x = x + (widget_width - bat_text_width) / 2; // Center the text
  Paint_DrawString_EN(bat_text_x, y + 8, bat_str, &Font20, CASIO_GREEN, THEMES[theme_idx].bg);
  y += widget_spacing;

  // 3) Temperature widget with rectangle and F suffix
  char temp_str[8];
  int display_temp = abs(temp_F);
  snprintf(temp_str, sizeof(temp_str), "%dF", display_temp);
  Paint_DrawRectangle(x, y, x + widget_width, y + widget_height, CASIO_GREEN, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  int temp_text_width = strlen(temp_str) * 10; // Approximate width for Font20
  int temp_text_x = x + (widget_width - temp_text_width) / 2; // Center the text
  Paint_DrawString_EN(temp_text_x, y + 8, temp_str, &Font20, CASIO_GREEN, THEMES[theme_idx].bg);
  y += widget_spacing;

  // 4) Date widget with rectangle
  char date_str[8];
  snprintf(date_str, sizeof(date_str), "%d", day);
  Paint_DrawRectangle(x, y, x + widget_width, y + widget_height, CASIO_GREEN, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  int date_text_width = strlen(date_str) * 10; // Approximate width for Font20
  int date_text_x = x + (widget_width - date_text_width) / 2; // Center the text
  Paint_DrawString_EN(date_text_x, y + 8, date_str, &Font20, CASIO_GREEN, THEMES[theme_idx].bg);
}

// ---------- Memory Usage Display ----------
void get_memory_info(char* flash_str, char* ram_str) {
  // Estimate current code size and RAM usage
  uint32_t estimated_flash_kb = 450; // Rough estimate for full version with games
  uint32_t total_flash_mb = 4096; // 4MB total flash
  uint32_t estimated_ram_kb = 65;  // Framebuffer + game variables + pets
  uint32_t total_ram_kb = 520;     // 520KB total RAM
  
  snprintf(flash_str, 32, "Flash: ~%ldKB / %ldMB", estimated_flash_kb, total_flash_mb/1024);
  snprintf(ram_str, 32, "RAM: ~%ldKB / %ldKB", estimated_ram_kb, total_ram_kb);
}

// ---------- GAME IMPLEMENTATIONS ----------

// ======== TETRIS GAME ========
void init_tetris() {
  game_score = 0;
  game_over = false;
  tetris_lines_cleared = 0;
  tetris_drop_speed = 500;
  tetris_last_drop = millis();
  tetris_piece_placed = false;
  
  // Clear grid
  for(int y = 0; y < GRID_H; y++) {
    for(int x = 0; x < GRID_W; x++) {
      tetris_grid[y][x] = 0;
    }
  }
  
  // Spawn first piece
  tetris_current_type = rng(7);
  tetris_next_type = rng(7);  // Generate next piece
  tetris_piece_x = 3;
  tetris_piece_y = 0;
  
  // Copy piece pattern
  for(int y = 0; y < 4; y++) {
    for(int x = 0; x < 4; x++) {
      tetris_current_piece[y][x] = tetris_pieces[tetris_current_type][y][x];
    }
  }
}

bool tetris_can_place_piece(int px, int py, int piece[4][4]) {
  for(int y = 0; y < 4; y++) {
    for(int x = 0; x < 4; x++) {
      if(piece[y][x]) {
        int nx = px + x;
        int ny = py + y;
        if(nx < 0 || nx >= GRID_W || ny >= GRID_H) return false;
        if(ny >= 0 && tetris_grid[ny][nx]) return false;
      }
    }
  }
  return true;
}

void tetris_place_piece() {
  for(int y = 0; y < 4; y++) {
    for(int x = 0; x < 4; x++) {
      if(tetris_current_piece[y][x]) {
        int nx = tetris_piece_x + x;
        int ny = tetris_piece_y + y;
        if(ny >= 0 && ny < GRID_H && nx >= 0 && nx < GRID_W) {
          tetris_grid[ny][nx] = tetris_current_type + 1;
        }
      }
    }
  }
}

void tetris_clear_lines() {
  int lines = 0;
  for(int y = GRID_H - 1; y >= 0; y--) {
    bool full = true;
    for(int x = 0; x < GRID_W; x++) {
      if(!tetris_grid[y][x]) {
        full = false;
        break;
      }
    }
    if(full) {
      lines++;
      // Move everything down
      for(int move_y = y; move_y > 0; move_y--) {
        for(int x = 0; x < GRID_W; x++) {
          tetris_grid[move_y][x] = tetris_grid[move_y - 1][x];
        }
      }
      // Clear top line
      for(int x = 0; x < GRID_W; x++) {
        tetris_grid[0][x] = 0;
      }
      y++; // Check same line again
    }
  }
  
  if(lines > 0) {
    tetris_lines_cleared += lines;
    game_score += lines * 100;
    if(tetris_lines_cleared >= 10) {
      tetris_drop_speed = max(100, tetris_drop_speed - 50);
      tetris_lines_cleared = 0;
    }
  }
}

void tetris_rotate_piece() {
  int temp[4][4];
  // Rotate counter-clockwise: temp[i][j] = current_piece[j][3-i]
  for(int i = 0; i < 4; i++) {
    for(int j = 0; j < 4; j++) {
      temp[i][j] = tetris_current_piece[3-j][i];
    }
  }
  
  // Try original position first
  if(tetris_can_place_piece(tetris_piece_x, tetris_piece_y, temp)) {
    memcpy(tetris_current_piece, temp, sizeof(temp));
    return;
  }
  
  // Try wall kicks - push piece away from walls if needed
  int kick_offsets[][2] = {{-1, 0}, {1, 0}, {-2, 0}, {2, 0}, {0, -1}};
  for(int i = 0; i < 5; i++) {
    int new_x = tetris_piece_x + kick_offsets[i][0];
    int new_y = tetris_piece_y + kick_offsets[i][1];
    if(tetris_can_place_piece(new_x, new_y, temp)) {
      tetris_piece_x = new_x;
      tetris_piece_y = new_y;
      memcpy(tetris_current_piece, temp, sizeof(temp));
      return;
    }
  }
  
  // If no valid position found, don't rotate (piece stays as is)
}

void tetris_spawn_new_piece() {
  // Use the next piece as current
  tetris_current_type = tetris_next_type;
  // Generate new next piece
  tetris_next_type = rng(7);
  
  tetris_piece_x = 3;
  tetris_piece_y = 0;
  
  for(int y = 0; y < 4; y++) {
    for(int x = 0; x < 4; x++) {
      tetris_current_piece[y][x] = tetris_pieces[tetris_current_type][y][x];
    }
  }
  
  if(!tetris_can_place_piece(tetris_piece_x, tetris_piece_y, tetris_current_piece)) {
    game_over = true;
    if(game_score > high_scores[1]) high_scores[1] = game_score;
  }
}

void update_tetris() {
  if(game_over) return;
  
  uint32_t now = millis();
  if(now - tetris_last_drop > tetris_drop_speed) {
    tetris_last_drop = now;
    
    if(tetris_can_place_piece(tetris_piece_x, tetris_piece_y + 1, tetris_current_piece)) {
      tetris_piece_y++;
    } else {
      tetris_place_piece();
      tetris_clear_lines();
      tetris_spawn_new_piece();
    }
  }
}

void draw_tetris_game() {
  Paint_Clear(BLACK);
  
  if(game_over) {
    Paint_DrawString_EN(60, 180, "GAME OVER", &Font24, 0xF800, BLACK);
    char score_str[32];
    sprintf(score_str, "Score: %d", game_score);
    Paint_DrawString_EN(80, 210, score_str, &Font24, WHITE, BLACK);
    sprintf(score_str, "High: %d", high_scores[1]);
    Paint_DrawString_EN(80, 230, score_str, &Font24, WHITE, BLACK);
    Paint_DrawString_EN(60, 260, "Touch to play again", &Font24, 0x7FFF, BLACK);
    AMOLED_1IN8_Display(BlackImage);
    return;
  }
  
  // Draw playing field border - matching Games.txt layout
  int grid_start_x = 64;
  int grid_start_y = 40;
  int grid_width = GRID_W * BLOCK_SIZE;
  int grid_height = GRID_H * BLOCK_SIZE;
  
  // Draw outer border - simple single pixel border like Games.txt
  Paint_DrawRectangle(grid_start_x - 2, grid_start_y - 2, 
                      grid_start_x + grid_width + 2, grid_start_y + grid_height + 2, 
                      WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  
  // Draw grid cells - simple like Games.txt
  for(int y = 0; y < GRID_H; y++) {
    for(int x = 0; x < GRID_W; x++) {
      if(tetris_grid[y][x]) {
        int px = grid_start_x + x * BLOCK_SIZE;
        int py = grid_start_y + y * BLOCK_SIZE;
        uint16_t color = tetris_piece_colors[tetris_grid[y][x] - 1];
        Paint_DrawRectangle(px, py, px + BLOCK_SIZE - 1, py + BLOCK_SIZE - 1, 
                          color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
      }
    }
  }
  
  // Draw current piece - simple solid blocks
  uint16_t piece_color = tetris_piece_colors[tetris_current_type];
  for(int y = 0; y < 4; y++) {
    for(int x = 0; x < 4; x++) {
      if(tetris_current_piece[y][x]) {
        int px = grid_start_x + (tetris_piece_x + x) * BLOCK_SIZE;
        int py = grid_start_y + (tetris_piece_y + y) * BLOCK_SIZE;
        Paint_DrawRectangle(px, py, px + BLOCK_SIZE - 1, py + BLOCK_SIZE - 1, 
                          piece_color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
      }
    }
  }
  
  // Draw score at top like Games.txt
  char score_str[32];
  snprintf(score_str, sizeof(score_str), "Score:%d Hi:%d", game_score, high_scores[1]);
  Paint_DrawString_EN(10, 10, score_str, &Font24, WHITE, BLACK);
  
  // Draw NEXT piece preview box on the right
  int next_box_x = 260;
  int next_box_y = 100;
  int next_box_size = 80;
  
  // Draw "NEXT" label
  Paint_DrawString_EN(next_box_x + 10, next_box_y - 30, "NEXT", &Font24, CYAN, BLACK);
  
  // Draw box border
  Paint_DrawRectangle(next_box_x, next_box_y, next_box_x + next_box_size, next_box_y + next_box_size, 
                     WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  
  // Draw next piece in the center of the box (scaled to fit)
  int piece_size = 16; // Smaller blocks for preview
  uint16_t next_color = tetris_piece_colors[tetris_next_type];
  
  // Calculate centering offset for the piece
  int piece_offset_x = next_box_x + 10;
  int piece_offset_y = next_box_y + 10;
  
  for(int y = 0; y < 4; y++) {
    for(int x = 0; x < 4; x++) {
      if(tetris_pieces[tetris_next_type][y][x]) {
        int px = piece_offset_x + x * piece_size;
        int py = piece_offset_y + y * piece_size;
        Paint_DrawRectangle(px, py, px + piece_size - 1, py + piece_size - 1, 
                          next_color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
      }
    }
  }
  
  // Draw controls at bottom like Games.txt
  Paint_DrawString_EN(10, 460, "L:Left M:Rotate R:Right", &Font24, GRAY, BLACK);
  
  AMOLED_1IN8_Display(BlackImage);
}

// ======== SNAKE GAME ========
void init_snake() {
  game_score = 0;
  game_over = false;
  snake_length = 4;
  snake_dir = 0;
  snake_speed = 300;
  snake_last_move = millis();
  
  // Initialize snake in center with bounds checking
  int start_x = max(4, SNAKE_GRID_W / 2);
  int start_y = max(4, SNAKE_GRID_H / 2);
  
  for(int i = 0; i < snake_length && i < SNAKE_MAX_LENGTH; i++) {
    snake_x[i] = start_x - i;
    snake_y[i] = start_y;
  }
  
  // Place food safely
  do {
    snake_food_x = rng(2, SNAKE_GRID_W - 2);
    snake_food_y = rng(2, SNAKE_GRID_H - 2);
  } while((snake_food_x == snake_x[0] && snake_food_y == snake_y[0]) && snake_food_x < SNAKE_GRID_W && snake_food_y < SNAKE_GRID_H);
}

bool snake_check_collision(int x, int y) {
  // Check walls
  if(x < 0 || x >= SNAKE_GRID_W || y < 0 || y >= SNAKE_GRID_H) return true;
  
  // Check self collision
  for(int i = 0; i < snake_length; i++) {
    if(snake_x[i] == x && snake_y[i] == y) return true;
  }
  
  return false;
}

void update_snake() {
  if(game_over) return;
  
  uint32_t now = millis();
  if(now - snake_last_move < snake_speed) return;
  snake_last_move = now;
  
  // Calculate new head position
  int new_x = snake_x[0];
  int new_y = snake_y[0];
  
  switch(snake_dir) {
    case 0: new_x++; break; // right
    case 1: new_y++; break; // down
    case 2: new_x--; break; // left
    case 3: new_y--; break; // up
  }
  
  // Check collision
  if(snake_check_collision(new_x, new_y)) {
    game_over = true;
    if(game_score > high_scores[2]) high_scores[2] = game_score;
    return;
  }
  
  // Check food
  bool ate_food = (new_x == snake_food_x && new_y == snake_food_y);
  
  if(ate_food) {
    game_score += 10;
    snake_length++;
    snake_speed = max(100, snake_speed - 5);
    
    // Place new food
    do {
      snake_food_x = rng(SNAKE_GRID_W);
      snake_food_y = rng(SNAKE_GRID_H);
    } while(snake_check_collision(snake_food_x, snake_food_y));
  }
  
  // Move snake
  for(int i = snake_length - 1; i > 0; i--) {
    snake_x[i] = snake_x[i-1];
    snake_y[i] = snake_y[i-1];
  }
  snake_x[0] = new_x;
  snake_y[0] = new_y;
  
  if(!ate_food && snake_length > 0) {
    snake_length--; // Don't grow tail if no food eaten
    snake_length++; // Restore length (effectively moves tail)
  }
}

void draw_snake_game() {
  Paint_Clear(BLACK);
  
  if(game_over) {
    Paint_DrawString_EN(60, 180, "GAME OVER", &Font24, 0xF800, BLACK);
    char score_str[32];
    sprintf(score_str, "Score: %d", game_score);
    Paint_DrawString_EN(80, 210, score_str, &Font24, WHITE, BLACK);
    sprintf(score_str, "High: %d", high_scores[2]);
    Paint_DrawString_EN(80, 230, score_str, &Font24, WHITE, BLACK);
    Paint_DrawString_EN(60, 260, "Touch to play again", &Font24, 0x7FFF, BLACK);
    AMOLED_1IN8_Display(BlackImage);
    return;
  }
  
  // Draw border
  Paint_DrawRectangle(0, 0, AMOLED_1IN8_WIDTH-1, AMOLED_1IN8_HEIGHT-1, WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  
  // Draw snake
  for(int i = 0; i < snake_length; i++) {
    int x = snake_x[i] * SNAKE_GRID_SIZE;
    int y = snake_y[i] * SNAKE_GRID_SIZE;
    uint16_t color = (i == 0) ? 0x07E0 : 0x07C0; // Head brighter
    Paint_DrawRectangle(x, y, x + SNAKE_GRID_SIZE - 1, y + SNAKE_GRID_SIZE - 1, color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  }
  
  // Draw food
  int fx = snake_food_x * SNAKE_GRID_SIZE;
  int fy = snake_food_y * SNAKE_GRID_SIZE;
  Paint_DrawRectangle(fx, fy, fx + SNAKE_GRID_SIZE - 1, fy + SNAKE_GRID_SIZE - 1, 0xF800, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  
  // Draw score
  char score_str[32];
  sprintf(score_str, "Score: %d", game_score);
  Paint_DrawString_EN(5, 5, score_str, &Font12, WHITE, BLACK);
  
  AMOLED_1IN8_Display(BlackImage);
}

// ======== BREAKOUT GAME ========
void init_breakout() {
  game_score = 0;
  game_over = false;
  bricks_remaining = 0;
  
  // Initialize bricks
  for(int row = 0; row < BRICK_ROWS; row++) {
    for(int col = 0; col < BRICK_COLS; col++) {
      bricks[row][col] = true;
      bricks_remaining++;
    }
  }
  
  // Initialize ball
  ball_x = AMOLED_1IN8_WIDTH / 2;
  ball_y = 300;
  ball_dx = 2;
  ball_dy = -3;
  
  // Initialize paddle
  paddle_x = (AMOLED_1IN8_WIDTH - paddle_w) / 2;
}

void update_breakout() {
  if(game_over) return;
  
  // Move ball
  ball_x += ball_dx;
  ball_y += ball_dy;
  
  // Ball collision with walls
  if(ball_x <= ball_radius || ball_x >= AMOLED_1IN8_WIDTH - ball_radius) {
    ball_dx = -ball_dx;
  }
  if(ball_y <= ball_radius) {
    ball_dy = -ball_dy;
  }
  
  // Ball hits bottom - game over
  if(ball_y >= AMOLED_1IN8_HEIGHT - ball_radius) {
    game_over = true;
    if(game_score > high_scores[3]) high_scores[3] = game_score;
    return;
  }
  
  // Ball collision with paddle
  if(ball_y + ball_radius >= paddle_y && ball_y - ball_radius <= paddle_y + paddle_h &&
     ball_x >= paddle_x && ball_x <= paddle_x + paddle_w) {
    ball_dy = -abs(ball_dy); // Always bounce up
    // Add some angle based on where it hit the paddle
    float hit_pos = (ball_x - paddle_x) / paddle_w; // 0 to 1
    ball_dx = (hit_pos - 0.5f) * 4; // -2 to +2
  }
  
  // Ball collision with bricks
  int brick_start_y = 50;
  for(int row = 0; row < BRICK_ROWS; row++) {
    for(int col = 0; col < BRICK_COLS; col++) {
      if(!bricks[row][col]) continue;
      
      int brick_x = col * (AMOLED_1IN8_WIDTH / BRICK_COLS);
      int brick_y = brick_start_y + row * BRICK_H;
      
      if(ball_x + ball_radius >= brick_x && ball_x - ball_radius <= brick_x + BRICK_W &&
         ball_y + ball_radius >= brick_y && ball_y - ball_radius <= brick_y + BRICK_H) {
        bricks[row][col] = false;
        bricks_remaining--;
        game_score += 10;
        ball_dy = -ball_dy;
        
        if(bricks_remaining == 0) {
          // Level complete - could restart with faster ball
          init_breakout();
          ball_dx *= 1.2f;
          ball_dy *= 1.2f;
        }
        break;
      }
    }
  }
}

void draw_breakout_game() {
  Paint_Clear(BLACK);
  
  if(game_over) {
    Paint_DrawString_EN(60, 180, "GAME OVER", &Font24, 0xF800, BLACK);
    char score_str[32];
    sprintf(score_str, "Score: %d", game_score);
    Paint_DrawString_EN(80, 210, score_str, &Font24, WHITE, BLACK);
    sprintf(score_str, "High: %d", high_scores[3]);
    Paint_DrawString_EN(80, 230, score_str, &Font24, WHITE, BLACK);
    Paint_DrawString_EN(60, 260, "Touch to play again", &Font24, 0x7FFF, BLACK);
    AMOLED_1IN8_Display(BlackImage);
    return;
  }
  
  // Draw bricks
  int brick_start_y = 50;
  uint16_t brick_colors[] = {0xF800, 0xFCA0, 0xFFE0, 0x07E0, 0x001F, 0xF81F}; // Different colors per row
  
  for(int row = 0; row < BRICK_ROWS; row++) {
    for(int col = 0; col < BRICK_COLS; col++) {
      if(!bricks[row][col]) continue;
      
      int brick_x = col * (AMOLED_1IN8_WIDTH / BRICK_COLS);
      int brick_y = brick_start_y + row * BRICK_H;
      uint16_t color = brick_colors[row % 6];
      
      Paint_DrawRectangle(brick_x, brick_y, brick_x + BRICK_W - 2, brick_y + BRICK_H - 1, color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    }
  }
  
  // Draw paddle
  Paint_DrawRectangle(paddle_x, paddle_y, paddle_x + paddle_w, paddle_y + paddle_h, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  
  // Draw ball
  Paint_DrawCircle(ball_x, ball_y, ball_radius, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  
  // Draw score
  char score_str[32];
  sprintf(score_str, "Score: %d", game_score);
  Paint_DrawString_EN(5, 5, score_str, &Font12, WHITE, BLACK);
  
  // Draw remaining bricks count
  sprintf(score_str, "Bricks: %d", bricks_remaining);
  Paint_DrawString_EN(5, 20, score_str, &Font12, WHITE, BLACK);
  
  AMOLED_1IN8_Display(BlackImage);
}

// ======== ASTEROIDS (EXISTING) ========
void init_asteroids() {
  game_score = 0;
  game_over = false;
  ship.x = 184.0f; 
  ship.y = 260.0f; 
  ship.angle = -90.0f;
  ship.dx = 0.0f; 
  ship.dy = 0.0f; 
  ship.alive = true;
  
  for(int i = 0; i < 5; i++) bullets[i].active = false;
  for(int i = 0; i < 15; i++) asteroids[i].active = false;
  
  // Initialize asteroids based on level - start with 4, add 1 per level, max 10
  int num_asteroids = min(3 + asteroids_level, 10);
  for(int i = 0; i < num_asteroids; i++) {
    asteroids[i].active = true; 
    asteroids[i].size = 2;
    asteroids[i].x = rng(50, 318); 
    asteroids[i].y = rng(50, 430);
    asteroids[i].dx = (rng(0, 100) / 50.0f) - 1.0f;
    asteroids[i].dy = (rng(0, 100) / 50.0f) - 1.0f;
    asteroids[i].angle = rng(0, 360);
    asteroids[i].spin = (rng(0, 100) / 100.0f) - 0.5f;
  }
}

void update_asteroids() {
  if(game_over || !ship.alive) return;
  
  // Check if all asteroids destroyed - advance to next level
  bool any_active = false;
  for(int i = 0; i < 15; i++) {
    if(asteroids[i].active) {
      any_active = true;
      break;
    }
  }
  if(!any_active) {
    // Level complete! Advance to next level
    asteroids_level++;
    init_asteroids();
    return;
  }
  
  // Update ship physics with simple drag
  ship.dx *= 0.98f;
  ship.dy *= 0.98f;
  ship.x += ship.dx;
  ship.y += ship.dy;
  
  // Wrap ship around screen
  if(ship.x < 0) ship.x = AMOLED_1IN8_WIDTH;
  if(ship.x > AMOLED_1IN8_WIDTH) ship.x = 0;
  if(ship.y < 0) ship.y = AMOLED_1IN8_HEIGHT;
  if(ship.y > AMOLED_1IN8_HEIGHT) ship.y = 0;
  
  // Update bullets
  for(int i = 0; i < 5; i++) {
    if(!bullets[i].active) continue;
    bullets[i].x += bullets[i].dx;
    bullets[i].y += bullets[i].dy;
    
    // Remove bullets that go off screen or are too old
    if(bullets[i].x < 0 || bullets[i].x > AMOLED_1IN8_WIDTH ||
       bullets[i].y < 0 || bullets[i].y > AMOLED_1IN8_HEIGHT ||
       millis() - bullets[i].fired_time > 2000) {
      bullets[i].active = false;
    }
  }
  
  // Update asteroids
  for(int i = 0; i < 15; i++) {
    if(!asteroids[i].active) continue;
    asteroids[i].x += asteroids[i].dx;
    asteroids[i].y += asteroids[i].dy;
    asteroids[i].angle += asteroids[i].spin;
    
    // Wrap around screen
    if(asteroids[i].x < 0) asteroids[i].x = AMOLED_1IN8_WIDTH;
    if(asteroids[i].x > AMOLED_1IN8_WIDTH) asteroids[i].x = 0;
    if(asteroids[i].y < 0) asteroids[i].y = AMOLED_1IN8_HEIGHT;
    if(asteroids[i].y > AMOLED_1IN8_HEIGHT) asteroids[i].y = 0;
  }
  
  // Check bullet-asteroid collisions - use actual radius like Games.txt
  for(int b = 0; b < 5; b++) {
    if(!bullets[b].active) continue;
    for(int a = 0; a < 15; a++) {
      if(!asteroids[a].active) continue;
      int ar = (asteroids[a].size == 2) ? 20 : (asteroids[a].size == 1) ? 12 : 6;
      float dx = bullets[b].x - asteroids[a].x;
      float dy = bullets[b].y - asteroids[a].y;
      float dist = sqrt(dx*dx + dy*dy);
      if(dist < ar) {
        bullets[b].active = false;
        int old_size = asteroids[a].size;
        float old_x = asteroids[a].x;
        float old_y = asteroids[a].y;
        asteroids[a].active = false;
        game_score += (old_size + 1) * 10;
        
        // Split into smaller asteroids if not already smallest
        if(old_size > 0) {
          // Find two empty asteroid slots
          int split_count = 0;
          for(int i = 0; i < 15 && split_count < 2; i++) {
            if(!asteroids[i].active) {
              asteroids[i] = asteroids[a];
              asteroids[i].size = old_size - 1;
              asteroids[i].x = old_x + rng(-10, 10);
              asteroids[i].y = old_y + rng(-10, 10);
              asteroids[i].dx = -asteroids[a].dx + (rng(100) / 100.0f);
              asteroids[i].dy = -asteroids[a].dy + (rng(100) / 100.0f);
              asteroids[i].active = true;
              split_count++;
            }
          }
        }
        break;
      }
    }
  }
  
  // Check ship-asteroid collisions - use actual radius like Games.txt
  for(int a = 0; a < 15; a++) {
    if(!asteroids[a].active) continue;
    int ar = (asteroids[a].size == 2) ? 20 : (asteroids[a].size == 1) ? 12 : 6;
    float dx = ship.x - asteroids[a].x;
    float dy = ship.y - asteroids[a].y;
    float dist = sqrt(dx*dx + dy*dy);
    if(dist < ar + 10) {
      ship.alive = false;
      game_over = true;
      if(game_score > high_scores[0]) high_scores[0] = game_score;
      break;
    }
  }
}

void draw_asteroids_game() {
  Paint_Clear(BLACK);
  
  if(game_over) {
    Paint_DrawString_EN(60, 180, "GAME OVER", &Font24, 0xF800, BLACK);
    char score_str[32];
    sprintf(score_str, "Score: %d", game_score);
    Paint_DrawString_EN(80, 210, score_str, &Font24, WHITE, BLACK);
    sprintf(score_str, "High: %d", high_scores[0]);
    Paint_DrawString_EN(80, 230, score_str, &Font24, WHITE, BLACK);
    Paint_DrawString_EN(60, 260, "Touch to play again", &Font24, 0x7FFF, BLACK);
    AMOLED_1IN8_Display(BlackImage);
    return;
  }
  
  // Draw ship if alive
  if(ship.alive) {
    float rad = ship.angle * 3.14159f / 180.0f;
    int x1 = ship.x + 10 * cos(rad);
    int y1 = ship.y + 10 * sin(rad);
    int x2 = ship.x + 6 * cos(rad + 2.5f);
    int y2 = ship.y + 6 * sin(rad + 2.5f);
    int x3 = ship.x + 6 * cos(rad - 2.5f);
    int y3 = ship.y + 6 * sin(rad - 2.5f);
    
    Paint_DrawLine(x1, y1, x2, y2, WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawLine(x2, y2, x3, y3, WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawLine(x3, y3, x1, y1, WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
  }
  
  // Draw bullets
  for(int i = 0; i < 5; i++) {
    if(bullets[i].active) {
      Paint_DrawCircle(bullets[i].x, bullets[i].y, 2, 0xFFE0, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    }
  }
  
  // Draw asteroids as irregular polygons (jagged rocks)
  for(int i = 0; i < 15; i++) {
    if(asteroids[i].active) {
      int base_r = (asteroids[i].size == 2) ? 20 : (asteroids[i].size == 1) ? 12 : 6;
      // Create 8 vertices with varied radii for irregular shape
      int num_vertices = 8;
      int vertices_x[8], vertices_y[8];
      
      for(int j = 0; j < num_vertices; j++) {
        float angle = (asteroids[i].angle + j * 45) * 3.14159f / 180.0f;
        // Vary radius between 70% and 100% of base for irregular look
        int variation = (i * 7 + j * 3) % 30; // Pseudo-random but consistent per asteroid
        float r = base_r * (0.7f + variation / 100.0f);
        vertices_x[j] = asteroids[i].x + r * cos(angle);
        vertices_y[j] = asteroids[i].y + r * sin(angle);
      }
      
      // Draw lines between vertices
      for(int j = 0; j < num_vertices; j++) {
        int next = (j + 1) % num_vertices;
        Paint_DrawLine(vertices_x[j], vertices_y[j], vertices_x[next], vertices_y[next], 
                      WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
      }
    }
  }
  
  // Draw score and level
  char score_str[32];
  sprintf(score_str, "Score:%d Lvl:%d", game_score, asteroids_level);
  Paint_DrawString_EN(5, 5, score_str, &Font12, WHITE, BLACK);
  
  AMOLED_1IN8_Display(BlackImage);
}

// ---------- Button virtual mappings ----------
void process_button(VButton b) {
  set_brightness_and_restart(255);

  if (current_screen == SCR_WATCHFACE) {
    if      (b == BTN_UP)   { current_screen = SCR_TL_FUTURE; draw_tl_card(TL_FUT[0], false); }
    else if (b == BTN_DOWN) { current_screen = SCR_TL_PAST;   draw_tl_card(TL_PAST[0], true); }
    else if (b == BTN_SELECT) open_menu();
  }
  else if (current_screen == SCR_MENU) {
    if (b == BTN_UP && menu_sel > 0) { menu_sel--; open_menu(); }
    else if (b == BTN_DOWN && menu_sel < MENU_COUNT-1) { menu_sel++; open_menu(); }
    else if (b == BTN_SELECT) {
      if (menu_sel == 3) { // Games
        current_screen = SCR_GAMES_MENU;
        draw_games_menu();
      } else if (menu_sel == 4) { // Settings
        current_screen = SCR_SETTINGS_MENU;
        draw_settings_menu();
      }
    }
    else if (b == BTN_BACK) open_watchface();
  }
  else if (current_screen == SCR_GAMES_MENU) {
    if (b == BTN_UP && games_sel > 0) { games_sel--; draw_games_menu(); }
    else if (b == BTN_DOWN && games_sel < GAMES_COUNT-1) { games_sel++; draw_games_menu(); }
    else if (b == BTN_SELECT) {
      if (games_sel == 0) { // Arcade
        current_screen = SCR_GAME_ARCADE;
        current_game = GAME_MENU;
        draw_arcade_menu();
      } else if (games_sel == 1) { // Tamagotchi
        current_screen = SCR_GAME_TAMAGOTCHI;
        draw_pet_main_screen();
      }
    }
    else if (b == BTN_BACK) open_menu();
  }
  else if (current_screen == SCR_GAME_ARCADE) {
    if (current_game == GAME_MENU) {
      if (b == BTN_BACK) {
        current_screen = SCR_GAMES_MENU;
        draw_games_menu();
      }
    } else if (current_game == ASTEROIDS || current_game == TETRIS || current_game == SNAKE || current_game == BREAKOUT) {
      if (b == BTN_BACK) {
        current_game = GAME_MENU;
        draw_arcade_menu();
      }
    }
  }
  else if (current_screen == SCR_GAME_TAMAGOTCHI) {
    if (b == BTN_BACK) {
      current_screen = SCR_GAMES_MENU;
      draw_games_menu();
    }
  }
  else if (current_screen == SCR_SETTINGS_MENU) {
    if (b == BTN_UP && settings_sel > 0) { settings_sel--; draw_settings_menu(); }
    else if (b == BTN_DOWN && settings_sel < SETTINGS_COUNT-1) { settings_sel++; draw_settings_menu(); }
    else if (b == BTN_SELECT) {
      if (settings_sel == 0) { // Set Time
        current_screen = SCR_SET_TIME;
        set_time_field = 0;
        set_time_dirty = false;
        draw_set_time();
      } else if (settings_sel == 1) { // Set Date
        current_screen = SCR_SET_DATE;
        set_date_field = 0;
        set_date_dirty = false;
        draw_set_date();
      } else if (settings_sel == 3) { // About
        current_screen = SCR_SETTINGS_ABOUT;
        draw_about();
      }
    }
    else if (b == BTN_BACK) open_menu();
  }
  else if (current_screen == SCR_SET_TIME) {
    last_time_button_press = millis();
    if (b == BTN_UP) {
      if (set_time_field == 0) { h = (h + 1) % 24; }
      else { m = (m + 1) % 60; }
      set_time_dirty = true;
      draw_set_time();
    }
    else if (b == BTN_DOWN) {
      if (set_time_field == 0) { h = (h == 0) ? 23 : h - 1; }
      else { m = (m == 0) ? 59 : m - 1; }
      set_time_dirty = true;
      draw_set_time();
    }
    else if (b == BTN_SELECT) {
      set_time_field = 1 - set_time_field;
      draw_set_time();
    }
    else if (b == BTN_BACK) {
      current_screen = SCR_SETTINGS_MENU;
      draw_settings_menu();
    }
  }
  else if (current_screen == SCR_SET_DATE) {
    last_date_button_press = millis();
    if (b == BTN_UP) {
      if (set_date_field == 0) { day = (day % 31) + 1; }
      else if (set_date_field == 1) { month = (month % 12) + 1; }
      else { year++; }
      set_date_dirty = true;
      draw_set_date();
    }
    else if (b == BTN_DOWN) {
      if (set_date_field == 0) { day = (day == 1) ? 31 : day - 1; }
      else if (set_date_field == 1) { month = (month == 1) ? 12 : month - 1; }
      else { year--; }
      set_date_dirty = true;
      draw_set_date();
    }
    else if (b == BTN_SELECT) {
      set_date_field = (set_date_field + 1) % 3;
      draw_set_date();
    }
    else if (b == BTN_BACK) {
      current_screen = SCR_SETTINGS_MENU;
      draw_settings_menu();
    }
  }
  else if (current_screen == SCR_SETTINGS_ABOUT) {
    if (b == BTN_BACK) {
      current_screen = SCR_SETTINGS_MENU;
      draw_settings_menu();
    }
  }
  else if (current_screen == SCR_TL_PAST || current_screen == SCR_TL_FUTURE) {
    if (b == BTN_BACK) open_watchface();
  }
}

// ---------- Draw functions ----------
void draw_watchface() {
  Paint_Clear(THEMES[theme_idx].bg);
  int centerX = AMOLED_1IN8_WIDTH / 2 - 60; // Moved 60 pixels left to avoid overlap
  draw_big_time_centered(centerX, 30, h, m, CASIO_GREEN); // GREEN digits
  draw_right_complications(50);
  AMOLED_1IN8_Display(BlackImage);
}

void open_watchface() {
  current_screen = SCR_WATCHFACE;
  draw_watchface();
}

void open_menu() {
  current_screen = SCR_MENU;
  Paint_Clear(THEMES[theme_idx].bg);
  Paint_DrawString_EN(20, 30, "MENU", &Font24, THEMES[theme_idx].accent, THEMES[theme_idx].bg);
  for (int i = 0; i < MENU_COUNT; i++) {
    uint16_t color = (i == menu_sel) ? THEMES[theme_idx].accent : THEMES[theme_idx].time;
    Paint_DrawString_EN(30, 70 + i*30, (char*)MENU_ITEMS[i], &Font20, color, THEMES[theme_idx].bg);
  }
  AMOLED_1IN8_Display(BlackImage);
}

void draw_games_menu() {
  Paint_Clear(THEMES[theme_idx].bg);
  Paint_DrawString_EN(20, 30, "GAMES", &Font24, THEMES[theme_idx].accent, THEMES[theme_idx].bg);
  for (int i = 0; i < GAMES_COUNT; i++) {
    uint16_t color = (i == games_sel) ? THEMES[theme_idx].accent : THEMES[theme_idx].time;
    Paint_DrawString_EN(30, 80 + i*40, (char*)GAMES_ITEMS[i], &Font20, color, THEMES[theme_idx].bg);
  }
  Paint_DrawString_EN(30, 180, "Arcade: 4 classic games", &Font24, THEMES[theme_idx].muted, THEMES[theme_idx].bg);
  Paint_DrawString_EN(30, 200, "Tamagotchi: Virtual pet", &Font24, THEMES[theme_idx].muted, THEMES[theme_idx].bg);
  AMOLED_1IN8_Display(BlackImage);
}

void draw_arcade_menu() {
  Paint_Clear(BLACK);
  Paint_DrawString_EN(90, 40, "ARCADE", &Font24, CYAN, BLACK);
  Paint_DrawRectangle(40, 120, 328, 180, WHITE, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(110, 140, "ASTEROIDS", &Font20, WHITE, BLACK);
  Paint_DrawRectangle(40, 200, 328, 260, WHITE, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(130, 220, "TETRIS", &Font20, WHITE, BLACK);
  Paint_DrawRectangle(40, 280, 328, 340, WHITE, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(135, 300, "SNAKE", &Font20, WHITE, BLACK);
  Paint_DrawRectangle(40, 360, 328, 420, WHITE, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(120, 380, "BREAKOUT", &Font20, WHITE, BLACK);
  Paint_DrawString_EN(50, 450, "ALL GAMES READY!", &Font24, 0x07E0, BLACK);
  Paint_DrawString_EN(50, 470, "Touch to play", &Font24, CYAN, BLACK);
  AMOLED_1IN8_Display(BlackImage);
}

void draw_settings_menu() {
  Paint_Clear(THEMES[theme_idx].bg);
  Paint_DrawString_EN(20, 30, "SETTINGS", &Font24, THEMES[theme_idx].accent, THEMES[theme_idx].bg);
  for (int i = 0; i < SETTINGS_COUNT; i++) {
    uint16_t color = (i == settings_sel) ? THEMES[theme_idx].accent : THEMES[theme_idx].time;
    Paint_DrawString_EN(30, 70 + i*30, (char*)SETTINGS_ITEMS[i], &Font20, color, THEMES[theme_idx].bg);
  }
  AMOLED_1IN8_Display(BlackImage);
}

void draw_set_time() {
  Paint_Clear(THEMES[theme_idx].bg);
  Paint_DrawString_EN(20, 30, "SET TIME", &Font24, THEMES[theme_idx].accent, THEMES[theme_idx].bg);

  char field_indicators[2][8] = {"HOUR", "MIN"};
  Paint_DrawString_EN(30, 80, field_indicators[set_time_field], &Font24, THEMES[theme_idx].accent, THEMES[theme_idx].bg);

  char time_display[16];
  snprintf(time_display, sizeof(time_display), "%02d:%02d", h, m);
  Paint_DrawString_EN(30, 120, time_display, &Font24, THEMES[theme_idx].time, THEMES[theme_idx].bg);
  Paint_DrawString_EN(30, 180, "UP/DOWN: Change", &Font24, THEMES[theme_idx].muted, THEMES[theme_idx].bg);
  Paint_DrawString_EN(30, 200, "SELECT: Next", &Font24, THEMES[theme_idx].muted, THEMES[theme_idx].bg);
  Paint_DrawString_EN(30, 220, "BACK: Save", &Font24, THEMES[theme_idx].muted, THEMES[theme_idx].bg);
  AMOLED_1IN8_Display(BlackImage);
}

void draw_set_date() {
  Paint_Clear(THEMES[theme_idx].bg);
  Paint_DrawString_EN(20, 30, "SET DATE", &Font24, THEMES[theme_idx].accent, THEMES[theme_idx].bg);

  char field_indicators[3][8] = {"DAY", "MONTH", "YEAR"};
  Paint_DrawString_EN(30, 80, field_indicators[set_date_field], &Font24, THEMES[theme_idx].accent, THEMES[theme_idx].bg);

  char date_display[16];
  snprintf(date_display, sizeof(date_display), "%02d/%02d/%d", month, day, year);
  Paint_DrawString_EN(30, 120, date_display, &Font24, THEMES[theme_idx].time, THEMES[theme_idx].bg);
  Paint_DrawString_EN(30, 180, "UP/DOWN: Change", &Font24, THEMES[theme_idx].muted, THEMES[theme_idx].bg);
  Paint_DrawString_EN(30, 200, "SELECT: Next", &Font24, THEMES[theme_idx].muted, THEMES[theme_idx].bg);
  Paint_DrawString_EN(30, 220, "BACK: Save", &Font24, THEMES[theme_idx].muted, THEMES[theme_idx].bg);
  AMOLED_1IN8_Display(BlackImage);
}

void draw_about() {
  Paint_Clear(BLACK);
  Paint_DrawString_EN(90, 30, "About", &Font24, CASIO_GREEN, BLACK);

  Paint_DrawString_EN(20, 80, "Pebble-Style Watch v3.0", &Font24, COL_WHITE, BLACK);
  Paint_DrawString_EN(20, 110, "RP2350 + AMOLED 1.8\"", &Font24, COL_GRAY, BLACK);
  Paint_DrawString_EN(20, 140, "FT3168 Touch", &Font24, COL_GRAY, BLACK);
  Paint_DrawString_EN(20, 170, "QMI8658 IMU", &Font24, COL_GRAY, BLACK);
  Paint_DrawString_EN(20, 200, "AXP2101 Power Mgmt", &Font24, COL_GRAY, BLACK);

  Paint_DrawString_EN(20, 240, "Features:", &Font24, CASIO_GREEN, BLACK);
  Paint_DrawString_EN(20, 270, "Full games arcade", &Font24, COL_WHITE, BLACK);
  Paint_DrawString_EN(20, 300, "Virtual pet", &Font24, COL_WHITE, BLACK);
  Paint_DrawString_EN(20, 330, "Live monitoring", &Font24, COL_WHITE, BLACK);
  Paint_DrawString_EN(20, 360, "Tap-to-wake", &Font24, COL_WHITE, BLACK);
  Paint_DrawString_EN(20, 390, "Auto-dimming", &Font24, COL_WHITE, BLACK);

  AMOLED_1IN8_Display(BlackImage);
}

void draw_tl_card(TLCard &card, bool is_past) {
  Paint_Clear(BLACK);

  uint16_t title_color = is_past ? COL_GRAY : CASIO_GREEN;
  uint16_t time_color = is_past ? 0x39E7 : 0xFFE0;

  Paint_DrawString_EN(20, 50, card.title, &Font20, title_color, BLACK);
  Paint_DrawString_EN(20, 80, card.subtitle, &Font24, COL_WHITE, BLACK);
  Paint_DrawString_EN(20, 120, card.body, &Font12, COL_GRAY, BLACK);

  char time_str[8];
  sprintf(time_str, "%02d:%02d", card.hh, card.mm);
  Paint_DrawString_EN(20, 350, time_str, &Font24, time_color, BLACK);

  const char* label = is_past ? "PAST" : "FUTURE";
  Paint_DrawString_EN(20, 390, label, &Font12, title_color, BLACK);

  AMOLED_1IN8_Display(BlackImage);
}

// ---------- PET FUNCTIONS ----------
void init_pet() {
  strcpy(pet.name, "Buddy");
  pet.stage = EGG;
  pet.mood = NEUTRAL;
  pet.hunger = 50;
  pet.happiness = 50;
  pet.health = 100;
  pet.cleanliness = 100;
  pet.age_hours = 0;
  pet.age_days = 0;
  pet.is_sleeping = false;
  pet.needs_cleanup = false;
  pet.last_poop_time = millis();
  pet.petting_count = 0;
  // Initialize animation
  pet.last_creature_move = millis();
  pet.last_eye_move = millis();
  pet.creature_offset_x = 0;
  pet.creature_offset_y = 0;
  pet.eye_offset_x = 0;
  pet.eye_offset_y = 0;
}

void draw_pet_main_screen() {
  Paint_Clear(BLACK);
  Paint_DrawString_EN(60, 10, "TAMAGOTCHI", &Font24, CASIO_GREEN, BLACK);
  
  // Pet name and stage at top
  Paint_DrawString_EN(20, 40, pet.name, &Font24, COL_WHITE, BLACK);
  const char* stage_names[] = {"Egg", "Baby", "Child", "Teen", "Adult"};
  Paint_DrawString_EN(150, 45, stage_names[pet.stage], &Font24, COL_GRAY, BLACK);
  
  // Large pet visual in center (Tamagotchi-style)
  int center_x = 120 + pet.creature_offset_x;  // Apply animation offset
  int center_y = 160 + pet.creature_offset_y;  // Apply animation offset
  
  if (pet.stage == EGG) {
    // Draw egg shape
    Paint_DrawCircle(center_x, center_y, 30, COL_WHITE, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawCircle(center_x, center_y - 5, 25, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    // Add pattern dots
    Paint_DrawCircle(center_x - 10, center_y, 3, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawCircle(center_x + 10, center_y, 3, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawCircle(center_x, center_y + 10, 3, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  } else {
    // Draw Tamagotchi-style creature (rounded blob body)
    // Body - rounded square shape
    Paint_DrawRectangle(center_x - 30, center_y - 20, center_x + 30, center_y + 30, 
                       COL_WHITE, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    
    // Head bump
    Paint_DrawCircle(center_x, center_y - 20, 15, COL_WHITE, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    
    // Eyes - big anime style with animation offset
    int eye_x_left = center_x - 15 + pet.eye_offset_x;
    int eye_x_right = center_x + 15 + pet.eye_offset_x;
    int eye_y = center_y - 5 + pet.eye_offset_y;
    Paint_DrawCircle(eye_x_left, eye_y, 6, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawCircle(eye_x_right, eye_y, 6, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    // Eye highlights
    Paint_DrawCircle(eye_x_left + 2, eye_y - 2, 2, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawCircle(eye_x_right + 2, eye_y - 2, 2, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    
    // Mouth - varies by mood
    if (pet.happiness > 70) {
      // Happy smile
      Paint_DrawLine(center_x - 12, center_y + 10, center_x, center_y + 15, 
                    COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
      Paint_DrawLine(center_x, center_y + 15, center_x + 12, center_y + 10, 
                    COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    } else if (pet.happiness < 30) {
      // Sad frown
      Paint_DrawLine(center_x - 12, center_y + 15, center_x, center_y + 10, 
                    COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
      Paint_DrawLine(center_x, center_y + 10, center_x + 12, center_y + 15, 
                    COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    } else {
      // Neutral line
      Paint_DrawLine(center_x - 10, center_y + 12, center_x + 10, center_y + 12, 
                    COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    }
    
    // Little arms/feet
    Paint_DrawLine(center_x - 30, center_y + 5, center_x - 40, center_y + 10, 
                  COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    Paint_DrawLine(center_x + 30, center_y + 5, center_x + 40, center_y + 10, 
                  COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    Paint_DrawLine(center_x - 20, center_y + 30, center_x - 20, center_y + 40, 
                  COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    Paint_DrawLine(center_x + 20, center_y + 30, center_x + 20, center_y + 40, 
                  COL_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
  }
  
  // Stats bars at BOTTOM with Font24
  int bar_y = 310;
  int bar_spacing = 35;
  
  Paint_DrawString_EN(10, bar_y, "FOOD", &Font24, COL_WHITE, BLACK);
  Paint_DrawRectangle(90, bar_y + 2, 220, bar_y + 18, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  int food_width = (pet.hunger * 130) / 100;
  Paint_DrawRectangle(91, bar_y + 3, 91 + food_width, bar_y + 17, 0xF800, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  
  bar_y += bar_spacing;
  Paint_DrawString_EN(10, bar_y, "JOY", &Font24, COL_WHITE, BLACK);
  Paint_DrawRectangle(90, bar_y + 2, 220, bar_y + 18, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  int happy_width = (pet.happiness * 130) / 100;
  Paint_DrawRectangle(91, bar_y + 3, 91 + happy_width, bar_y + 17, 0xFFE0, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  
  bar_y += bar_spacing;
  Paint_DrawString_EN(10, bar_y, "HP", &Font24, COL_WHITE, BLACK);
  Paint_DrawRectangle(90, bar_y + 2, 220, bar_y + 18, COL_WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  int health_width = (pet.health * 130) / 100;
  Paint_DrawRectangle(91, bar_y + 3, 91 + health_width, bar_y + 17, 0x07E0, DOT_PIXEL_1X1, DRAW_FILL_FULL);
  
  // Action buttons at bottom - CENTERED with Font16 to fit text
  int btn_y = 410;
  int btn_h = 35;
  int btn_w = 70;  // Width for each button
  int btn_spacing = 5;
  
  // Calculate centered positioning
  // Total width needed: 4 buttons + 3 spaces
  int total_width = (btn_w * 4) + (btn_spacing * 3);
  int start_x = (AMOLED_1IN8_WIDTH - total_width) / 2;  // Center horizontally
  
  // Feed button
  Paint_DrawRectangle(start_x, btn_y, start_x + btn_w, btn_y + btn_h, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(start_x + 12, btn_y + 10, "FEED", &Font16, 0xF800, BLACK);
  
  // Play button  
  int play_x = start_x + btn_w + btn_spacing;
  Paint_DrawRectangle(play_x, btn_y, play_x + btn_w, btn_y + btn_h, 0xFFE0, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(play_x + 12, btn_y + 10, "PLAY", &Font16, 0xFFE0, BLACK);
  
  // Clean button
  int clean_x = play_x + btn_w + btn_spacing;
  Paint_DrawRectangle(clean_x, btn_y, clean_x + btn_w, btn_y + btn_h, CYAN, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(clean_x + 8, btn_y + 10, "CLEAN", &Font16, CYAN, BLACK);
  
  // Medicine button
  int med_x = clean_x + btn_w + btn_spacing;
  Paint_DrawRectangle(med_x, btn_y, med_x + btn_w, btn_y + btn_h, 0x07E0, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(med_x + 17, btn_y + 10, "MED", &Font16, 0x07E0, BLACK);
  
  AMOLED_1IN8_Display(BlackImage);
}

// ---------- Touch handling ----------
void Touch_INT_callback(uint gpio, uint32_t events) {
  if (i2c_lock) return;
  if (gpio == Touch_INT_PIN) touch_flag = 1;
}

void handle_touch() {
  if (!touch_flag) return;
  static uint32_t last_touch_ms = 0;
  uint32_t now = millis();
  if (now - last_touch_ms < 60) { touch_flag = 0; return; } // debounce
  last_touch_ms = now;

  touch_flag = 0;

  while(i2c_lock);
  I2C_LOCK();
  if (!FT3168_Get_Point()) { I2C_UNLOCK(); return; }
  uint16_t ty = FT3168.y_point;
  uint16_t tx = FT3168.x_point;
  I2C_UNLOCK();
  
  // Game arcade touch handling
  if (current_screen == SCR_GAME_ARCADE) {
    if (current_game == GAME_MENU) {
      // Arcade menu - select games with original coordinates
      if (ty >= 120 && ty <= 180) {
        // ASTEROIDS
        current_game = ASTEROIDS;
        asteroids_level = 1; // Reset level
        init_asteroids();
        delay(200);
      } else if (ty >= 200 && ty <= 260) {
        // TETRIS
        current_game = TETRIS;
        init_tetris();
        delay(200);
      } else if (ty >= 280 && ty <= 340) {
        // SNAKE
        current_game = SNAKE;
        init_snake();
        delay(200);
      } else if (ty >= 360 && ty <= 420) {
        // BREAKOUT
        current_game = BREAKOUT;
        init_breakout();
        delay(200);
      }
    } else if (current_game == ASTEROIDS) {
      if (game_over) {
        // Game over - touch resets game
        asteroids_level = 1; // Reset level
        init_asteroids();
      } else {
        // Simple controls: left third = rotate left, right third = rotate right, middle = fire
        if (tx < AMOLED_1IN8_WIDTH / 3) {
          ship.angle -= 15;
        } else if (tx > (AMOLED_1IN8_WIDTH * 2) / 3) {
          ship.angle += 15;
        } else {
          // Fire bullet
          for(int i = 0; i < 5; i++) {
            if(!bullets[i].active) {
              bullets[i].active = true; 
              bullets[i].x = ship.x; 
              bullets[i].y = ship.y;
              float rad = ship.angle * 3.14159f / 180.0f;
              bullets[i].dx = 5.0f * cos(rad); 
              bullets[i].dy = 5.0f * sin(rad);
              bullets[i].fired_time = millis(); 
              break;
            }
          }
        }
      }
    } else if (current_game == TETRIS) {
      if (game_over) {
        init_tetris();
        delay(200);
      } else {
        // Simple immediate response like Games.txt with bounds checking
        bool left = tx < 123;
        bool middle = tx >= 123 && tx <= 245;
        bool right = tx > 245;
        
        if (left) {
          // Try to move left, with collision check
          if (tetris_can_place_piece(tetris_piece_x - 1, tetris_piece_y, tetris_current_piece)) {
            tetris_piece_x--;
          }
        } else if (middle) {
          // Rotate piece
          tetris_rotate_piece();
        } else if (right) {
          // Try to move right, with collision check
          if (tetris_can_place_piece(tetris_piece_x + 1, tetris_piece_y, tetris_current_piece)) {
            tetris_piece_x++;
          }
        }
        
        delay(150);
      }
      draw_tetris_game();
    } else if (current_game == SNAKE) {
      if (game_over) {
        init_snake();
      } else {
        // Snake direction controls
        if (tx < AMOLED_1IN8_WIDTH / 3) {
          snake_dir = 2; // left
        } else if (tx > (AMOLED_1IN8_WIDTH * 2) / 3) {
          snake_dir = 0; // right
        } else if (ty < AMOLED_1IN8_HEIGHT / 2) {
          snake_dir = 3; // up
        } else {
          snake_dir = 1; // down
        }
      }
    } else if (current_game == BREAKOUT) {
      if (game_over) {
        init_breakout();
      } else {
        // Move paddle to touch X position
        paddle_x = tx - paddle_w / 2;
        if (paddle_x < 0) paddle_x = 0;
        if (paddle_x > AMOLED_1IN8_WIDTH - paddle_w) paddle_x = AMOLED_1IN8_WIDTH - paddle_w;
      }
    }
    return;
  } else if (current_screen == SCR_GAME_TAMAGOTCHI) {
    // Handle tamagotchi touches
    if (pet.stage == EGG) {
      // Touch egg to hatch
      pet.stage = BABY;
      draw_pet_main_screen();
    } else if (ty >= 410 && ty <= 445) {
      // Bottom button area - use centered coordinates
      int btn_w = 70;
      int btn_spacing = 5;
      int total_width = (btn_w * 4) + (btn_spacing * 3);
      int start_x = (AMOLED_1IN8_WIDTH - total_width) / 2;
      
      if (tx >= start_x && tx <= start_x + btn_w) {
        // Feed button (red)
        pet.hunger = min(100, pet.hunger + 20);
        pet.happiness = min(100, pet.happiness + 5);
        draw_pet_main_screen();
      } else if (tx >= start_x + btn_w + btn_spacing && tx <= start_x + (btn_w * 2) + btn_spacing) {
        // Play button (yellow)
        pet.happiness = min(100, pet.happiness + 20);
        pet.hunger = max(0, pet.hunger - 5);
        draw_pet_main_screen();
      } else if (tx >= start_x + (btn_w * 2) + (btn_spacing * 2) && tx <= start_x + (btn_w * 3) + (btn_spacing * 2)) {
        // Clean button (cyan)
        pet.cleanliness = 100;
        pet.happiness = min(100, pet.happiness + 10);
        draw_pet_main_screen();
      } else if (tx >= start_x + (btn_w * 3) + (btn_spacing * 3) && tx <= start_x + (btn_w * 4) + (btn_spacing * 3)) {
        // Medicine button (green)
        pet.health = min(100, pet.health + 30);
        draw_pet_main_screen();
      }
    } else if (ty > 80 && ty < 280) {
      // Pet the creature in the center area (petting makes it happy)
      pet.happiness = min(100, pet.happiness + 3);
      pet.petting_count++;
      draw_pet_main_screen();
    }
    return;
  }

  // Normal watch touch handling
  uint16_t H = AMOLED_1IN8_HEIGHT;
  if (ty < H/3)              process_button(BTN_UP);     // Top 1/3 = UP = Future
  else if (ty > (H*2)/3)     process_button(BTN_DOWN);   // Bottom 1/3 = DOWN = Past
  else                       process_button(BTN_SELECT); // Middle = SELECT
}

// ---------- Time tick (minutes only) ----------
void tick_time() {
  uint32_t now = millis();
  if (now - last_tick_ms >= 1000) {
    last_tick_ms = now;
    static uint16_t sec_acc = 0;
    if (++sec_acc >= 60) {
      sec_acc = 0;
      if (++m >= 60) { m = 0; h = (h + 1) % 24; }
      if (current_screen == SCR_WATCHFACE) draw_watchface();
    }
  }
}

// ---------- Backlight dimming control ----------
void update_dimming() {
  // Don't dim during games
  if (current_screen == SCR_GAME_ARCADE || current_screen == SCR_GAME_TAMAGOTCHI) {
    return;
  }
  
  unsigned long now = millis();
  if (now - last_dim_ms >= DIM_INTERVAL) {
    last_dim_ms = now;
    if (brightness_value >= DIM_STEP) {
      brightness_value -= DIM_STEP;
    } else if (brightness_value > 0) {
      brightness_value = 0;
    }
    AMOLED_1IN8_SetBrightness(brightness_value);
  }
}

void set_brightness_and_restart(uint8_t v) {
  brightness_value = v;
  AMOLED_1IN8_SetBrightness(brightness_value);
  last_dim_ms = millis();
}

// ---------- QMI8658 tap polling ----------
static inline bool qmi_read_accel_g(float &gx, float &gy, float &gz) {
  float acc[3] = {0,0,0};
  QMI8658_read_acc_xyz(acc);
  gx = acc[0];
  gy = acc[1];
  gz = acc[2];
  return true;
}

void qmi_poll_for_tap_wake() {
  uint32_t now = millis();
  if (now - last_qmi_sample_ms < QMI_POLL_MS) return;
  last_qmi_sample_ms = now;

  float gx, gy, gz;
  if (!qmi_read_accel_g(gx, gy, gz)) return;

  // Slow gravity adaptation to filter out orientation changes
  ax_f = (1.0f - LPF_ALPHA) * ax_f + LPF_ALPHA * gx;
  ay_f = (1.0f - LPF_ALPHA) * ay_f + LPF_ALPHA * gy;
  az_f = (1.0f - LPF_ALPHA) * az_f + LPF_ALPHA * gz;

  // Remove gravity to get dynamic acceleration
  float dx = gx - ax_f;
  float dy = gy - ay_f;
  float dz = gz - az_f;

  // High-pass filter to isolate impulses
  hx = HPF_ALPHA * hx + (1.0f - HPF_ALPHA) * dx;
  hy = HPF_ALPHA * hy + (1.0f - HPF_ALPHA) * dy;
  hz = HPF_ALPHA * hz + (1.0f - HPF_ALPHA) * dz;

  // Calculate total acceleration magnitude
  float mag = sqrt(hx*hx + hy*hy + hz*hz);
  
  // Simple threshold check - should detect deliberate taps
  if (mag >= TAP_G_THRESH) {
    if (now - last_tap_ms >= TAP_DEBOUNCE) {
      last_tap_ms = now;
      // Wake to 50% and restart dim cadence
      set_brightness_and_restart(128);
      if (current_screen == SCR_WATCHFACE) draw_watchface();
    }
  }
}

// ---------- Live sensor readings ----------
void update_sensor_readings() {
  // Protect I2C access from conflicts with touch sensor
  if (i2c_lock) return; // Skip if I2C is busy
  
  while(i2c_lock);
  I2C_LOCK();
  
  // Update battery percentage from AXP2101
  battery_percent = get_battery_percentage();
  
  // Update temperature from AXP2101 (convert Celsius to Fahrenheit)
  float temp_c = get_temperature();
  temp_F = (int8_t)(temp_c * 9.0f / 5.0f + 32.0f);
  
  I2C_UNLOCK();
}

// ---------- Setup / Loop ----------
void setup() {
  // Display init
  audio_init();
  audio_set_volume(40);
  DEV_Module_Init();
  QSPI_GPIO_Init(qspi);
  QSPI_PIO_Init(qspi);
  QSPI_1Wrie_Mode(&qspi);
  AMOLED_1IN8_Init();

  // Start at 100% brightness
  set_brightness_and_restart(255);

  // Framebuffer
  UDOUBLE ImageSize = AMOLED_1IN8_HEIGHT * AMOLED_1IN8_WIDTH * 2;
  BlackImage = (UWORD*)malloc(ImageSize);
  Paint_NewImage((UBYTE*)BlackImage, AMOLED_1IN8.WIDTH, AMOLED_1IN8.HEIGHT, 0, BLACK);
  Paint_SetScale(65);
  Paint_SetRotate(ROTATE_0);
  Paint_Clear(BLACK);

  // Touch init & interrupt
  FT3168_Init(FT3168_Point_Mode);
  DEV_KEY_Config(Touch_INT_PIN);
  DEV_IRQ_SET(Touch_INT_PIN, GPIO_IRQ_EDGE_RISE, &Touch_INT_callback);

  // Physical BACK button
  pinMode(BACK_BUTTON_PIN, INPUT);

  // QMI8658 IMU init
  QMI8658_init();

  // AXP2101 power management init
  init_axp2101();

  // Initialize game systems - use custom RNG to avoid hardware conflicts
  seed_rng(millis());
  init_pet();

  // Splash
  Paint_DrawString_EN(30, 180, "Pebble-Style Watch", &Font24, CASIO_GREEN, COL_BLACK);
  Paint_DrawString_EN(10, 210, "Complete Games Edition", &Font24, CASIO_GREEN, COL_BLACK);
  Paint_DrawString_EN(25, 240, "Tetris Snake Breakout", &Font24, CASIO_GREEN, COL_BLACK);
  Paint_DrawString_EN(80, 270, "Asteroids", &Font24, CASIO_GREEN, COL_BLACK);
  AMOLED_1IN8_Display(BlackImage);
  delay(3000);
  AMOLED_1IN8_Display(BlackImage);
  delay(3000);

  last_tick_ms = millis();
  open_watchface();
}

void loop() {
  handle_touch();

  // BACK button debounce + brightness pop to 100%
  bool reading = digitalRead(BACK_BUTTON_PIN);
  if (reading != backLastState) backLastDebounce = millis();
  if (millis() - backLastDebounce > BACK_DEBOUNCE_MS) {
    if (reading != backBtnState) {
      backBtnState = reading;
      if (backBtnState == HIGH) {
        set_brightness_and_restart(255);
        process_button(BTN_BACK);
      }
    }
  }
  backLastState = reading;

  tick_time();
  update_dimming();
  qmi_poll_for_tap_wake();
  
  // Update sensor readings every loop
  update_sensor_readings();
  
  // Update games - but not every loop to prevent overload
  static uint32_t last_game_update = 0;
  if (current_screen == SCR_GAME_ARCADE && millis() - last_game_update > 50) { // 20 FPS max
    last_game_update = millis();
    if (current_game == ASTEROIDS) {
      update_asteroids();
      draw_asteroids_game();
    } else if (current_game == TETRIS) {
      update_tetris();
      draw_tetris_game();
    } else if (current_game == SNAKE) {
      update_snake();
      draw_snake_game();
    } else if (current_game == BREAKOUT) {
      update_breakout();
      draw_breakout_game();
    }
  }
  
  // Tamagotchi updates
  if (current_screen == SCR_GAME_TAMAGOTCHI) {
    uint32_t now = millis();
    static uint32_t last_stat_decay = 0;
    static uint32_t last_redraw = 0;
    
    // Animate creature movement every 3 seconds
    if (now - pet.last_creature_move > 3000) {
      pet.last_creature_move = now;
      pet.creature_offset_x = rng(-5, 6);
      pet.creature_offset_y = rng(-5, 6);
    }
    
    // Animate eyes every 1 second
    if (now - pet.last_eye_move > 1000) {
      pet.last_eye_move = now;
      pet.eye_offset_x = rng(-3, 4);
      pet.eye_offset_y = rng(-3, 4);
    }
    
    // Decay stats every 30 seconds
    if (now - last_stat_decay > 30000) {
      last_stat_decay = now;
      // Decrease stats over time
      if (pet.hunger > 0) pet.hunger = max(0, pet.hunger - 2);
      if (pet.happiness > 0) pet.happiness = max(0, pet.happiness - 1);
      if (pet.cleanliness > 0) pet.cleanliness = max(0, pet.cleanliness - 1);
      // Health decreases if other stats are low
      if (pet.hunger < 20 || pet.happiness < 20 || pet.cleanliness < 30) {
        pet.health = max(0, pet.health - 3);
      }
    }
    
    // Redraw every second to show animations
    if (now - last_redraw > 1000) {
      last_redraw = now;
      draw_pet_main_screen();
    }
    
    // Ensure stats don't go below 0
    if (pet.hunger < 0) pet.hunger = 0;
    if (pet.happiness < 0) pet.happiness = 0;
    if (pet.health < 0) pet.health = 0;
  }

  delay(10);
}
