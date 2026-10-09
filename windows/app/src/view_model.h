// Qué enseña la ventana del PC en cada estado, sin dibujar nada: main.cpp lo pinta con GDI+ y
// tests/test_view_model.cpp comprueba que cada cosa aparece donde debe. Todo lo que el usuario ve sale de aquí.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "settings.h"

enum class State { kStopped, kStarting, kWaiting, kConnected, kStopping, kRestarting, kError };

// Color con el que se pinta un texto (tokens de design/tokens.json).
enum class Tone { kNormal, kSecondary, kSuccess, kWarning, kError };

struct ViewInput {
  State state = State::kStopped;
  Settings settings;
  std::map<std::wstring, std::wstring> stats;  // última línea «@estado» del host
  std::wstring error;                          // mensaje de error para el usuario
  bool advanced_open = false;
  bool driver_installed = true;
  bool dirty = false;      // hay ajustes que solo se aplican reiniciando la transmisión
  bool autostart = false;  // abrir al iniciar Windows
  std::vector<std::wstring> monitors;
  std::wstring addresses;  // IP de este PC
};

enum class Control { kSegmented, kChoice, kToggle, kInfo, kLink };

struct Row {
  std::string id;  // estable: lo usan las pruebas y main.cpp para saber qué hacer al pulsar
  Control control = Control::kInfo;
  std::wstring label;
  std::wstring hint;
  Tone hint_tone = Tone::kSecondary;
  std::vector<std::wstring> options;  // kSegmented
  int selected = -1;                  // kSegmented
  bool on = false;                    // kToggle
  std::wstring value;                 // kChoice: lo elegido; kInfo: el texto
};

struct Line {
  std::wstring text;
  Tone tone = Tone::kSecondary;
};

struct View {
  std::wstring title;  // «Conectado por cable USB»
  Tone dot = Tone::kSecondary;
  std::wstring detail;
  std::wstring stats;  // «120 fps · 48,0 Mbps · 4 ms», solo conectado
  Line notice;         // por qué la imagen va por debajo de lo elegido, si es así
  std::wstring error;
  std::vector<Line> connection;  // por dónde puede conectar la tablet (cable USB y Wi-Fi)
  std::wstring primary;          // «Iniciar», «Detener»…
  bool primary_enabled = true;
  bool primary_stop = false;
  bool show_apply = false;
  std::vector<Row> main;  // a la vista: modo, calidad, sonido, táctil
  bool advanced_open = false;
  std::vector<Row> advanced;
};

View BuildView(const ViewInput& in);

// Opciones de los desplegables de Configuración avanzada (0 o vacío = automático).
const std::vector<int>& FpsChoices();
const std::vector<int>& BitrateChoices();
const std::vector<std::wstring>& SizeChoices();
const std::vector<std::wstring>& CodecChoices();
std::wstring FpsLabel(int fps);
std::wstring BitrateLabel(int mbps);
std::wstring SizeLabel(const std::wstring& size);
std::wstring CodecLabel(const std::wstring& codec);

// Calidad, sonido y táctil como índice de su control y al revés.
int QualityIndex(const std::wstring& quality);
std::wstring QualityAt(int index);
