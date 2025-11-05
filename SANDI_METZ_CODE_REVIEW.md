# Sandi Metz Code Review: Pico1.8 Smartwatch Firmware

**Reviewer Perspective**: Sandi Metz - Object-Oriented Design Expert
**Review Date**: 2025-11-05
**Codebase**: Pebble-style watch firmware (RP2350 + AMOLED + Games)

---

## Executive Summary

This embedded C++ project demonstrates **impressive ambition and functionality**, delivering a complete smartwatch with games, a Tamagotchi pet, and hardware management. However, from an object-oriented design perspective, the codebase exhibits **critical architectural problems** that will make it increasingly difficult to maintain, test, and extend.

**Key Concerns**:
- Files regularly exceed 2,000+ lines (vs. 100 line rule)
- Functions exceed 100+ lines (vs. 5 line rule)
- Massive global state coupling
- Zero dependency injection
- Untestable procedural design
- Single Responsibility Principle violations throughout

---

## Sandi Metz's Rules Violations

### Rule 1: Classes no longer than 100 lines

**Status**: ❌ **SEVERE VIOLATIONS**

| File | Lines | Violation |
|------|-------|-----------|
| `Pico1.8.ino` | 2,038 | **20.4x over limit** |
| `GUI_Paint.cpp` | 746 | **7.5x over limit** |
| `font24.cpp` | 2,520 | **25.2x over limit** |
| `font20.cpp` | 2,142 | **21.4x over limit** |
| `QMI8658.cpp` | 617 | **6.2x over limit** |

**Impact**:
- The main application file (`Pico1.8.ino:1-2038`) is a **God Object** that knows everything and does everything
- Impossible to understand without reading thousands of lines
- Changes ripple unpredictably across the entire codebase

**Recommendation**:
Break `Pico1.8.ino` into focused classes:
```cpp
// Instead of one 2000-line file, extract:
class WatchFace { /* UI for time display */ };
class GameManager { /* Game lifecycle */ };
class TamagotchiPet { /* Pet logic */ };
class BatteryMonitor { /* AXP2101 interface */ };
class TouchInput { /* FT3168 handling */ };
class MenuNavigator { /* Menu state machine */ };
```

---

### Rule 2: Methods no longer than 5 lines

**Status**: ❌ **SEVERE VIOLATIONS**

Methods routinely exceed 50-100+ lines:

**Examples**:

1. **`Paint_DrawCircle()` (GUI_Paint.cpp:386-443)** - **58 lines**
   - Draws filled vs. hollow circles
   - Implements 8-point circle algorithm
   - Should be 3-4 smaller methods

2. **`draw_tetris_game()` (Pico1.8.ino:625-717)** - **93 lines**
   - Clears screen
   - Draws grid, pieces, UI, preview
   - Should be extracted to separate rendering methods

3. **`update_asteroids()` (Pico1.8.ino:1009-1120)** - **112 lines**
   - Physics updates
   - Collision detection
   - Level progression
   - Should be 6-8 focused methods

4. **`Paint_DrawString_CN()` (GUI_Paint.cpp:546-606)** - **61 lines**
   - Handles both ASCII and Chinese characters
   - Complex bitmap rendering logic
   - Violates Single Responsibility

**What 5 lines looks like** (Sandi Metz example):
```cpp
void draw_tetris_game() {
    render_game_over_if_needed();
    render_playing_field();
    render_score_and_controls();
    display_frame();
}
```

**Impact**:
- Methods are impossible to test in isolation
- Difficult to reuse logic
- Hard to understand what code does at a glance
- Debugging requires reading entire functions

---

### Rule 3: Pass no more than 4 parameters

**Status**: ⚠️ **MODERATE VIOLATIONS**

Most methods respect this rule, but some violations exist:

**Examples**:

1. **`Paint_DrawPoint()` (GUI_Paint.cpp:252-279)**
   ```cpp
   void Paint_DrawPoint(UWORD Xpoint, UWORD Ypoint, UWORD Color,
                        DOT_PIXEL Dot_Pixel, DOT_STYLE Dot_Style)  // 5 params
   ```

2. **`Paint_DrawLine()` (GUI_Paint.cpp:292-340)**
   ```cpp
   void Paint_DrawLine(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend,
                       UWORD Color, DOT_PIXEL Line_width, LINE_STYLE Line_Style)  // 7 params!
   ```

3. **`Paint_DrawRectangle()` (GUI_Paint.cpp:353-373)**
   ```cpp
   void Paint_DrawRectangle(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend,
                            UWORD Color, DOT_PIXEL Line_width, DRAW_FILL Draw_Fill)  // 7 params!
   ```

**Recommendation**:
Introduce parameter objects:
```cpp
struct Point { UWORD x, y; };
struct Rect { Point start, end; };
struct DrawStyle { UWORD color; DOT_PIXEL width; LINE_STYLE style; };

// Instead of 7 parameters:
void Paint_DrawLine(Point start, Point end, DrawStyle style);
void Paint_DrawRectangle(Rect bounds, DrawStyle style);
```

**Why it matters**:
- Each parameter increases complexity exponentially
- Hard to remember parameter order
- Easy to swap parameters by mistake
- Parameter objects make intent clear

---

### Rule 4: One object per controller/view

**Status**: ❌ **NOT APPLICABLE** (Embedded C, but principle violated)

While this is a web framework rule, the **underlying principle** (limit coupling) is violated:

**In `process_button()` (Pico1.8.ino:1194-1328)**:
- Directly manipulates 8+ global variables
- Knows about every screen type
- 135 lines of nested conditionals
- Changes to any screen require modifying this function

**What good OO looks like**:
```cpp
// Each screen handles its own input
interface Screen {
    virtual void handleButton(VButton b) = 0;
};

class WatchFaceScreen : public Screen {
    void handleButton(VButton b) override {
        // Only knows about watchface logic
    }
};

class MenuScreen : public Screen {
    void handleButton(VButton b) override {
        // Only knows about menu logic
    }
};
```

---

## Critical Design Issues

### 1. **Global State Everywhere** 🔴

The codebase relies on **dozens of global variables** (Pico1.8.ino:139-329):

```cpp
// From line 139 onwards:
UWORD *BlackImage = nullptr;                    // Global framebuffer
volatile uint8_t touch_flag = 0;                // Touch state
uint8_t brightness_value = 255;                 // Display brightness
Screen current_screen = SCR_WATCHFACE;          // UI state
uint32_t last_tick_ms = 0;                      // Time tracking
uint8_t h = 12, m = 0;                         // Current time
bool bt_connected = true;                       // Bluetooth
uint8_t battery_percent = 82;                   // Battery
GameState current_game = GAME_MENU;             // Game state
Ship ship;                                      // Asteroids game
Bullet bullets[5];                              // Bullets
Asteroid asteroids[15];                         // Asteroids
int tetris_grid[GRID_H][GRID_W];               // Tetris grid
Tamagotchi pet;                                 // Pet state
// ... and 50+ more globals
```

**Problems**:
- **Impossible to test**: Can't instantiate clean state for unit tests
- **Hidden dependencies**: Functions reach out and touch global state
- **Race conditions**: Multiple systems modify shared state
- **No encapsulation**: Everything can modify everything

**Example of hidden coupling** (Pico1.8.ino:1195):
```cpp
void process_button(VButton b) {
    set_brightness_and_restart(255);  // Touches global brightness_value
    // ... modifies current_screen, menu_sel, games_sel, etc.
}
```

**Sandi Metz's solution**: **Dependency Injection**
```cpp
class ButtonHandler {
    Screen& screen;
    BrightnessControl& brightness;

    ButtonHandler(Screen& s, BrightnessControl& b)
        : screen(s), brightness(b) {}

    void process(VButton b) {
        brightness.reset();
        screen.handleButton(b);
    }
};
```

---

### 2. **God Object: Pico1.8.ino** 🔴

This file violates **Single Responsibility Principle** catastrophically:

**What it does** (partial list):
- Battery management (lines 25-47)
- Random number generation (lines 49-64)
- Game logic for 4 different games (lines 463-1191)
- Tamagotchi pet simulation (lines 1466-1500+)
- UI rendering for 10+ screens
- Touch input handling
- Button debouncing
- IMU tap-to-wake
- Theme management
- Time/date setting
- Menu navigation

**Why this is problematic**:
- Changes to games affect battery code (shared file)
- Can't reuse game logic in other projects
- Merge conflicts on every feature
- Impossible to reason about

**Recommendation**:
Extract to separate files with clear boundaries:
```
src/
  hardware/
    Battery.cpp/h       - AXP2101 interface
    TouchInput.cpp/h    - FT3168 handling
    IMU.cpp/h           - QMI8658 tap detection
  ui/
    WatchFace.cpp/h     - Main watch display
    Menu.cpp/h          - Menu system
    Theme.cpp/h         - Color schemes
  games/
    Tetris.cpp/h        - Self-contained Tetris
    Snake.cpp/h         - Self-contained Snake
    Asteroids.cpp/h     - Self-contained Asteroids
    Breakout.cpp/h      - Self-contained Breakout
  pet/
    Tamagotchi.cpp/h    - Pet simulation
```

---

### 3. **Tell, Don't Ask** Violations 🔴

Code repeatedly asks objects for data, then makes decisions:

**Bad Example** (Pico1.8.ino:628-638):
```cpp
void draw_tetris_game() {
    Paint_Clear(BLACK);

    if(game_over) {  // Asking game state
        Paint_DrawString_EN(60, 180, "GAME OVER", &Font24, 0xF800, BLACK);
        char score_str[32];
        sprintf(score_str, "Score: %d", game_score);  // Asking for score
        // ... more rendering based on asked data
    }
}
```

**Sandi Metz's way**:
```cpp
class TetrisGame {
    void render() {
        if (isGameOver()) {
            renderGameOver();
        } else {
            renderActiveGame();
        }
    }

private:
    void renderGameOver() {
        // Game knows how to render itself
    }
};
```

**Why it matters**:
- Current design couples rendering to game internals
- Can't change game without changing renderer
- Logic scattered across codebase

---

### 4. **Duplication vs. Wrong Abstraction** ⚠️

Sandi Metz says: **"Duplication is far cheaper than the wrong abstraction"**

This codebase has **good instincts** here:
- Each game (Tetris, Snake, Asteroids, Breakout) is implemented separately
- No premature "Game Base Class" abstraction
- Similar rendering code duplicated across games

**What's working**:
```cpp
// Each game has its own init/update/draw - good!
void init_tetris() { /* Tetris-specific setup */ }
void update_tetris() { /* Tetris-specific logic */ }
void draw_tetris_game() { /* Tetris-specific rendering */ }

void init_snake() { /* Snake-specific setup */ }
void update_snake() { /* Snake-specific logic */ }
void draw_snake_game() { /* Snake-specific rendering */ }
```

**Improvement**: Keep them separate, but encapsulate:
```cpp
class TetrisGame {
public:
    void init();
    void update();
    void render();
private:
    int grid[GRID_H][GRID_W];
    int score;
    // ... all Tetris state here
};
```

This preserves flexibility while adding encapsulation.

---

### 5. **Lack of Testability** 🔴

**Current state**: Literally **impossible to unit test**

**Why**:
- All logic requires hardware (framebuffer, I2C, SPI)
- Global state prevents isolation
- Functions have side effects (modify display, play sounds)
- No mocking possible

**Example** - try to test this:
```cpp
void draw_watchface() {
    Paint_Clear(THEMES[theme_idx].bg);  // Global theme_idx
    int centerX = AMOLED_1IN8_WIDTH / 2 - 60;
    draw_big_time_centered(centerX, 30, h, m, CASIO_GREEN);  // Global h, m
    draw_right_complications(50);  // Reads battery_percent, temp_F, day
    AMOLED_1IN8_Display(BlackImage);  // Hardware call
}
```

**Sandi Metz's testable design**:
```cpp
class WatchFace {
    Theme& theme;
    TimeSource& time;
    BatteryMonitor& battery;
    Display& display;

public:
    WatchFace(Theme& t, TimeSource& ts, BatteryMonitor& b, Display& d)
        : theme(t), time(ts), battery(b), display(d) {}

    void render() {
        DisplayBuffer buffer(display.width(), display.height());
        buffer.clear(theme.background());
        renderTime(buffer, time.getHour(), time.getMinute());
        renderBattery(buffer, battery.getPercentage());
        display.show(buffer);
    }
};

// Now testable with mocks:
TEST(WatchFace, renders_time_correctly) {
    MockTheme theme;
    MockTimeSource time(14, 30);  // 2:30 PM
    MockBattery battery(85);
    MockDisplay display;

    WatchFace face(theme, time, battery, display);
    face.render();

    ASSERT_TRUE(display.containsText("14"));
    ASSERT_TRUE(display.containsText("30"));
}
```

---

### 6. **The Law of Demeter Violations** 🔴

**"Only talk to your immediate friends"**

**Bad Example** (Pico1.8.ino:406):
```cpp
void draw_right_complications(int startY) {
    Paint_DrawRectangle(x, y, x + widget_width, y + widget_height,
                       CASIO_GREEN, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    // Directly reaches into global Paint object's internals
    Paint_DrawString_EN(x + 18, y + 8, "BT", &Font20,
                       CASIO_GREEN, THEMES[theme_idx].bg);
}
```

**Violations**:
- Function knows Paint's API
- Function knows Theme structure
- Function knows font details
- Too many dependencies

**Better**:
```cpp
class ComplicationRenderer {
    Canvas& canvas;
    Theme& theme;

public:
    void renderBluetooth(Point position, bool connected) {
        if (connected) {
            canvas.drawBox(position, theme.accentColor());
            canvas.drawText(position.offsetBy(18, 8), "BT", theme.accentColor());
        }
    }
};
```

---

### 7. **Switch Statement Smell** ⚠️

**Long switch statements are a code smell** - they should be polymorphism.

**Example** (GameAudio.cpp:96-177):
```cpp
void audio_play_sfx(SoundEffect sfx) {
    switch (sfx) {
        case SFX_BEEP:
            audio_play_tone(800, 50);
            break;
        case SFX_SELECT:
            audio_play_tone(1200, 50);
            delay(20);
            audio_play_tone(1500, 50);
            break;
        // ... 10 more cases
    }
}
```

**Sandi Metz's polymorphic solution**:
```cpp
class SoundEffect {
public:
    virtual void play(AudioOutput& audio) = 0;
};

class BeepSound : public SoundEffect {
    void play(AudioOutput& audio) override {
        audio.playTone(800, 50);
    }
};

class SelectSound : public SoundEffect {
    void play(AudioOutput& audio) override {
        audio.playTone(1200, 50);
        audio.delay(20);
        audio.playTone(1500, 50);
    }
};
```

**Benefits**:
- Each sound is a class (testable)
- Adding sounds doesn't modify existing code (Open/Closed Principle)
- Sound logic encapsulated

---

### 8. **Magic Numbers Everywhere** ⚠️

**Examples**:
```cpp
// Pico1.8.ino:1195
set_brightness_and_restart(255);  // What is 255? Max brightness?

// Pico1.8.ino:1334
int centerX = AMOLED_1IN8_WIDTH / 2 - 60;  // Why 60?

// GUI_Paint.cpp:164-169
if(X > Paint.WidthMemory || Y > Paint.HeightMemory) return;
if(Paint.Scale == 2) {  // Why 2? What does it mean?
    UDOUBLE Addr = X / 8 + Y * Paint.WidthByte;  // Why 8?
```

**Recommendation**: Extract named constants
```cpp
const uint8_t BRIGHTNESS_MAX = 255;
const int WATCHFACE_CENTER_OFFSET = 60;
const int BITS_PER_BYTE = 8;
const int SCALE_1BPP = 2;

set_brightness_and_restart(BRIGHTNESS_MAX);
int centerX = AMOLED_1IN8_WIDTH / 2 - WATCHFACE_CENTER_OFFSET;
UDOUBLE Addr = X / BITS_PER_BYTE + Y * Paint.WidthByte;
```

---

### 9. **Error Handling** ⚠️

Error handling is **minimal** and uses `Debug()` logging:

```cpp
// GUI_Paint.cpp:114-116
if(Xpoint > Paint.Width || Ypoint > Paint.Height){
    Debug("Exceeding display boundaries\r\n");
    return;  // Silent failure
}
```

**Problems**:
- Errors silently ignored in production
- No way to detect failures
- Debug output only - no logging levels

**Better approach**:
```cpp
enum class PaintError { OutOfBounds, InvalidScale, NullImage };

class PaintResult {
    bool success;
    PaintError error;
public:
    static PaintResult ok() { return {true, {}}; }
    static PaintResult err(PaintError e) { return {false, e}; }
    bool isOk() const { return success; }
};

PaintResult Paint_SetPixel(UWORD x, UWORD y, UWORD color) {
    if (x > Paint.Width || y > Paint.Height) {
        return PaintResult::err(PaintError::OutOfBounds);
    }
    // ... paint pixel
    return PaintResult::ok();
}
```

---

### 10. **Naming Inconsistencies** ⚠️

**Inconsistent naming conventions**:

```cpp
// Mix of styles:
void Paint_DrawLine(...)      // Prefix notation
void audio_play_sfx(...)      // Snake_case
void DEV_Module_Init(void)    // SCREAMING_Snake_case
void draw_watchface()         // snake_case
bool tetris_can_place_piece() // snake_case with prefix

// Hungarian notation mixed in:
UWORD Xpoint    // Type prefix + CamelCase
uint8_t h, m    // Single letter
int snake_x[SNAKE_MAX_LENGTH]  // snake_case
```

**Recommendation**: Pick **one convention** and stick to it:

```cpp
// Option 1: Modern C++ style
class Paint {
    void drawLine(...);
    void drawCircle(...);
};

// Option 2: C style (if not using classes)
void paint_draw_line(...);
void paint_draw_circle(...);
void audio_play_sfx(...);
```

---

## What's Going Well ✅

Despite the issues, this code demonstrates **some good practices**:

### 1. **Avoiding Premature Abstraction** ✅
- Games are kept separate (no forced inheritance hierarchy)
- Following "duplication is cheaper than wrong abstraction"

### 2. **Clear File Organization** ✅
- Hardware drivers in separate files
- Fonts isolated from logic
- Game code together

### 3. **Reasonable Function Names** ✅
- `init_tetris()`, `update_asteroids()`, `draw_snake_game()` are clear
- Intent is obvious from name

### 4. **No Deep Inheritance** ✅
- No complex class hierarchies
- Procedural approach is honest about design

### 5. **Consistent Data Structures** ✅
- Game objects use simple structs (`Ship`, `Bullet`, `Asteroid`)
- Not over-engineered

---

## Priority Recommendations

### Immediate (Next Sprint)

1. **Extract Games to Classes** (Pico1.8.ino:463-1191)
   - Create `TetrisGame`, `SnakeGame`, `AsteroidsGame`, `BreakoutGame` classes
   - Move all game state into class members (remove globals)
   - Each class owns its own `init()`, `update()`, `render()`

2. **Dependency Injection for Display** (GUI_Paint.cpp:1-746)
   - Pass `Paint` object as parameter instead of global
   - Create `Display` interface for testing
   - Extract into `Renderer` class

3. **Break Up Long Methods** (All files)
   - Target: No function > 20 lines (5 is aspirational for embedded)
   - Start with `draw_tetris_game()`, `update_asteroids()`, `process_button()`

### Medium-term (Next Quarter)

4. **Introduce Screen Interface**
   ```cpp
   class Screen {
   public:
       virtual void render() = 0;
       virtual void handleButton(VButton b) = 0;
   };
   ```

5. **Extract Hardware Abstraction Layer**
   - Create interfaces for battery, touch, display, IMU
   - Enable mock implementations for testing

6. **Reduce Global State**
   - Create `GameState`, `UIState`, `HardwareState` classes
   - Pass as parameters instead of accessing globals

### Long-term (Next Year)

7. **Add Unit Tests**
   - Start with pure logic (game collision detection, score calculation)
   - Mock hardware interfaces
   - Aim for 50% coverage on business logic

8. **Refactor Paint Library**
   - Break into `Canvas`, `Renderer`, `Font` classes
   - Each < 100 lines
   - Remove magic numbers

---

## Metrics Summary

| Metric | Current | Target | Status |
|--------|---------|--------|--------|
| Longest file | 2,520 lines | 100 lines | 🔴 25x over |
| Longest method | 135 lines | 5 lines | 🔴 27x over |
| Global variables | ~80+ | 0 | 🔴 Critical |
| Max parameters | 7 | 4 | 🔴 Violations |
| Unit test coverage | 0% | 60% | 🔴 None |
| Cyclomatic complexity | Very High | Low | 🔴 High |

---

## Final Thoughts (in Sandi's voice)

> "This codebase works, and that's wonderful. The games run, the battery monitoring functions, the touch screen responds. You've built something real. But **working code is not the same as maintainable code**.
>
> Right now, changing anything is scary. Adding a new game means touching the 2,000-line main file. Testing requires hardware. Understanding requires reading thousands of lines.
>
> **Small, focused objects with clear responsibilities** - that's the path forward. Not because it's 'pure' or 'correct', but because **it makes your life easier**.
>
> Start small. Extract one game. Inject one dependency. Write one test. Then do it again. **The code will thank you, and so will future-you when debugging at 2am**."

---

## References

- **"Practical Object-Oriented Design in Ruby"** (POODR) - Sandi Metz
- **"99 Bottles of OOP"** - Sandi Metz & Katrina Owen
- **Sandi Metz's Rules**: https://thoughtbot.com/blog/sandi-metz-rules-for-developers
- **"The Wrong Abstraction"**: https://sandimetz.com/blog/2016/1/20/the-wrong-abstraction

---

**Review completed by**: Claude (in the style of Sandi Metz)
**Codebase version**: Pico1.8 (RP2350 + AMOLED)
**Total files reviewed**: 22 source files + 9 font files
**Lines of code analyzed**: ~16,500 lines
