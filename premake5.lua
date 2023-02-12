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
        targetdir(to.."/bin/%{cfg.buildcfg}/%{cfg.platform}")
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

--------------------------------------------------------------------------------
local any_warnings_or_failures = nil

--------------------------------------------------------------------------------
local release_manifest = {
    "sudo.exe",
}

--------------------------------------------------------------------------------
local function warn(msg)
    print("\x1b[0;33;1mWARNING: " .. msg.."\x1b[m")
    any_warnings_or_failures = true
end

--------------------------------------------------------------------------------
local function failed(msg)
    print("\x1b[0;31;1mFAILED: " .. msg.."\x1b[m")
    any_warnings_or_failures = true
end

--------------------------------------------------------------------------------
local exec_lead = "\n"
local function exec(cmd, silent)
    print(exec_lead .. "## EXEC: " .. cmd)

    if silent then
        cmd = "1>nul 2>nul "..cmd
    else
        -- cmd = "1>nul "..cmd
    end

    -- Premake replaces os.execute() with a version that runs path.normalize()
    -- which converts \ to /. This is fine for everything except cmd.exe.
    local prev_norm = path.normalize
    path.normalize = function (x) return x end
    local _, _, ret = os.execute(cmd)
    path.normalize = prev_norm

    return ret == 0
end

--------------------------------------------------------------------------------
local function exec_with_retry(cmd, tries, delay, silent)
    while tries > 0 do
        if exec(cmd, silent) then
            return true
        end

        tries = tries - 1

        if tries > 0 then
            print("... waiting to retry ...")
            local target = os.clock() + delay
            while os.clock() < target do
                -- Busy wait, but this is such a rare case that it's not worth
                -- trying to be more efficient.
            end
        end
    end

    return false
end

--------------------------------------------------------------------------------
local function mkdir(dir)
    if os.isdir(dir) then
        return
    end

    local ret = exec("md " .. path.translate(dir), true)
    if not ret then
        error("Failed to create directory '" .. dir .. "' ("..tostring(ret)..")", 2)
    end
end

--------------------------------------------------------------------------------
local function rmdir(dir)
    if not os.isdir(dir) then
        return
    end

    return exec("rd /q /s " .. path.translate(dir), true)
end

--------------------------------------------------------------------------------
local function unlink(file)
    return exec("del /q " .. path.translate(file), true)
end

--------------------------------------------------------------------------------
local function copy(src, dest)
    src = path.translate(src)
    dest = path.translate(dest)
    return exec("copy /y " .. src .. " " .. dest, true)
end

--------------------------------------------------------------------------------
local function rename(src, dest)
    src = path.translate(src)
    return exec("ren " .. src .. " " .. dest, true)
end

--------------------------------------------------------------------------------
local function file_exists(name)
    local f = io.open(name, "r")
    if f ~= nil then
        io.close(f)
        return true
    end
    return false
end

--------------------------------------------------------------------------------
local function have_required_tool(name, fallback)
    if exec("where " .. name, true) then
        return name
    end

    if fallback then
        local t
        if type(fallback) == "table" then
            t = fallback
        else
            t = { fallback }
        end
        for _,dir in ipairs(t) do
            local file = dir .. "\\" .. name .. ".exe"
            if file_exists(file) then
                return '"' .. file .. '"'
            end
        end
    end

    return nil
end

--------------------------------------------------------------------------------
newaction {
    trigger = "release",
    description = "Creates a release of sudo-windows",
    execute = function ()
        local premake = _PREMAKE_COMMAND
        local root_dir = path.getabsolute(".build/release") .. "/"

        -- Check we have the tools we need.
        local have_msbuild = have_required_tool("msbuild", { "c:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Enterprise\\MSBuild\\Current\\Bin", "c:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Enterprise\\MSBuild\\Current\\Bin" })
        local have_7z = have_required_tool("7z", { "c:\\Program Files\\7-Zip", "c:\\Program Files (x86)\\7-Zip" })

        -- Clone repo in release folder and checkout the specified version
        local code_dir = root_dir .. "~working/"
        rmdir(code_dir)
        mkdir(code_dir)

        exec("git clone . " .. code_dir)
        if not os.chdir(code_dir) then
            error("Failed to chdir to '" .. code_dir .. "'")
        end
        exec("git checkout " .. (_OPTIONS["commit"] or "HEAD"))

        -- Build the code.
        local x86_ok = true
        local x64_ok = true
        local arm64_ok = true
        local toolchain = "ERROR"
        local build_code = function (target)
            if have_msbuild then
                target = target or "build"

                toolchain = _OPTIONS["vsver"] or "vs2019"
                exec(premake .. " " .. toolchain)
                os.chdir(".build/" .. toolchain)

                x86_ok = exec(have_msbuild .. " /m /v:q /p:configuration=release /p:platform=win32 sudo.sln /t:" .. target)
                x64_ok = exec(have_msbuild .. " /m /v:q /p:configuration=release /p:platform=x64 sudo.sln /t:" .. target)

                os.chdir("../..")
            else
                error("Unable to locate msbuild.exe")
            end
        end

        -- Build everything.
        build_code()

        local src = path.getabsolute(".build/" .. toolchain .. "/bin/release").."/"

        -- Do a coarse check to make sure there's a build available.
        if not os.isdir(src .. ".") or not (x86_ok or x64_ok) then
            error("There's no build available in '" .. src .. "'")
        end

        -- Now we can extract the version from the executables.
        local version = nil
        local exe = x86_ok and "x32/sudo.exe" or "x64/sudo.exe"
        local ver_cmd = src:gsub("/", "\\") .. exe .. " --version"
        for line in io.popen(ver_cmd):lines() do
            if not version then
                version = line:match(" (%d%.[%x.]+)")
            end
        end
        if not version then
            error("Failed to extract version from build executables")
        end

        -- Now we know the version we can create our output directory.
        local target_dir = root_dir .. os.date("%Y%m%d_%H%M%S") .. "_" .. version .. "/"
        rmdir(target_dir)
        mkdir(target_dir)

        -- Package the release and the pdbs separately.
        os.chdir(src .. "/x32")
        if have_7z then
            exec(have_7z .. " a -r  " .. target_dir .. "/sudo-x86-v" .. version .. "-pdb.zip  *.pdb")
            exec(have_7z .. " a -r  " .. target_dir .. "/sudo-x86-v" .. version .. "-exe.zip  *.exe")
        end
        os.chdir(src .. "/x64")
        if have_7z then
            exec(have_7z .. " a -r  " .. target_dir .. "/sudo-x64-v" .. version .. "-pdb.zip  *.pdb")
            exec(have_7z .. " a -r  " .. target_dir .. "/sudo-x64-v" .. version .. "-exe.zip  *.exe")
        end

        -- Tidy up code directory.
        os.chdir(code_dir)
        rmdir(".build")
        rmdir(".git")
        unlink(".gitignore")

        -- Report some facts about what just happened.
        print("\n\n")
        if not have_7z then     warn("7-ZIP NOT FOUND -- Packing to .zip files was skipped.") end
        if not x86_ok then      failed("x86 BUILD FAILED") end
        if not x64_ok then      failed("x64 BUILD FAILED") end
        if not any_warnings_or_failures then
            print("\x1b[0;32;1mRelease " .. version .. " built successfully.\x1b[m")
        end
    end
}

