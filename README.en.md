# WhaleUI

A lightweight HTML/CSS rendering engine and GUI toolkit. Written in C++14,
exposed through a plain C API for reuse across languages and platforms.
Built for small, fast desktop tool UIs.

## Features

- **HTML + CSS**: DOM parsing via [lexbor](https://github.com/lexbor/lexbor);
  a self-contained CSS parser and style engine with selector matching
  (tag / `#id` / `.class` / descendant / child), cascade (specificity +
  `!important`), `var()` custom properties, `@media` and `@keyframes`.
- **Layout**: own box-model + basic flex engine - margin / padding / border /
  content, `content-box` / `border-box`, `px` / `%` / `em` / `auto` lengths,
  block flow, flex (`direction` / `justify-content` / `gap` / `flex-grow`),
  `position` (static / relative / absolute / fixed), `z-index`, `opacity`,
  `display: none`.
- **Rendering**: SDL3 GPU (offscreen texture + blit to the swapchain;
  D3D11 / Vulkan / OpenGL backends chosen automatically by SDL). Text is
  rasterized by SDL3_ttf from fonts registered through the virtual file
  system.
- **Themes**: built-in default stylesheet (presets for every tag, shared
  utility classes), light / dark variable sets, system-following or manual,
  hot-switchable.
- **Virtual file system**: all resource loading (HTML / CSS / fonts) goes
  through one VFS; the default disk loader can be replaced entirely
  (e.g. HTTP CDN).
- **Three build targets**: `full` (SDL3 + SDL_Image + SDL3_ttf + lexbor +
  stb + utf8proc), `lite` (no SDL_Image / SDL3_ttf), `minimal` (layout and
  render core only).

## Building

Requirements: [xmake](https://xmake.io) and MinGW-w64 (gcc ≥ 14). Third-party
libraries come from [xrepo](https://xrepo.xmake.io); `SDL3` and `SDL3_ttf`
prefer official prebuilt packages (see below).

```bash
xmake            # build everything (static libs + tests + demo)
xmake build demo # demo only
xmake run demo   # run the demo (T toggles light/dark, ESC quits)
xmake run test_dom   # run one test (run from the repo root; file:// URIs
                     # are relative to the working directory)
```

On Windows, fetch the prebuilt third-party packages first:

```powershell
.\tools\fetch-3rdparty.ps1   # downloads SDL3_ttf / SDL3 mingw devel packages
```

Outputs go to `build/<plat>/<arch>/release/`; `SDL3.dll` and `SDL3_ttf.dll`
are copied next to each binary automatically.

### Tests

```bash
xmake build
xmake run test_api test_fs test_font test_dom test_style test_layout test_render test_window
```

All tests are framework-free `assert` style unit tests; exit code 0 means pass.

## Quick start

```c
#include "whaleui.h"

int main(void) {
    whaleui_app_t* app = whaleui_app_create();

    whaleui_window_t* win = whaleui_window_create(app, "Hello", 640, 480);
    whaleui_window_load_html(win,
        "<html><body><div class=\"card\">"
        "<h1>Hello, WhaleUI</h1>"
        "<p>Press ESC to quit.</p>"
        "</div></body></html>");
    whaleui_window_show(win);

    whaleui_app_run(app);

    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 0;
}
```

## Layout

```
include/whaleui.h       # the only public header: all whaleui_* C API
src/
  core/                 # app / window / event loop / theme
  dom/                  # DOM (wraps lexbor)
  style/                # CSS parsing, style engine, default theme
  layout/               # layout engine
  render/               # SDL3 rendering (GPU + software fallback)
  fs/                   # virtual file system
  font/                 # font registry
  platform/             # platform backends (Windows done; Linux/macOS TODO)
doc/                    # design docs
tests/                  # unit tests
examples/demo.cpp       # demo
```

## Docs

- [README-css.md](README-css.md) - CSS property support list and priority
- [doc/00-structure.md](doc/00-structure.md) - project structure
- [doc/01-architecture.md](doc/01-architecture.md) - architecture
- [doc/09-implementation.md](doc/09-implementation.md) - implementation
  status and known limitations (in Chinese)
- Per-module API manuals: `doc/02-*.md` .. `doc/07-*.md`

## Platform support

| Platform | Status |
|----------|--------|
| Windows | ✅ supported (SDL3 window, registry light/dark detection, system font dirs) |
| Linux (Wayland/X11) | 🚧 SDL3 is cross-platform; platform backend TODO |
| macOS | 🚧 same |
| Android / iOS | 🔲 reserved |

## License

MIT (third-party deps under their own licenses, see `3rdparty/*/share/licenses`).
