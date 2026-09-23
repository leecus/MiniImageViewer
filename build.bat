@echo off
rem 一键构建 看图 MiniImageViewer（需 MinGW-w64 在 PATH 中）
windres app.rc -O coff -o app.res || goto :err
g++ -O2 -std=c++17 -municode -mwindows -s -static -o ImageViewer.exe ImageViewer.cpp app.res -lgdiplus -lgdi32 -luser32 -lshell32 -lcomdlg32 -lole32 -ladvapi32 -lshlwapi || goto :err
echo BUILD OK: ImageViewer.exe
exit /b 0
:err
echo BUILD FAILED
exit /b 1
