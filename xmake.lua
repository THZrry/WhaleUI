-- WhaleUI: xmake build script
-- C++14, minimal deps, C API exposed.
-- Toolchain: MinGW (Windows) / clang (macOS) / gcc (Linux)

set_project("whaleui")
set_version("0.96")
set_languages("c++14")

-- Release must actually optimize. xmake leaves the compiler at its default
-- (-O0 for gcc/clang) unless told otherwise, and an unoptimized engine is
-- several times slower on exactly the loops that matter: byte-wise hashing of
-- long text runs (a 292k-char run's FNV measured 3.85ms at -O0 vs 0.51ms at
-- -O2), per-pixel glyph blits, and the layout walk. Debug keeps -O0 on
-- purpose, for debuggability.
if is_mode("release") then
    set_optimize("fastest")
end

-- ============================================================
-- Third-party strategy (per README: link, don't vendor; keep only
-- include/ in git, ignore binaries).
--
-- Two ways to consume each dep:
--   A. Prebuilt: user downloads the official prebuilt package and drops it
--      under 3rdparty/<name>/  (see tools/fetch-3rdparty.ps1). xmake detects
--      it and links directly - no toolchain needed for that dep.
--   B. Source: xmake pulls the package from xrepo and builds it.
--
-- Priority is A > B. On windows/mingw, SDL3_ttf has no working source build
-- (its pkgconf->meson dep chain hard-requires MSVC), so A is mandatory there
-- and the script errors out with a clear message if the prebuilt is missing.
--
-- Build variants:
--   Full    - everything on (SDL_image/SDL3_ttf), NO stb.
--   Lite    - feature-complete trimmed build, stb resource management.
--   Minimal - no SDL_image, no filter (backdrop/box-shadow blur) rendering,
--             stb resources; keeps layout/text/color/animation.
--   Tests   - off by default (also in release); enable with: xmake f --tests=y
-- ============================================================

-- --- SDL3_ttf (font rendering, Full target only) ---
local sdl3_ttf_prebuilt = os.isdir("3rdparty/sdl3_ttf/lib")
if sdl3_ttf_prebuilt then
    -- mode A: prebuilt, linked manually in whaleui-full below
elseif not is_plat("windows", "mingw") then
    -- mode B: source build via xrepo
    add_requires("libsdl3_ttf", {configs = {shared = false}})
else
    raise("SDL3_ttf prebuilt missing on mingw. Run tools/fetch-3rdparty.ps1 or manually place the official SDL3_ttf mingw devel package under 3rdparty/sdl3_ttf/")
end

-- --- SDL3: prefer the official prebuilt mingw package (3rdparty/sdl3,
-- downloaded by tools/fetch-3rdparty.ps1) - it ships SDL3.dll which the
-- prebuilt SDL3_ttf.dll imports at runtime. Fall back to a shared xrepo
-- source build when the prebuilt is missing.
local sdl3_prebuilt = os.isdir("3rdparty/sdl3/lib")
if not sdl3_prebuilt then
    add_requires("libsdl3", {configs = {shared = true}})
end

-- --- always source-built via xrepo ---
add_requires("lexbor")
add_requires("stb")
add_requires("utf8proc")
add_requires("libsdl3_image", {configs = {shared = false}})

-- audio/video placeholders: not implemented yet (commented out)
-- add_requires("libsdl3_mixer")

set_policy("build.across_targets_in_parallel", true)

-- Unit tests are opt-in: off by default (also in release), so a plain
-- `xmake` builds only the three libraries + demo. Enable with:
--   xmake f --tests=y
option("tests")
    set_default(false)
    set_showmenu(true)
    set_description("Build and run unit tests (default: off, also in release)")

-- attach SDL3 to a target: prebuilt or xrepo
local function use_sdl3(t)
    t:add("defines", "SDL_DISABLE_OLD_NAMES")
    t:add("includedirs", "3rdparty/dxc/include") -- dxcapi.h (HLSL -> DXIL)
    if sdl3_prebuilt then
        t:add("includedirs", "3rdparty/sdl3/include")
        t:add("linkdirs", "3rdparty/sdl3/lib")
        t:add("links", "SDL3")
    else
        t:add("packages", "libsdl3")
    end
end

-- Full: everything on, SDL image/font etc. (no stb)
target("whaleui-full")
    set_kind("static")
    add_defines("WHALEUI_BUILD_FULL")
    add_packages("libsdl3_image", "lexbor", "utf8proc")
    add_files("src/**.cpp")
    add_includedirs("include", "src")
    on_load(function (target)
        use_sdl3(target)
        if sdl3_ttf_prebuilt then
            target:add("includedirs", "3rdparty/sdl3_ttf/include")
            target:add("linkdirs", "3rdparty/sdl3_ttf/lib")
            target:add("links", "SDL3_ttf")
        else
            target:add("packages", "libsdl3_ttf")
        end
    end)

-- Lite: trimmed full build, stb resource management (no SDL image/font)
target("whaleui-lite")
    set_kind("static")
    add_defines("WHALEUI_BUILD_LITE")
    add_packages("lexbor", "stb", "utf8proc")
    add_files("src/**.cpp")
    add_includedirs("include", "src")
    on_load(function (target)
        use_sdl3(target)
    end)

-- Minimal: layout/text/color/animation only. No SDL image/font, no filter
-- rendering (backdrop-filter / box-shadow blur, see WHALEUI_BUILD_MINIMAL),
-- stb resources. lexbor stays: the layout engine walks a lexbor DOM tree.
target("whaleui-minimal")
    set_kind("static")
    add_defines("WHALEUI_BUILD_MINIMAL")
    add_packages("lexbor", "stb")
    add_files("src/**.cpp")
    add_includedirs("include", "src")
    on_load(function (target)
        use_sdl3(target)
    end)

-- ======================= tests =======================
-- Zero-dependency assert-style unit tests; each links whaleui-full.
-- Run with:  xmake run test_api   (from the repo root, so file:// URIs in
-- tests resolve under tests/data/).

for _, name in ipairs({"test_api", "test_fs", "test_font", "test_dom", "test_style", "test_anim", "test_layout", "test_render", "test_window", "test_scroll"}) do
    target(name)
        set_kind("binary")
        if is_mode("release") then
            set_strip("all") -- release: drop symbols (demo 5.4MB -> 2.9MB)
        end
        add_files("tests/" .. name .. ".cpp")
        add_deps("whaleui-full")
        add_packages("libsdl3_image", "lexbor", "stb", "utf8proc")
        add_includedirs("include", "src")
        add_defines("WHALEUI_BUILD_FULL")
        -- absolute repo root so file:// URIs work regardless of cwd
        add_defines('WHALEUI_TEST_ROOT="' .. os.projectdir():gsub("\\", "/") .. '"')
        -- tests are opt-in: off by default (and in release), on with --tests=y
        set_default(has_config("tests"))
        on_load(function (target)
            use_sdl3(target)
            if sdl3_ttf_prebuilt then
                target:add("includedirs", "3rdparty/sdl3_ttf/include")
                target:add("linkdirs", "3rdparty/sdl3_ttf/lib")
                target:add("links", "SDL3_ttf")
            else
                target:add("packages", "libsdl3_ttf")
            end
        end)
        target_end()
end

-- ship the SDL3_ttf + SDL3 runtime dlls next to every binary (tests + demo)
after_build(function (target)
    if target:kind() == "binary" then
        local ttfdll = os.projectdir() .. "/3rdparty/sdl3_ttf/bin/SDL3_ttf.dll"
        if os.isfile(ttfdll) then
            os.cp(ttfdll, target:targetdir())
        end
        local sdl3dll = os.projectdir() .. "/3rdparty/sdl3/bin/SDL3.dll"
        if os.isfile(sdl3dll) then
            os.cp(sdl3dll, target:targetdir())
        end
        local dxcdll = os.projectdir() .. "/3rdparty/dxc/bin/dxcompiler.dll"
        if os.isfile(dxcdll) then
            os.cp(dxcdll, target:targetdir())
        end
        local dxildll = os.projectdir() .. "/3rdparty/dxc/bin/dxil.dll"
        if os.isfile(dxildll) then
            os.cp(dxildll, target:targetdir())
        end
        -- runtime CJK dictionary (word segmentation), same search as the
        -- loader: exe dir first, then ./res relative to the cwd
        local dict = os.projectdir() .. "/res/whaleui_dict.bin"
        if os.isfile(dict) then
            os.cp(dict, target:targetdir() .. "/whaleui_dict.bin")
        end
    end
end)

-- ======================= demo =======================
target("demo")
    set_kind("binary")
    if is_mode("release") then
        set_strip("all") -- release: drop symbols (demo 5.4MB -> 2.9MB)
    end
    add_files("examples/demo.cpp")
    add_deps("whaleui-full")
    add_packages("libsdl3_image", "lexbor", "stb", "utf8proc")
    add_includedirs("include", "src")
    add_defines("WHALEUI_BUILD_FULL")
    on_load(function (target)
        use_sdl3(target)
    end)
target_end()

-- ======================= hover 布局 diff 探针（临时） ======================
-- 与 render_shot 同样只在 temp/probe_hover.cpp 存在时定义。
--   Build:  xmake b -y probe_hover
--   Run:    probe_hover [uri] [theme]
if os.isfile("temp/probe_hover.cpp") then
    target("probe_hover")
        set_kind("binary")
        if is_mode("release") then
            set_strip("all")
        end
        add_files("temp/probe_hover.cpp")
        add_deps("whaleui-full")
        add_packages("libsdl3_image", "lexbor", "stb", "utf8proc")
        add_includedirs("include", "src")
        add_defines("WHALEUI_BUILD_FULL")
        on_load(function (target)
            use_sdl3(target)
            if sdl3_ttf_prebuilt then
                target:add("includedirs", "3rdparty/sdl3_ttf/include")
                target:add("linkdirs", "3rdparty/sdl3_ttf/lib")
                target:add("links", "SDL3_ttf")
            else
                target:add("packages", "libsdl3_ttf")
            end
        end)
    target_end()
end

-- ======================= anim frame-CPU benchmark (temporary) ==========
-- gitignored tool: measures per-frame whaleui_render_frame CPU cost with
-- demo-like animations. Only defined when temp/bench_anim.cpp exists.
--   Build:  xmake b -y bench_anim
--   Run:    bench_anim [full|pulse|bar|static] [frames]
if os.isfile("temp/bench_anim.cpp") then
    target("bench_anim")
        set_kind("binary")
        if is_mode("release") then
            set_strip("all")
        end
        add_files("temp/bench_anim.cpp")
        add_deps("whaleui-full")
        add_packages("libsdl3_image", "lexbor", "stb", "utf8proc")
        add_includedirs("include", "src")
        add_defines("WHALEUI_BUILD_FULL")
        on_load(function (target)
            use_sdl3(target)
            if sdl3_ttf_prebuilt then
                target:add("includedirs", "3rdparty/sdl3_ttf/include")
                target:add("linkdirs", "3rdparty/sdl3_ttf/lib")
                target:add("links", "SDL3_ttf")
            else
                target:add("packages", "libsdl3_ttf")
            end
        end)
    target_end()
end
-- ======================= temp verification target (optional) ==============
-- Offscreen renderer + colour probes used to verify the theme stylesheets
-- against the reference screenshots: it renders a page to a PPM without a GPU
-- window. The tool itself lives in temp/ (gitignored), so the target is only
-- defined when that file is present - a fresh checkout builds unaffected.
--   Build:  xmake b -y render_shot
--   Run:    render_shot <uri> <out.ppm> <style> <light|dark> <h> [dump] <frames> [delay_ms]
if os.isfile("temp/render_shot.cpp") then
    target("render_shot")
        set_kind("binary")
        if is_mode("release") then
            set_strip("all")
        end
        add_files("temp/render_shot.cpp")
        add_deps("whaleui-full")
        add_packages("libsdl3_image", "lexbor", "stb", "utf8proc")
        add_includedirs("include", "src")
        add_defines("WHALEUI_BUILD_FULL")
        on_load(function (target)
            use_sdl3(target)
            if sdl3_ttf_prebuilt then
                target:add("includedirs", "3rdparty/sdl3_ttf/include")
                target:add("linkdirs", "3rdparty/sdl3_ttf/lib")
                target:add("links", "SDL3_ttf")
            else
                target:add("packages", "libsdl3_ttf")
            end
        end)
    target_end()
end


