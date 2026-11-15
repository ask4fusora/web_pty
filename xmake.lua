set_project("web_pty")
set_version("0.1.0")
set_xmakever("2.9.8") -- TODO: Remove/Change this.

add_rules("mode.debug", "mode.release")
set_languages("c++26")
set_warnings("all", "extra")
set_policy("build.c++.modules", true)

target("wpty-lib")
    set_kind("$(kind)")
    set_basename("wpty")
    add_headerfiles("wpty.h")
    add_files("pty_process.cppm", "ws_server.cppm", {public = true})
    add_files("c_api.cpp")
    if is_kind("shared") and is_plat("windows") then
        add_defines("WPTY_SHARED_BUILD")
    end
    if is_plat("windows") then
        add_syslinks("ws2_32", "kernel32", "user32")
        add_defines("_WIN32_WINNT=0x0A00", "WIN32_LEAN_AND_MEAN", "NOMINMAX")
    else
        add_syslinks("pthread", "util")
    end

target("wpty")
    set_kind("binary")
    add_deps("wpty-lib")
    add_files("main.cpp")
    if is_plat("windows") then
        add_syslinks("ws2_32", "kernel32", "user32")
        add_defines("_WIN32_WINNT=0x0A00", "WIN32_LEAN_AND_MEAN", "NOMINMAX")
    else
        add_syslinks("pthread", "util")
    end
