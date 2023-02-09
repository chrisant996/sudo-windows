-- Copyright (c) 2022 Christopher Antos
-- Portions Copyright (c) 2012 Martin Ridgers
-- License: http://opensource.org/licenses/MIT

local to = ".build/"..(_ACTION or "nullaction")

if _ACTION == "gmake2" then
    error("Use `premake5 gmake` instead; gmake2 neglects to link resources.")
end



--------------------------------------------------------------------------------
local function get_git_info()
    local git_cmd = "git branch --verbose --no-color 2>nul"
    for line in io.popen(git_cmd):lines() do
        local _, _, branch, commit = line:find("^%*.+%s+([^ )]+)%)%s+([a-f0-9]+)%s")
        if branch and commit then
            return branch, commit:sub(1, 6)
        end

        local _, _, branch, commit = line:find("^%*%s+([^ ]+)%s+([a-f0-9]+)%s")
        if branch and commit then
            return branch, commit:sub(1, 6)
        end
    end
end

--------------------------------------------------------------------------------
-- MinGW's windres tool can't seem to handle string concatenation like rc does,
-- so I gave up and generate it here.
local function get_version_info(commit, def_prefix, version_file_name)
    def_prefix = def_prefix or ""
    version_file_name = version_file_name or "version.h"

    local maj, min, pat
    local x
    for line in io.lines(version_file_name) do
        x = line:match(def_prefix.."VERSION_MAJOR[ \t]+([0-9]+)")
        if x then maj = x end
        x = line:match(def_prefix.."VERSION_MINOR[ \t]+([0-9]+)")
        if x then min = x end
        x = line:match(def_prefix.."VERSION_PATCH[ \t]+([0-9]+)")
        if x then pat = x end
    end

    if not maj or not min or not pat then
        error("Unable to find version number in '"..version_file_name.."'.")
    end

    commit = commit and '.'..commit or ''

    local str = '#define '..def_prefix..'VERSION_STR "'..maj..'.'..min..'.'..pat..commit..'"\n'
    local lstr = '#define '..def_prefix..'VERSION_LSTR L"'..maj..'.'..min..'.'..pat..commit..'"\n'
    return str..lstr
end

--------------------------------------------------------------------------------
local function write_commit_file(commit, def_prefix)
    local version_info = get_version_info(commit, def_prefix)
    commit = commit or ""
    def_prefix = def_prefix or ""

    local commit_file
    local commit_file_name = ".build/commit_file.h"
    local new_commit_string = "#pragma once\n#define "..def_prefix.."COMMIT_HASH "..commit.."\n"..version_info
    local old_commit_string = ""

    commit_file = io.open(path.getabsolute(commit_file_name), "r")
    if commit_file then
        old_commit_string = commit_file:read("*all")
        commit_file:close()
    end

    if old_commit_string ~= new_commit_string then
        commit_file = io.open(path.getabsolute(commit_file_name), "w")
        if not commit_file then
            error("Unable to write '"..commit_file_name.."'.")
        end
        commit_file:write(new_commit_string)
        commit_file:close()
        print("Generated "..commit_file_name.."...")
    end
end

--------------------------------------------------------------------------------
if _ACTION and (_ACTION:find("^vs") or _ACTION:find("^gmake")) then
    branch, commit = get_git_info()
    write_commit_file(commit, "SUDO_")
end



--------------------------------------------------------------------------------
local function setup_cfg(cfg)
    configuration(cfg)
        defines("BUILD_"..cfg:upper())
        targetdir(to.."/bin/"..cfg)
        objdir(to.."/obj/")
end



--------------------------------------------------------------------------------
local function define_project(name)
    project(name)
    flags("fatalwarnings")
    language("c++")
end

--------------------------------------------------------------------------------
local function define_lib(name)
    define_project(name)
    kind("staticlib")
end

local function define_dll(name)
    define_project(name)
    kind("sharedlib")
end

--------------------------------------------------------------------------------
local function define_exe(name, exekind)
    define_project(name)
    kind(exekind or "consoleapp")
end



--------------------------------------------------------------------------------
workspace("sudo")
    configurations({"debug", "release"})
    platforms({"x32", "x64"})
    location(to)

    characterset("Unicode")
    flags("NoManifest")
    staticruntime("on")
    symbols("on")
    exceptionhandling("off")

    setup_cfg("release")
    setup_cfg("debug")

    configuration("debug")
        rtti("on")
        optimize("off")
        defines("DEBUG")
        defines("_DEBUG")

    configuration("release")
        rtti("off")
        optimize("full")
        omitframepointer("on")
        defines("NDEBUG")

    configuration({"release", "vs*"})
        flags("LinkTimeOptimization")

    configuration("vs*")
        defines("_HAS_EXCEPTIONS=0")
        --defines("_CRT_SECURE_NO_WARNINGS")
        --defines("_CRT_NONSTDC_NO_WARNINGS")

    configuration("gmake")
        defines("__MSVCRT_VERSION__=0x0601")
        defines("_WIN32_WINNT=0x0601")
        defines("WINVER=0x0601")
        defines("_POSIX=1")             -- so vsnprintf returns needed size
        buildoptions("-Wno-error=missing-field-initializers")
        buildoptions("-ffunction-sections")
        buildoptions("-fdata-sections")
        makesettings { "CC=gcc" }

    configuration("*")
        includedirs(".build")           -- for commit_file.h

--------------------------------------------------------------------------------
define_exe("sudo")
    targetname("sudo")
    files("main.cpp")
    files("version.rc")

    configuration("vs*")
        defines("_HAS_EXCEPTIONS=0")
        defines("_CRT_SECURE_NO_WARNINGS")
        defines("_CRT_NONSTDC_NO_WARNINGS")

    configuration("gmake")
        buildoptions("-fpermissive")
        buildoptions("-std=c++17")
        linkgroups("on")

