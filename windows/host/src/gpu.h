#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>

using Microsoft::WRL::ComPtr;

// Dispositivo D3D11 creado sobre el adaptador al que pertenece la salida elegida.
// Capturador, conversor y codificador comparten este dispositivo: así el frame
// no sale de la GPU en todo el pipeline.
struct Gpu {
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput1> output;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  std::wstring device_name;  // nombre GDI de la salida («\\.\DISPLAY3»)
  RECT desktop_rect{};  // posición de la salida en el escritorio virtual, en píxeles (ver ScreenRect)
};

// output_index recorre todas las salidas de todos los adaptadores, en orden.
bool CreateGpuForOutput(int output_index, Gpu* gpu);
// Busca la salida por su nombre GDI («\\.\DISPLAY3»), p. ej. la del monitor virtual.
bool CreateGpuForDeviceName(const wchar_t* device_name, Gpu* gpu);
// Posición y tamaño actuales de la pantalla en el escritorio virtual, en píxeles reales, según Windows (GDI).
// Es lo que vale para el táctil y el ratón: la descripción de la salida en DXGI se queda con lo que había al crear
// la GPU y no ve los cambios de resolución o de sitio que vengan después. false si esa pantalla ya no está.
bool ScreenRect(const std::wstring& device_name, RECT* rect);
