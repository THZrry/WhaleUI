-- WhaleUI: xmake build script
-- C++14, minimal deps, C API exposed.
-- Toolchain: MinGW (Windows) / clang (macOS) / gcc (Linux)

set_project("whaleui")
set_version("0.1.0")
set_languages("c++14")

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

-- --- always source-built via xrepo ---
add_requires("libsdl3")
add_requires("lexbor")
add_requires("stb")
add_requires("utf8proc")
add_requires("libsdl3_image", {configs = {shared = false}})

-- audio/video placeholders: not implemented yet (commented out)
-- add_requires("libsdl3_mixer")

set_policy("build.across_targets_in_parallel", true)

-- Full: everything on, SDL image/font etc.
target("whaleui-full")
    set_kind("static")
    add_defines("WHALEUI_BUILD_FULL")
    add_packages("libsdl3", "libsdl3_image", "lexbor", "stb", "utf8proc")
    add_files("src/**.cpp")
    add_includedirs("include", "src")
    on_load(function (target)
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
    add_packages("libsdl3", "lexbor", "stb", "utf8proc")
    add_files("src/**.cpp")
    add_includedirs("include", "src")

-- Minimal: layout only, no HTML parsing/cache, stb resources
target("whaleui-minimal")
    set_kind("static")
    add_defines("WHALEUI_BUILD_MINIMAL")
    add_packages("libsdl3", "stb")
    -- no HTML parsing in minimal: dom.cpp needs lexbor, exclude it
    add_files("src/core/**.cpp", "src/style/**.cpp", "src/layout/**.cpp",
              "src/render/**.cpp", "src/fs/**.cpp", "src/font/**.cpp",
              "src/platform/**.cpp")
    add_includedirs("include", "src")

-- ======================= tests =======================
-- Zero-dependency assert-style unit tests; each links whaleui-full.
-- Run with:  xmake run test_api   (from the repo root, so file:// URIs in
-- tests resolve under tests/data/).

for _, name in ipairs({"test_api", "test_fs", "test_font", "test_dom", "test_style", "test_window"}) do
    target(name)
        set_kind("binary")
        add_files("tests/" .. name .. ".cpp")
        add_deps("whaleui-full")
        add_packages("libsdl3", "libsdl3_image", "lexbor", "stb", "utf8proc")
        add_includedirs("include", "src")
        add_defines("WHALEUI_BUILD_FULL")
        -- absolute repo root so file:// URIs work regardless of cwd
        add_defines('WHALEUI_TEST_ROOT="' .. os.projectdir():gsub("\\", "/") .. '"')
        set_default(true)
end

