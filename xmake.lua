-- 工作区的xmake.lua
set_project("weasel")

-- 定义全局变量
set_xmakever("2.9.4")
set_languages("c++17")
set_runtimes("MT")  -- 设置运行时库为静态链接，避免/MD与/MT冲突
add_defines("UNICODE", "_UNICODE")
add_defines("WINDOWS")
add_defines("MSVC")
add_defines(
  "VERSION_MAJOR=" .. (os.getenv("VERSION_MAJOR") or "0"),
  "VERSION_MINOR=" .. (os.getenv("VERSION_MINOR") or "0"),
  "VERSION_PATCH=" .. (os.getenv("VERSION_PATCH") or "0"),
  "PRODUCT_VERSION=" .. (os.getenv("PRODUCT_VERSION") or "0.0.0.0")
)

add_includedirs("$(projectdir)/include")
-- 设置Boost库的全局路径：优先 BOOST_ROOT 环境变量，其次 scoop 默认安装路径
boost_root = os.getenv("BOOST_ROOT")
if not boost_root or boost_root == '' then
  local userprofile = os.getenv("USERPROFILE") or ''
  if userprofile ~= '' then
    local scoop_boost = path.join(userprofile, "scoop", "apps", "boost", "current")
    if os.isdir(scoop_boost) then
      boost_root = scoop_boost
    end
  end
end
if not (boost_root and os.isfile(path.join(boost_root, "boost", "version.hpp"))) then
  raise("Boost not found! Set BOOST_ROOT (see env.bat.template) or install boost via scoop.")
end
boost_include_path = boost_root
boost_lib_path = path.join(boost_root, "stage", "lib")
add_includedirs(boost_include_path)
add_linkdirs(boost_lib_path)
if not os.isdir(boost_lib_path) then
  print(("warning: %s not found; run setup-build-env.ps1 to build boost static libs first."):format(boost_lib_path))
end
add_cxflags("/utf-8 /MP /O2 /Oi /Gm- /EHsc /MT /GS /Gy /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline /external:W3 /Gd /TP")
add_ldflags("/TLBID:1 /DYNAMICBASE /NXCOMPAT")

-- 全局ATL lib路径
-- 优先使用 Developer Prompt 的 %include% 环境；否则用 vswhere 自行定位
-- Visual Studio，使 xmake 不依赖 Developer Command Prompt 也能编译
local atl_include = ''
local atl_lib_dir = ''
dpi_manifest = ''
local vs_include_env = os.getenv("include")
if vs_include_env and vs_include_env ~= '' then
  for include in vs_include_env:gmatch("([^;]+)") do
    if atl_include == '' and include:match(".*ATLMFC\\include\\?$") then
      atl_include = include
      atl_lib_dir = include:replace("include$", "lib")
      dpi_manifest = include:replace("ATLMFC\\include$", "Include\\Manifest\\PerMonitorHighDPIAware.manifest")
    end
    add_includedirs(include)
  end
end
if atl_include == '' then
  -- 从已知的 Visual Studio 安装位置探测（优先 VSINSTALLDIR 环境变量）
  local vs_dirs = {}
  local vs_env = os.getenv("VSINSTALLDIR")
  if vs_env and vs_env ~= '' then
    table.insert(vs_dirs, vs_env:trim())
  end
  for _, prog_files in ipairs({os.getenv("ProgramFiles") or '', os.getenv("ProgramFiles(x86)") or ''}) do
    if prog_files ~= '' then
      table.join2(vs_dirs, os.dirs(path.join(prog_files, "Microsoft Visual Studio", "*", "*")))
      table.join2(vs_dirs, os.dirs(path.join(prog_files, "Microsoft Visual Studio", "*")))
    end
  end
  for _, vs_dir in ipairs(vs_dirs) do
    local toolsets = os.dirs(path.join(vs_dir, "VC", "Tools", "MSVC", "*"))
    table.sort(toolsets, function(a, b) return a > b end)
    for _, toolset_dir in ipairs(toolsets) do
      if os.isdir(path.join(toolset_dir, "atlmfc")) then
        atl_include = path.join(toolset_dir, "atlmfc", "include")
        atl_lib_dir = path.join(toolset_dir, "atlmfc", "lib")
        for _, manifest in ipairs({
          path.join(toolset_dir, "include", "Manifest", "PerMonitorHighDPIAware.manifest"),
          path.join(atl_include, "Manifest", "PerMonitorHighDPIAware.manifest")}) do
          if os.isfile(manifest) then
            dpi_manifest = manifest
            break
          end
        end
        break
      end
    end
    if atl_include ~= '' then
      add_includedirs(atl_include)
      break
    end
  end
  if atl_include == '' then
    print("warning: cannot locate ATLMFC, ATL/WTL headers may be missing. Install VS with ATL/MFC components.")
  end
end
-- DPI manifest：仓库根缺失时尽量从 VS 复制一份（各 target 的 add_files 引用仓库根路径）
if dpi_manifest ~= '' and not os.isfile(path.join(os.projectdir(), "PerMonitorHighDPIAware.manifest")) then
  if not pcall(function() os.cp(dpi_manifest, os.projectdir()) end) then
    print(("warning: copy %s to the repo root for DPI manifest embedding."):format(dpi_manifest))
  end
end

add_includedirs("$(projectdir)/include/wtl")

if is_arch("x64") then
  add_linkdirs("$(projectdir)/lib64")
  add_linkdirs(atl_lib_dir .. "/x64")
elseif is_arch("x86") then
  add_linkdirs("$(projectdir)/lib")
  add_linkdirs(atl_lib_dir .. "/x86")
elseif is_arch("arm") then
  add_linkdirs(atl_lib_dir .. "/arm")
elseif is_arch("arm64") then
  add_linkdirs(atl_lib_dir .. "/arm64")
end

add_links("atls", "shell32", "advapi32", "gdi32", "user32", "uuid", "ole32")

includes("WeaselIPC", "WeaselUI", "WeaselTSF")

if is_arch("x64") or is_arch("x86") then
  includes("RimeWithWeasel", "WeaselIPCServer", "WeaselServer", "WeaselDeployer")
end

if is_arch("x86") then
  includes("WeaselSetup")
end

if is_mode("debug") then
  includes("test/TestWeaselIPC")
  includes("test/TestResponseParser")
  includes("test/TestPipeChannel")
  includes("test/TestFindIME")
  includes("test/TestCompartmentUtil")
  includes("test/TestPerUserReg")
  includes("test/TestWeaselUI")
  includes("test/TestBstrUtil")
  includes("test/TestWeaselSetup")
  includes("test/TestKeyEvent")
  includes("test/TestRegisterTipKeys")
else
  add_cxflags("/GL")
  add_ldflags("/LTCG /INCREMENTAL:NO", {force = true})
end

rule("subcmd")
  on_load(function(target)
    target:add("ldflags", "/SUBSYSTEM:CONSOLE")
  end)
rule("subwin")
  on_load(function(target)
    target:add("ldflags", "/SUBSYSTEM:WINDOWS")
  end)

rule("add_rcfiles")
  on_load(function(target)
    target:add("files", path.join(target:scriptdir(), "*.rc"),
      {defines = {"VERSION_MAJOR=" .. (os.getenv("VERSION_MAJOR") or "0"),
      "VERSION_MINOR=" .. (os.getenv("VERSION_MINOR") or "0"),
      "VERSION_PATCH=" .. (os.getenv("VERSION_PATCH") or "0"),
      "FILE_VERSION=" .. (os.getenv("FILE_VERSION") or "0.0.0.0"),
      "PRODUCT_VERSION=" .. (os.getenv("PRODUCT_VERSION") or "0.0.0.0")
    }})
  end)
rule("use_weaselconstants")
  on_load(function(target)
    function check_include_weasel_constants_in_dir(dir)
      -- memoize the scan result per directory for the lifetime of this xmake
      -- process: this rule's on_load runs for every target, and each scan
      -- otherwise re-reads every .h/.cpp file under the target directory
      local cache = _weasel_constants_check_cache
      if cache == nil then
        cache = {}
        _weasel_constants_check_cache = cache
      end
      local cached = cache[dir]
      if cached ~= nil then
        return cached
      end
      local found = false
      local files = os.files(path.join(dir, "**.h"))
      table.join2(files, os.files(path.join(dir, "**.cpp")))
      for _, file in ipairs(files) do
        local content = io.readfile(file)
        if content:find('#include%s+"WeaselConstants%.h"') or content:find('#include%s+<WeaselConstants%.h>') then
          found = true
          break
        end
      end
      cache[dir] = found
      return found
    end
    if check_include_weasel_constants_in_dir(target:scriptdir()) then
      target:add("rcflags", {
        "/dVERSION_MAJOR=" .. (os.getenv("VERSION_MAJOR") or "0"),
        "/dVERSION_MINOR=" .. (os.getenv("VERSION_MINOR") or "0"),
        "/dVERSION_PATCH=" .. (os.getenv("VERSION_PATCH") or "0"),
        "/dFILE_VERSION=" .. (os.getenv("FILE_VERSION") or "\"0.0.0.0\""),
        "/dPRODUCT_VERSION=" .. (os.getenv("PRODUCT_VERSION") or "\"0.0.0.0\"")
      })
    end
  end)
