@echo off
rem 在 MSVC x64 环境下重新配置并构建 FMT（Debug + Release），供本地发布验证使用。
rem 用法： build-release.cmd            -> 构建 Debug 与 Release
rem        build-release.cmd Debug      -> 只构建 Debug
setlocal

set VCVARS="D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set CMAKE="D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

call %VCVARS% >nul
if errorlevel 1 (
  echo [FAIL] vcvars64 失败
  exit /b 1
)

if "%~1"=="" goto both
if /I "%~1"=="Debug" goto debug
if /I "%~1"=="Release" goto release
echo 未知参数：%~1
exit /b 2

:both
call :onedir Debug || exit /b 1
call :onedir Release || exit /b 1
goto end

:debug
call :onedir Debug || exit /b 1
goto end

:release
call :onedir Release || exit /b 1
goto end

:onedir
echo.
echo ===== %1 =====
%CMAKE% -S . -B cmake-build-%1 -G Ninja -DCMAKE_BUILD_TYPE=%1 -DBUILD_TESTING=ON
if errorlevel 1 exit /b 1
%CMAKE% --build cmake-build-%1 --parallel
if errorlevel 1 exit /b 1
exit /b 0

:end
echo.
echo [OK] 构建完成
endlocal
