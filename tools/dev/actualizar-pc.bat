@echo off
rem Instala en este PC el instalador de la última versión de prueba.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0actualizar.ps1" -PC
pause
