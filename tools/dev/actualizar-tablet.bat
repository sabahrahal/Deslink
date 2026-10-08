@echo off
rem Instala en la tablet (conectada por USB) la APK de la última versión de prueba.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0actualizar.ps1" -Tablet
pause
