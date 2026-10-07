#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Dispositivo D3D11 creado sobre el adaptador al que pertenece la salida elegida.
// Capturador, conversor y codificador comparten este dispositivo: así el frame
// no sale de la GPU en todo el pipeline.
struct Gpu {
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput1> output;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
};

// output_index recorre todas las salidas de todos los adaptadores, en orden.
bool CreateGpuForOutput(int output_index, Gpu* gpu);
