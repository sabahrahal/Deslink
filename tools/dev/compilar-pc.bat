@echo off
rem Compila en este PC el instalador de Windows (sin GitHub Actions) y lo instala encima.
rem La primera vez hay que abrirlo como administrador: instala Visual Studio Build Tools, el WDK e Inno Setup.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0compilar-pc.ps1" -Instalar
pause
