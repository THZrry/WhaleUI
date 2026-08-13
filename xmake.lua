-- WhaleUI: xmake build script
-- C++14, minimal deps, C API exposed.
-- Toolchain: MinGW (Windows) / clang (macOS) / gcc (Linux)

set_project("whaleui")
set_version("0.1.0")
set_languages("c++14")

-- third-party deps (xrepo)
-- render: SDL3; layout: lexbor; charset: utf8proc; tasks: std::thread (libco unsupported on mingw)
add_requires("libsdl3")
add_requires("lexbor")
add_requires("stb")
add_requires("utf8proc")
add_requires("libsdl3_image", {configs = {shared = false}})

-- SDL3_ttf: on windows/mingw use the official prebuilt mingw devel package
-- under 3rdparty/sdl3_ttf (downloaded from
-- https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-devel-3.2.2-mingw.zip,
-- include kept in git, bin/lib gitignored). Building from source on a windows
-- host needs MSVC (pkgconf->meson tool chain), so prebuilt wins here.
-- On other platforms build from xrepo source.
if not is_plat("windows", "mingw") then
    add_requires("libsdl3_ttf", {configs = {shared = false}})
end

-- audio/video placeholders: not implemented yet (commented out)
-- add_requires("libsdl3_mixer")

set_policy("build.across_targets_in_parallel", true)

-- Full: everything on, SDL image/font etc.
target("whaleui-full")
    set_kind("static")
    add_defines("WHALEUI_BUILD_FULL")
    add_packages("libsdl3", "libsdl3_image", "lexbor", "stb", "utf8proc")
    add_files("src/**.cpp")
    add_includedirs("include")
    on_load(function (target)
        if is_plat("windows", "mingw") then
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
    add_includedirs("include")

-- Minimal: layout only, no HTML parsing/cache, stb resources
target("whaleui-minimal")
    set_kind("static")
    add_defines("WHALEUI_BUILD_MINIMAL")
    add_packages("libsdl3", "stb")
    add_files("src/**.cpp")
    add_includedirs("include")
