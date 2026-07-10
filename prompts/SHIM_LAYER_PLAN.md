# ETR 0.8.4 I/O Interface Map (PS3 Porting Reference)

## 1. VIDEO / GRAPHICS

### Dependencies
- **OpenGL 1.x** (fixed-function pipeline) — `glBegin`/`glEnd`, `glVertex3`, display lists, `gluPerspective`
- **SFML Graphics** — `sf::Texture`, `sf::Text`, `sf::Sprite`, `sf::RectangleShape`
- **GLU** — `gluPerspective()`, `gluErrorString()`

### Key Files & Functions

| File | Key Functions | What It Does |
|---|---|---|
| `winsys.h/cpp` | `CWinsys::Init()`, `SetupVideoMode()`, `SwapBuffers()`, `clear()`, `draw()`, `beginSFML()/endSFML()` | Window creation via `sf::RenderWindow`, SFML<->GL mode switching |
| `ogl.h/cpp` | `InitOpenglExtensions()`, `Reshape()`, `Setup2dScene()`, `ClearRenderContext()`, `PushRenderMode()/PopRenderMode()`, `set_material()` | GL state management, projection setup, material/lighting helpers |
| `textures.h/cpp` | `TTexture::Load()`, `Bind()`, `Draw()`, `CTexture::LoadTextureList()` | Loads PNGs via `sf::Texture::loadFromFile()`, draws via GL vertex arrays |
| `gui.h/cpp` | `DrawGUI()`, `DrawFrameX()`, `DrawGUIBackground()` | All 2D UI rendered via SFML (`sf::Text`, `sf::RectangleShape`, `sf::Sprite`) |
| `font.h/cpp` | `CFont::LoadFont()`, `DrawString()`, `GetTextWidth()` | TTF fonts via `sf::Font::loadFromFile()`, text via `sf::Text` |
| `hud.h/cpp` | `DrawHud()`, gauge bar rendering | Mix: gauge bars use raw GL vertex arrays, text uses SFML font |
| `env.h/cpp` | `DrawSkybox()`, `SetupLight()`, `SetupFog()`, `DrawFog()` | `glLightfv`, `glFogfv`, skybox quads with `glVertexPointer` |
| `course_render.cpp` | `RenderCourse()`, `setup_course_tex_gen()` | `glTexGenfv` for auto-texturing, quadtree rendering |
| `tux.cpp` | `CCharShape::Draw()`, `DrawShadow()` | Character model rendering via GL, stencil shadow |
| `particles.cpp` | `draw_particles()` | GL point sprites / vertex arrays |
| `track_marks.cpp` | `DrawTrackmarks()` | GL vertex arrays for ski tracks |
| `view.h/cpp` | `update_view()`, `SetupViewFrustum()`, `SetCameraDistance()` | Camera matrix via `gluLookAt` / manual matrix math |

### GL Extensions Used
- `GL_EXT_compiled_vertex_array` — `glLockArraysEXT`/`glUnlockArraysEXT` (loaded via `sf::Context::getFunction()` in `ogl.cpp:58`)

### Platform #ifdefs
| File | Lines | Purpose |
|---|---|---|
| `bh.h:34-73` | `_WIN32`, `__APPLE__`, `__linux__` | Platform detection, GL headers (`<GL/gl.h>`, `<GL/glx.h>`) |
| `ogl.cpp:58` | — | `sf::Context::getFunction()` for GL extension loading |
| `ogl.cpp:362-374` | `USE_STENCIL_BUFFER` | Shadow stencil vs depth path |
| `env.cpp:202` | `OS_LINUX` | Skybox UV seam fix |
| `winsys.cpp:72-88` | `USE_STENCIL_BUFFER`, `_WIN32` | Stencil bits, Win32 icon |

---

## 2. AUDIO

### Dependencies
- **SFML Audio** — `sf::Sound`, `sf::SoundBuffer`, `sf::Music`

### Key Files & Functions

| File | Key Functions | What It Does |
|---|---|---|
| `audio.h/cpp` | `CSound::LoadChunk()`, `Play()`, `Halt()`, `SetVolume()` | Sound effects: `sf::SoundBuffer::loadFromFile()` + `sf::Sound::play()` |
| `audio.h/cpp` | `CMusic::LoadPiece()`, `Play()`, `PlayTheme()`, `Halt()` | Streaming music: `sf::Music::openFromFile()` + `sf::Music::play()` |

### Asset Manifests
- `data/sounds/sounds.lst` — `*[name] tree_hit [file] tree_hit.wav [vol] 1.0`
- `data/music/music.lst` — `*[name] credits_1 [file] credits1-cp.ogg`
- `data/music/racing_themes.lst` — `*[name] normal [race] race_1 [wonrace] wonrace_1 [lostrace] lostrace_1`

### File Formats
- Sound: `.wav` (PCM) — loaded fully into memory
- Music: `.ogg` (Vorbis) — streamed from disk

---

## 3. INPUT

### Dependencies
- **SFML Window** — `sf::Event`, `sf::Joystick`

### Key Files & Functions

| File | Key Functions | What It Does |
|---|---|---|
| `states.h/cpp` | `State::Manager::PollEvent()` | Dispatches `sf::Event::JoystickMoved` → `Jaxis(axis, value)`, `JoystickButtonPressed/Released` → `Jbutt(button, pressed)` |
| `winsys.h/cpp` | `CWinsys::PollEvent()`, `PrintJoystickInfo()` | Wraps `window.pollEvent()`, joystick enumeration |

### Gamepad Mapping (current)
- Axis 0 (X): horizontal stick → steering (racing) / menu nav
- Axis 1 (Y): vertical stick → paddle/brake (racing) / menu nav
- Button 0: Confirm / Paddle
- Button 1: Back / Trick modifier
- Button 2: Brake
- Button 3: Charge/Jump
- Button 4/5: Gear down/up
- Button 7: Start (exit to map select)

### State Virtual Interface (`states.h:58-63`)
```cpp
virtual void Enter() {}
virtual void Loop(float time_step) {}
virtual void Jaxis(int axis, float value) {}
virtual void Jbutt(int button, bool pressed) {}
virtual void Exit() {}
```

---

## 4. ASSETS

### Directory Structure (from `game_config.cpp:331-342`)
All paths relative to `param.data_dir` (`ETR_DATA_DIR + "/etr"`):

| `param` field | Path | Contents |
|---|---|---|
| `tex_dir` | `data_dir/textures` | PNG textures + `textures.lst` |
| `font_dir` | `data_dir/fonts` | TTF fonts + `fonts.lst` |
| `common_course_dir` | `data_dir/courses` | Course maps (elev.png, terrain.png, trees.png, course.dim, preview.png, items.lst) |
| `char_dir` | `data_dir/char` | Characters (preview.png, shape.lst, start/finish/wonrace/lostrace.lst) |
| `terr_dir` | `data_dir/terrains` | `terrains.lst` + terrain texture PNGs |
| `obj_dir` | `data_dir/objects` | `object_types.lst` + object texture PNGs |
| `env_dir2` | `data_dir/env` | Skybox PNGs + `environment.lst` + `light.lst` per location/light |
| `sounds_dir` | `data_dir/sounds` | WAV files + `sounds.lst` |
| `music_dir` | `data_dir/music` | OGG files + `music.lst` + `racing_themes.lst` |
| `trans_dir` | `data_dir/translations` | `languages.lst` + per-language `.lst` files |
| `player_dir` | `data_dir/players` | `avatars.lst` + avatar PNGs |

### Course Directory Structure (`data/courses/<group>/<course>/`)
| File | Format | Loader |
|---|---|---|
| `course.dim` | SP-line: `[width]90.0[length]520.0...` + translations | `CCourseList::Load()` |
| `preview.png` | PNG | `TTexture::Load()` |
| `elev.png` | PNG RGBA (brightness = height) | `sf::Image::loadFromFile()` → pixel read |
| `terrain.png` | PNG RGBA (color = terrain type) | `sf::Image::loadFromFile()` → pixel read |
| `trees.png` | PNG RGBA (color = object type) | `sf::Image::loadFromFile()` → pixel read |
| `items.lst` | SP-lines with x,z,height,diam | `CSPList::Load()` |

### Character Directory Structure (`data/char/<name>/`)
| File | Format | Loader |
|---|---|---|
| `preview.png` | PNG | `TTexture::Load()` |
| `shape.lst` | SP-lines: materials + node hierarchy | `CCharShape::Load()` |
| `start.lst`, `finish.lst`, etc. | SP-lines: keyframe animation | `CKeyframe::Load()` |

### Asset Manifest Formats
- `textures.lst`: `*[id] N [name] X [file] Y.png [repeat] 0|1`
- `fonts.lst`: `*[name] X [file] Y.ttf`
- `courses.lst`: `*[name] Bunny Hill [dir] bunny_hill`
- `groups.lst`: `*[dir] default`
- `characters.lst`: `*[name] Tux [type] spheres [dir] tux`
- `terrains.lst`: terrain type definitions with texture refs
- `object_types.lst`: object type definitions with texture refs
- `environment.lst`: `[location] etr [high_res] true`
- `languages.lst`: `*[lang] de_DE [language] Deutsch`

---

## 5. DISK I/O

### Read Operations

| Operation | File | I/O Method |
|---|---|---|
| All `.lst` files | `spx.cpp:366` `CSPList::Load()` | `std::ifstream` |
| PNG images | `textures.cpp:42`, `course.cpp:318,599,430` | `sf::Texture::loadFromFile()`, `sf::Image::loadFromFile()` |
| TTF fonts | `font.cpp:111` | `sf::Font::loadFromFile()` |
| WAV sounds | `audio.cpp:56` | `sf::SoundBuffer::loadFromFile()` |
| OGG music | `audio.cpp:159` | `sf::Music::openFromFile()` |
| Config file | `game_config.cpp:257` | `CSPList::Load()` |

### Write Operations

| Operation | File | I/O Method |
|---|---|---|
| `options.txt` | `game_config.cpp:130` `SaveConfigFile()` | `CSPList::Save()` via `std::ofstream` |
| `messages` log | `common.cpp:131` `SaveMessages()` | `CSPList::Save()` via `std::ofstream` |
| `items.lst` cache | `course.cpp:430` `LoadAndConvertObjectMap()` | `CSPList::Save()` (writes cached tree positions) |
| Screenshots | `winsys.cpp:133` `TakeScreenshot()` | `sf::Image::saveToFile()` → PNG |

### Write Paths (PS3 → `/dev_hdd0/`)
- `param.config_dir + "/options.txt"` — config save
- `param.config_dir + "/messages"` — log file
- `param.screenshot_dir + "/..."` — PNG screenshots
- Course cache: `CourseDir + "/items.lst"` — generated if missing

### File Utilities
| Function | File | Purpose |
|---|---|---|
| `MakePathStr()` | `spx.cpp:35` | `src + SEP + add` path concatenation |
| `FileExists()` | `common.cpp:167` | `stat()` check |
| `DirExists()` | `common.cpp:179` | `opendir()` (Unix) or `GetFileAttributesA` (Win32) |
| `CSPList::Load/Save()` | `spx.cpp:366` | Universal `.lst` file parser via `std::ifstream`/`std::ofstream` |

---

## 6. STARTUP SEQUENCE

```
main.cpp:
  1. InitConfig()           → game_config.cpp  — sets all param.* paths, loads options.txt
  2. Winsys.Init()          → winsys.cpp       — creates SFML window (1280×720)
  3. Tex.LoadTextureList()  → textures.cpp     — loads textures/textures.lst + all PNGs
  4. FT.LoadFontlist()      → font.cpp         — loads fonts/fonts.lst + all TTFs
  5. Music.LoadMusicList()  → audio.cpp        — loads music/music.lst + OGGs
  6. State::manager.Run(SplashScreen) → splash_screen.cpp
     → Loading state:
        Course.LoadCourseList()  → course.cpp   — loads courses/groups.lst + per-group courses.lst
        Course.LoadCourse()      → course.cpp   — loads elev.png, terrain.png, trees.png, items.lst
        Env.LoadEnvironment()    → env.cpp      — loads skybox PNGs + light.lst
        Char.LoadCharacterList() → game_ctrl.cpp — loads characters.lst + shape/keyframe .lst files
```

---

## 7. PLATFORM #ifdef SUMMARY

| File | Lines | ifdef | Purpose |
|---|---|---|---|
| `bh.h` | 34-48 | `_WIN32`, `_MSC_VER`, `__APPLE__`, `__linux__` | OS detection |
| `bh.h` | 50-73 | `OS_WIN32_MSC`, `OS_WIN32_MINGW`, else | GL extension headers |
| `bh.h` | 76 | `USE_STENCIL_BUFFER` | Always defined |
| `common.cpp` | 179-195 | `OS_WIN32_MSC` | `DirExists()` Win32 vs POSIX |
| `game_config.cpp` | 260-329 | `OS_WIN32_MINGW/MSC`, else | Path resolution |
| `translation.cpp` | 225-237 | `_WIN32` | System locale detection |
| `winsys.cpp` | 72-76 | `USE_STENCIL_BUFFER` | Stencil bits |
| `winsys.cpp` | 80-88 | `_WIN32` | Win32 window icon |
| `winsys.cpp` | 141-147 | `!OS_WIN32_*` | POSIX `mkdir` for screenshots |
| `ogl.cpp` | 362-374 | `USE_STENCIL_BUFFER` | Shadow rendering path |
| `tux.cpp` | 45-49 | `USE_STENCIL_BUFFER` | Shadow alpha |
| `env.cpp` | 202-208 | `OS_LINUX` | Skybox UV fix |

---

## 8. EXTERNAL LIBRARY SUMMARY

| Library | Linked As | Used For |
|---|---|---|
| SFML System | `-lsfml-system` | Clocks, threads, `sf::Context::getFunction()` |
| SFML Window | `-lsfml-window` | Window, events, joystick, GL context |
| SFML Graphics | `-lsfml-graphics` | 2D rendering, texture/font/sprite loading |
| SFML Audio | `-lsfml-audio` | WAV/OGG playback |
| OpenGL | `-lGL` / `-lOpenGL` | All 3D rendering (fixed-function pipeline) |
| GLU | `-lGLU` | `gluPerspective`, `gluErrorString` |

---

## 9. PS3 PORTING ABSTRACTION POINTS

**Already abstracted (minimal changes needed):**
- Input: `State::Jaxis()`/`Jbutt()` interface already gamepad-only
- Render mode stack: `TRenderMode` + `ScopedRenderMode` cleanly separates GL state
- Text file I/O: `CSPList` is the single chokepoint for all `.lst` files

**Need replacement layers:**
1. **SFML → PSGL + native APIs**: Window creation, event loop, GL context
2. **sf::Texture::loadFromFile → custom PNG loader** (e.g., stb_image): used in `textures.cpp`, `course.cpp`, `env.cpp`
3. **sf::Font/sf::Text → bitmap font or FreeType**: used in `font.cpp`, `gui.cpp`
4. **sf::Sound/sf::Music → cellAudio or libaudio**: used in `audio.cpp`
5. **gluPerspective → manual matrix**: used in `ogl.cpp:175`
6. **GL_TEXTURE_GEN_S/T → manual texcoord generation or PSGL equivalent**: used in `course_render.cpp`
7. **Path resolution → PS3-specific**: `/dev_hdd0/game/ETR08400/` for data, `/dev_hdd0/home/<user>/savedata/` for writes

---

## 10. SFML ABSTRACTION PLAN

### Goal
Create a platform abstraction layer so SFML can be swapped for native PS3 APIs without modifying any existing source files. PS3 types are defined inside `namespace sf {}` so all existing code compiles unchanged.

### Architecture: 3 New Files

#### `src/platform.h` — Conditional include dispatcher
```cpp
#ifndef PLATFORM_H
#define PLATFORM_H
#ifdef PS3
#include "platform_ps3.h"
#else
#include "platform_sfml.h"
#endif
#endif
```

#### `src/platform_sfml.h` — SFML includes (existing behavior)
```cpp
#ifndef PLATFORM_SFML_H
#define PLATFORM_SFML_H
#include <SFML/System.hpp>
#include <SFML/Window.hpp>
#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#endif
```

#### `src/platform_ps3.h` — PS3 class definitions in `namespace sf`
Defines minimal compatible types in `namespace sf` using PS3 native APIs. Each category below maps SFML types to PS3 equivalents.

### Per-Category Abstraction

#### Value Types (POD structs, no dependencies)
| SFML Type | PS3 Implementation | Used In |
|---|---|---|
| `sf::Color` | `struct Color { uint8_t r,g,b,a; }` + predefined constants | ~30+ files |
| `sf::Vector2<T>` | `struct Vector2<T> { T x, y; }` | gui, winsys |
| `sf::Vector2i` | `Vector2<int>` | winsys |
| `sf::Vector2u` | `Vector2<unsigned int>` | winsys |
| `sf::Rect<T>` | `struct Rect<T> { T left, top, width, height; }` | font, gui |
| `sf::IntRect` | `Rect<int>` | textures |
| `sf::FloatRect` | `Rect<float>` | font, gui |
| `sf::Time` | `struct Time { int64_t us; float asSeconds() const; }` | winsys |
| `sf::String` | `std::string` (already done) | — |

#### Timing (`sys_time_*` on PS3)
| SFML Type | PS3 Implementation | Used In |
|---|---|---|
| `sf::Clock` | Wraps `sys_time_get_current_time()`, `restart()` returns Time | winsys.cpp |

#### Window & Events (PSGL + cellPad)
| SFML Type | PS3 Implementation | Used In |
|---|---|---|
| `sf::RenderWindow` | PSGL context + `cellPadReadBufferPort()` polling | winsys |
| `sf::Event` | Union: `Closed`, `JoystickMoveEvent{axis,position}`, `JoystickButtonEvent{button}` | states.cpp |
| `sf::Joystick` | `cellPadInfo` wrapper: `isConnected()`, `getAxisCount()`, `getAxisPosition()`, `getButtonCount()`, `isButtonPressed()` | states.cpp, winsys.cpp |
| `sf::VideoMode` | `struct VideoMode { unsigned int width, height; }` | winsys.cpp |

#### Textures & Images (libpng + GL)
| SFML Type | PS3 Implementation | Used In |
|---|---|---|
| `sf::Texture` | Loads PNG via `png_read_image()` → `glTexImage2D()`. Stores GLuint handle + width/height. `getSize()` returns Vector2u. `loadFromFile()` parses PNG. `bind()` calls `glBindTexture()`. `getNativeHandle()` returns GLuint. | textures.cpp, course.cpp, gui.cpp, env.cpp |
| `sf::Image` | Raw RGBA pixel buffer. `loadFromFile()` loads PNG pixels. `getPixelsPtr()` returns uint8_t*. `create()` allocates buffer. `getSize()` returns Vector2u. | course.cpp (elev/terrain/trees maps), winsys.cpp (screenshots) |
| `sf::Sprite` | Stores texture ref + position + scale. `setPosition()`, `setScale()`, `getTextureRect()`, `getLocalBounds()`. `draw()` uses GL quads with texture. | gui.cpp, splash_screen.cpp |
| `sf::RectangleShape` | Filled/outlined rect. `setSize()`, `setPosition()`, `setFillColor()`, `setOutlineColor()`, `setOutlineThickness()`. `draw()` uses GL quads. | gui.cpp |

#### Fonts & Text (FreeType texture atlas)
| SFML Type | PS3 Implementation | Used In |
|---|---|---|
| `sf::Font` | FreeType2 `FT_Face`. `loadFromFile()` opens TTF. Stores glyph texture atlas (single GL texture with all cached glyphs). `getGlyph()` returns UV rects. | font.cpp |
| `sf::Text` | String + font ref + size + color. `setString()`, `setFont()`, `setCharacterSize()`, `setFillColor()`, `setOutlineColor()`, `setPosition()`, `getLocalBounds()`, `getGlobalBounds()`. `draw()` renders quads per glyph using font atlas. | font.cpp, gui.cpp, game_over.cpp, splash_screen.cpp |

#### Audio (cellAudio / libmstream)
| SFML Type | PS3 Implementation | Used In |
|---|---|---|
| `sf::SoundBuffer` | Loads WAV via header parse → PCM data in memory. `loadFromFile()` parses WAV. `getSampleCount()`, `getSampleRate()`, `getChannelCount()`, `getSamples()`. | audio.cpp |
| `sf::Sound` | Plays SoundBuffer via cellAudio multi-track. `setBuffer()`, `setVolume()`, `play()`, `stop()`, `getStatus()`. | audio.cpp |
| `sf::Music` | Streams OGG via libvorbisfile → cellAudio. `openFromFile()`, `setVolume()`, `play()`, `stop()`, `setLoop()`, `getStatus()`, `getDuration()`. | audio.cpp |

### Single Change to Existing Code

**`src/bh.h`** — Replace SFML includes with platform dispatcher:
```cpp
// Remove these 3 lines:
// #include <SFML/System.hpp>
// #include <SFML/Window.hpp>
// #include <SFML/Graphics.hpp>

// Add this one line:
#include "platform.h"
```

Also add `OS_PS3` platform detection:
```cpp
#ifdef __PS3__
#define OS_PS3
#endif
```

### Implementation Order

1. **platform.h** — 6-line conditional include (trivial)
2. **platform_sfml.h** — Move SFML includes from bh.h (trivial)
3. **platform_ps3.h value types** — Color, Vector2, Rect, Time as POD structs (no dependencies)
4. **platform_ps3.h Clock** — Wrap `sys_time_get_current_time()`
5. **platform_ps3.h Texture/Image** — PNG loading via stb_image or libpng + GL texture creation
6. **platform_ps3.h Font/Text** — FreeType2 glyph atlas + GL quad rendering
7. **platform_ps3.h Sprite/RectangleShape** — GL immediate-mode quads
8. **platform_ps3.h RenderWindow** — PSGL context creation + cellPad event polling
9. **platform_ps3.h Event/Joystick** — cellPad → sf::Event translation
10. **platform_ps3.h Audio** — cellAudio + libvorbisfile for OGG streaming

### Files Modified
- `src/bh.h` — Replace 3 SFML includes with `#include "platform.h"`, add `OS_PS3` define

### Files Created
- `src/platform.h` — Conditional include dispatcher
- `src/platform_sfml.h` — SFML includes wrapper
- `src/platform_ps3.h` — All PS3-compatible sf:: type definitions

### Verification
1. **SFML build still works**: Compile with no PS3 define → `platform_sfml.h` included → identical to current behavior
2. **PS3 types compile**: Define PS3 → `platform_ps3.h` included → all sf:: types resolve
3. **Zero source changes**: No `.cpp` files modified except bh.h; all existing `sf::` usage compiles against either header
4. **Link test**: PS3 build links against PSGL, cellAudio, libpng, FreeType2 instead of SFML
