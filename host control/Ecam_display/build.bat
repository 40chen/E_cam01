@echo off
chcp 65001 >nul
title Build Ecam Status Display

echo ========================================
echo   Building Ecam Status Display .exe
echo ========================================
echo.

echo [1/3] Installing dependencies...
:: 在这里加入了 keyring
pip install pyserial psutil pynvml pyinstaller keyring -q
if %errorlevel% neq 0 (
    echo ERROR: Failed to install dependencies!
    pause
    exit /b 1
)

echo [2/3] Cleaning old build...
if exist "build" rmdir /s /q "build"
if exist "dist" rmdir /s /q "dist"
if exist "*.spec" del /q *.spec

echo [3/3] Building .exe file...
pyinstaller --onefile --windowed --name "Ecam_Status_Display" ^
    --hidden-import=psutil ^
    --hidden-import=serial ^
    --hidden-import=serial.tools ^
    --hidden-import=serial.tools.list_ports ^
    --hidden-import=keyring.backends.Windows ^
    --hidden-import=pynvml ^
    Ecam_Status_Display.py

echo.
if exist "dist\Ecam_Status_Display.exe" (
    echo ========================================
    echo   Build Complete!
    echo   Executable: dist\Ecam_Status_Display.exe
    echo ========================================
    explorer "dist"
) else (
    echo ========================================
    echo   ERROR: Build failed!
    echo ========================================
)

pause