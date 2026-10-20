#!/usr/bin/env python3
"""Generates build/xbox360/Balatro360.vcxproj (+ .sln and .filters).

Run after adding or removing source files:

    python3 tools/gen_vcxproj.py

The generated files are committed so Windows users don't need Python.
"""
import glob
import os
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "build", "xbox360")
PROJECT_GUID = "{7A1F3C52-8E4B-4B0B-9C3D-0B4E1A5E3600}"
SLN_GUID = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}"

# Libraries used by the XDK's own samples (Debug / Release).
LIBS_DEBUG = ("xapilibd.lib;d3d9d.lib;d3dx9d.lib;xgraphicsd.lib;xboxkrnl.lib;xnetd.lib;"
              "xaudiod2.lib;x3daudiod.lib;xmcored.lib;xbdm.lib;vcompd.lib")
LIBS_RELEASE = ("xapilib.lib;d3d9.lib;d3dx9.lib;xgraphics.lib;xboxkrnl.lib;xnet.lib;"
                "xaudio2.lib;x3daudio.lib;xmcore.lib;vcomp.lib")

COMMON_INCLUDES = [r"..\..\src", r"..\..\third_party\stb", r"..\..\third_party\miniz"]
PUC_INCLUDES = [r"..\..\third_party\lua\src", r"..\..\third_party\luabitop"]
LUAJIT_DIR = r"..\..\third_party\luajit\src"

# (name, debug, luajit)
CONFIGS = [
    ("Debug", True, False),
    ("Release", False, False),
    # Link LuaJIT (built with its src\xedkbuild.bat) instead of PUC Lua 5.1.
    ("Debug LuaJIT", True, True),
    ("Release LuaJIT", False, True),
]


def rel(p):
    return os.path.relpath(p, OUT_DIR).replace("/", "\\")


def sources():
    files = []
    lua = sorted(glob.glob(os.path.join(ROOT, "third_party", "lua", "src", "*.c")))
    skip = {"lua.c", "luac.c", "print.c", "linit.c", "loslib.c", "liolib.c"}
    for f in lua:
        if os.path.basename(f) not in skip:
            files.append((f, "lua"))
    files.append((os.path.join(ROOT, "third_party", "luabitop", "bit.c"), "lua"))
    for name in ["miniz.c", "miniz_tdef.c", "miniz_tinfl.c"]:
        files.append((os.path.join(ROOT, "third_party", "miniz", name), "c"))
    for f in sorted(glob.glob(os.path.join(ROOT, "src", "runtime", "*.cpp"))):
        files.append((f, "cpp"))
    for f in ["src/gfx/shader_translate.cpp", "src/gfx/d3d9/device_d3d9.cpp", "src/third_party_impl.cpp",
              "src/platform/xbox360/platform_x360.cpp", "src/platform/xbox360/main_x360.cpp"]:
        files.append((os.path.join(ROOT, f), "cpp"))
    return files


def headers():
    out = []
    for pat in ["src/**/*.h", "third_party/lua/src/*.h", "third_party/miniz/*.h"]:
        out += glob.glob(os.path.join(ROOT, pat), recursive=True)
    return sorted(out)


def filter_for(path):
    r = os.path.relpath(path, ROOT).replace("/", "\\")
    return os.path.dirname(r)


def config_block(cfg, debug, luajit):
    defs = ("_DEBUG;" if debug else "NDEBUG;") + "_XBOX;"
    defs += "B360_LUAJIT;" if luajit else "LUA_ANSI;"
    defs += "B360_HAS_SOURCE_STAMP;%(PreprocessorDefinitions)"
    opt = "Disabled" if debug else "Full"
    libs = LIBS_DEBUG if debug else LIBS_RELEASE
    if luajit:
        libs = LUAJIT_DIR + "\\luajit20.lib;" + libs
    inc = ";".join(["$(ProjectDir)stamp"] + COMMON_INCLUDES + ([LUAJIT_DIR] if luajit else PUC_INCLUDES))
    cond = "'$(Configuration)|$(Platform)'=='%s|Xbox 360'" % cfg
    return """  <ItemDefinitionGroup Condition="{cond}">
    <ClCompile>
      <PrecompiledHeader>NotUsing</PrecompiledHeader>
      <WarningLevel>Level3</WarningLevel>
      <Optimization>{opt}</Optimization>
      <FunctionLevelLinking>true</FunctionLevelLinking>
      <StringPooling>true</StringPooling>
      <ExceptionHandling>Sync</ExceptionHandling>
      <BufferSecurityCheck>false</BufferSecurityCheck>
      <RuntimeTypeInfo>false</RuntimeTypeInfo>
      <PreprocessorDefinitions>{defs}</PreprocessorDefinitions>
      <RuntimeLibrary>{crt}</RuntimeLibrary>
      <AdditionalIncludeDirectories>{inc};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <ProgramDataBaseFileName>$(IntDir)vc$(PlatformToolsetVersion).pdb</ProgramDataBaseFileName>
      <DebugInformationFormat>ProgramDatabase</DebugInformationFormat>
    </ClCompile>
    <Link>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <ProgramDatabaseFile>$(OutDir)$(ProjectName).pdb</ProgramDatabaseFile>
      <AdditionalDependencies>{libs};%(AdditionalDependencies)</AdditionalDependencies>
      <OptimizeReferences>{optref}</OptimizeReferences>
      <SetChecksum>true</SetChecksum>
    </Link>
    <PreBuildEvent>
      <!-- Records the git commit for the first line of b360.log. -->
      <Command>call "$(ProjectDir)..\\..\\tools\\xbox360\\source_stamp.bat" "$(ProjectDir)stamp\\source_stamp.h"</Command>
    </PreBuildEvent>
    <Deploy>
      <!-- No automatic copy to a dev kit (most people test with Xenia or a
           USB launcher). Set to false if you have a dev kit attached. -->
      <ExcludedFromBuild>true</ExcludedFromBuild>
      <DeploymentType>CopyToHardDrive</DeploymentType>
    </Deploy>
  </ItemDefinitionGroup>
""".format(cond=cond, opt=opt, defs=defs, inc=inc, libs=libs, optref="false" if debug else "true",
           # Always the static CRT: the XDK picks /MD (DLL CRT, which needs a
           # nonexistent XAPI.lib) for configuration names it doesn't know.
           crt="MultiThreadedDebug" if debug else "MultiThreaded")


def vcxproj():
    configs = CONFIGS
    out = ['<?xml version="1.0" encoding="utf-8"?>',
           '<!-- Generated by tools/gen_vcxproj.py. Do not edit by hand. -->',
           '<Project DefaultTargets="Build" ToolsVersion="4.0" '
           'xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
           '  <ItemGroup Label="ProjectConfigurations">']
    for cfg, _, _ in configs:
        out.append('    <ProjectConfiguration Include="%s|Xbox 360">' % cfg)
        out.append('      <Configuration>%s</Configuration>' % cfg)
        out.append('      <Platform>Xbox 360</Platform>')
        out.append('    </ProjectConfiguration>')
    out.append('  </ItemGroup>')
    out.append('  <PropertyGroup Label="Globals">')
    out.append('    <ProjectGuid>%s</ProjectGuid>' % PROJECT_GUID)
    out.append('    <Keyword>Xbox360Proj</Keyword>')
    out.append('    <RootNamespace>Balatro360</RootNamespace>')
    out.append('  </PropertyGroup>')
    out.append('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />')
    for cfg, debug, _ in configs:
        out.append('  <PropertyGroup Condition="\'$(Configuration)|$(Platform)\'==\'%s|Xbox 360\'" '
                   'Label="Configuration">' % cfg)
        out.append('    <ConfigurationType>Application</ConfigurationType>')
        out.append('    <CharacterSet>MultiByte</CharacterSet>')
        if not debug:
            out.append('    <WholeProgramOptimization>true</WholeProgramOptimization>')
        out.append('  </PropertyGroup>')
    out.append('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />')
    out.append('  <ImportGroup Label="ExtensionSettings" />')
    out.append('  <ImportGroup Label="PropertySheets">')
    out.append('    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props" '
               'Condition="exists(\'$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props\')" '
               'Label="LocalAppDataPlatform" />')
    out.append('  </ImportGroup>')
    out.append('  <PropertyGroup Label="UserMacros" />')
    for cfg, debug, _ in configs:
        cond = "'$(Configuration)|$(Platform)'=='%s|Xbox 360'" % cfg
        out.append('  <PropertyGroup Condition="%s">' % cond)
        out.append('    <OutDir>$(SolutionDir)out\\$(Configuration)\\</OutDir>')
        out.append('    <IntDir>$(SolutionDir)obj\\$(Configuration)\\</IntDir>')
        out.append('    <TargetName>default</TargetName>')
        out.append('    <LinkIncremental>false</LinkIncremental>')
        out.append('    <ImageXexOutput>$(OutDir)default.xex</ImageXexOutput>')
        out.append('    <RemoteRoot>devkit:\\Balatro360</RemoteRoot>')
        out.append('  </PropertyGroup>')
    for cfg, debug, luajit in configs:
        out.append(config_block(cfg, debug, luajit).rstrip("\n"))

    out.append('  <ItemGroup>')
    for path, kind in sources():
        r = rel(path)
        if kind == "lua":
            # Lua is compiled as C++ (errors become C++ exceptions) and with
            # strict floating point (no fused multiply-add), so number
            # crunching matches LuaJIT on PC bit for bit. Not built in the
            # LuaJIT configurations.
            out.append('    <ClCompile Include="%s">' % r)
            out.append('      <CompileAs>CompileAsCpp</CompileAs>')
            out.append('      <FloatingPointModel>Strict</FloatingPointModel>')
            out.append('      <WarningLevel>Level1</WarningLevel>')
            for cfg, _, luajit in CONFIGS:
                if luajit:
                    out.append('      <ExcludedFromBuild Condition="\'$(Configuration)|$(Platform)\'=='
                               '\'%s|Xbox 360\'">true</ExcludedFromBuild>' % cfg)
            out.append('    </ClCompile>')
        elif kind == "c":
            out.append('    <ClCompile Include="%s">' % r)
            out.append('      <CompileAs>CompileAsC</CompileAs>')
            out.append('      <WarningLevel>Level1</WarningLevel>')
            out.append('    </ClCompile>')
        elif path.endswith("l_compat.cpp"):
            out.append('    <ClCompile Include="%s">' % r)
            out.append('      <FloatingPointModel>Strict</FloatingPointModel>')
            out.append('    </ClCompile>')
        else:
            out.append('    <ClCompile Include="%s" />' % r)
    out.append('  </ItemGroup>')
    out.append('  <ItemGroup>')
    for h in headers():
        out.append('    <ClInclude Include="%s" />' % rel(h))
    out.append('  </ItemGroup>')
    out.append('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />')
    out.append('  <ImportGroup Label="ExtensionTargets" />')
    out.append('</Project>')
    return "\r\n".join(out) + "\r\n"


def filters():
    items = [(p, "ClCompile") for p, _ in sources()] + [(h, "ClInclude") for h in headers()]
    dirs = sorted(set(filter_for(p) for p, _ in items))
    out = ['<?xml version="1.0" encoding="utf-8"?>',
           '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
           '  <ItemGroup>']
    seen = set()
    for d in dirs:
        parts = d.split("\\")
        for i in range(1, len(parts) + 1):
            sub = "\\".join(parts[:i])
            if sub in seen:
                continue
            seen.add(sub)
            out.append('    <Filter Include="%s">' % sub)
            out.append('      <UniqueIdentifier>{%s}</UniqueIdentifier>' % uuid.uuid5(uuid.NAMESPACE_URL, sub))
            out.append('    </Filter>')
    out.append('  </ItemGroup>')
    for kind in ["ClCompile", "ClInclude"]:
        out.append('  <ItemGroup>')
        for p, k in items:
            if k != kind:
                continue
            out.append('    <%s Include="%s">' % (kind, rel(p)))
            out.append('      <Filter>%s</Filter>' % filter_for(p))
            out.append('    </%s>' % kind)
        out.append('  </ItemGroup>')
    out.append('</Project>')
    return "\r\n".join(out) + "\r\n"


def sln():
    return "\r\n".join([
        "",
        "Microsoft Visual Studio Solution File, Format Version 11.00",
        "# Visual Studio 2010",
        'Project("%s") = "Balatro360", "Balatro360.vcxproj", "%s"' % (SLN_GUID, PROJECT_GUID),
        "EndProject",
        "Global",
        "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution",
    ] + ["\t\t%s|Xbox 360 = %s|Xbox 360" % (c[0], c[0]) for c in CONFIGS] + [
        "\tEndGlobalSection",
        "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution",
    ] + sum([["\t\t%s.%s|Xbox 360.ActiveCfg = %s|Xbox 360" % (PROJECT_GUID, c[0], c[0]),
              "\t\t%s.%s|Xbox 360.Build.0 = %s|Xbox 360" % (PROJECT_GUID, c[0], c[0])] for c in CONFIGS], []) + [
        "\tEndGlobalSection",
        "\tGlobalSection(SolutionProperties) = preSolution",
        "\t\tHideSolutionNode = FALSE",
        "\tEndGlobalSection",
        "EndGlobal",
        "",
    ])


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, data in [("Balatro360.vcxproj", vcxproj()), ("Balatro360.vcxproj.filters", filters()),
                       ("Balatro360.sln", sln())]:
        with open(os.path.join(OUT_DIR, name), "w", newline="") as f:
            f.write(data)
    print("wrote", os.path.relpath(OUT_DIR, ROOT))


if __name__ == "__main__":
    main()
