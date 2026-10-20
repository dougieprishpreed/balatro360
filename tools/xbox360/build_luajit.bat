@rem Balatro360: builds LuaJIT for the Xbox 360 "LuaJIT" configurations.
@rem
@rem Requires LuaJIT 2.0 (the v2.0 branch) in third_party\luajit. LuaJIT 2.1
@rem removed the PowerPC object-file writer the Xbox 360 build needs, so its
@rem leftover xedkbuild.bat cannot work.
@rem
@rem This is LuaJIT 2.0's own src\xedkbuild.bat (public domain) with two
@rem changes:
@rem  * the Xbox 360 compile uses /fp:strict. Without it the compiler fuses
@rem    multiply-adds in LuaJIT's C code (e.g. math.randomseed), which changes
@rem    random numbers compared to the PC version and breaks seed parity.
@rem  * no /DLUA_BUILD_AS_DLL: LuaJIT is linked statically into the XEX, and
@rem    dllexport'ed API functions would give the XEX an export table.
@rem
@rem Usage: open "Visual Studio Command Prompt (2010)" (32 bit) and run
@rem   tools\xbox360\build_luajit.bat
@rem Produces third_party\luajit\src\luajit20.lib.

@if not exist "%~dp0..\..\third_party\luajit\src\lj_vm.h" (
  @echo LuaJIT not found in third_party\luajit. See docs\BUILDING_XDK.md.
  @exit /b 1
)
@if exist "%~dp0..\..\third_party\luajit\src\lib_buffer.c" (
  @echo third_party\luajit contains LuaJIT 2.1, which can no longer build for
  @echo the Xbox 360. Replace it with LuaJIT 2.0, see docs\BUILDING_XDK.md.
  @exit /b 1
)
@pushd "%~dp0..\..\third_party\luajit\src"
@call :BUILD %*
@popd
@exit /b

:BUILD
@rem ---- LuaJIT 2.0's xedkbuild.bat follows ----
@rem Script to build LuaJIT with the Xbox 360 SDK.
@rem Donated to the public domain.
@rem
@rem Open a "Visual Studio .NET Command Prompt" (32 bit host compiler)
@rem Then cd to this directory and run this script.

@if not defined INCLUDE goto :FAIL
@if not defined XEDK goto :FAIL

@setlocal
@rem ---- Host compiler ----
@set LJCOMPILE=cl /nologo /c /MD /O2 /W3 /D_CRT_SECURE_NO_DEPRECATE
@set LJLINK=link /nologo
@set LJMT=mt /nologo
@set DASMDIR=..\dynasm
@set DASM=%DASMDIR%\dynasm.lua
@set ALL_LIB=lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c

%LJCOMPILE% host\minilua.c
@if errorlevel 1 goto :BAD
%LJLINK% /out:minilua.exe minilua.obj
@if errorlevel 1 goto :BAD
if exist minilua.exe.manifest^
  %LJMT% -manifest minilua.exe.manifest -outputresource:minilua.exe

@rem Error out for 64 bit host compiler
@minilua
@if errorlevel 8 goto :FAIL

@set DASMFLAGS=-D GPR64 -D FRAME32 -D PPE -D SQRT -D DUALNUM
minilua %DASM% -LN %DASMFLAGS% -o host\buildvm_arch.h vm_ppc.dasc
@if errorlevel 1 goto :BAD

if exist ..\.git ( git show -s --format=%%ct >luajit_relver.txt ) else ( type ..\.relver >luajit_relver.txt )
minilua host\genversion.lua

%LJCOMPILE% /I "." /I %DASMDIR% /D_XBOX_VER=200 /DLUAJIT_TARGET=LUAJIT_ARCH_PPC  host\buildvm*.c
@if errorlevel 1 goto :BAD
%LJLINK% /out:buildvm.exe buildvm*.obj
@if errorlevel 1 goto :BAD
if exist buildvm.exe.manifest^
  %LJMT% -manifest buildvm.exe.manifest -outputresource:buildvm.exe

buildvm -m peobj -o lj_vm.obj
@if errorlevel 1 goto :BAD
buildvm -m bcdef -o lj_bcdef.h %ALL_LIB%
@if errorlevel 1 goto :BAD
buildvm -m ffdef -o lj_ffdef.h %ALL_LIB%
@if errorlevel 1 goto :BAD
buildvm -m libdef -o lj_libdef.h %ALL_LIB%
@if errorlevel 1 goto :BAD
buildvm -m recdef -o lj_recdef.h %ALL_LIB%
@if errorlevel 1 goto :BAD
buildvm -m vmdef -o jit\vmdef.lua %ALL_LIB%
@if errorlevel 1 goto :BAD
buildvm -m folddef -o lj_folddef.h lj_opt_fold.c
@if errorlevel 1 goto :BAD

@rem ---- Cross compiler ----
@set LJCOMPILE="%XEDK%\bin\win32\cl" /nologo /c /MT /O2 /fp:strict /W3 /GF /Gm- /GR- /GS- /Gy /openmp- /D_CRT_SECURE_NO_DEPRECATE /DNDEBUG /D_XBOX /D_LIB /DLUAJIT_USE_SYSMALLOC
@set LJLIB="%XEDK%\bin\win32\lib" /nologo
@set "INCLUDE=%XEDK%\include\xbox"

@if "%1" neq "debug" goto :NODEBUG
@shift
@set "LJCOMPILE=%LJCOMPILE% /Zi"
:NODEBUG
@if "%1"=="amalg" goto :AMALG
%LJCOMPILE% lj_*.c lib_*.c
@if errorlevel 1 goto :BAD
%LJLIB% /OUT:luajit20.lib lj_*.obj lib_*.obj
@if errorlevel 1 goto :BAD
@goto :NOAMALG
:AMALG
%LJCOMPILE% ljamalg.c
@if errorlevel 1 goto :BAD
%LJLIB% /OUT:luajit20.lib ljamalg.obj lj_vm.obj
@if errorlevel 1 goto :BAD
:NOAMALG

@del *.obj *.manifest minilua.exe buildvm.exe
@echo.
@echo === Successfully built LuaJIT for Xbox 360 ===

@goto :END
:BAD
@echo.
@echo *******************************************************
@echo *** Build FAILED -- Please check the error messages ***
@echo *******************************************************
@goto :END
:FAIL
@echo To run this script you must open a "Visual Studio .NET Command Prompt"
@echo (32 bit host compiler). The Xbox 360 SDK must be installed, too.
:END
