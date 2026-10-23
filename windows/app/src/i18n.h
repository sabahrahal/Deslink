// Idiomas de la ventana del PC. El inglés es la base: un texto que falte en otro idioma sale en inglés. Sin Windows,
// para poder probarlo en tests/.
//
// Para añadir un idioma: su valor en Lang (antes de kCount), su código y su nombre en i18n.cpp (kLangs) y su columna
// en cada texto de la tabla. tests/test_view_model.cpp avisa si a un texto le falta alguna.
#pragma once

#include <initializer_list>
#include <string>
#include <vector>

enum class Lang { kEn, kEs, kCount };
constexpr int kLangCount = static_cast<int>(Lang::kCount);

const wchar_t* LangCode(Lang lang);  // «en», «es»
const wchar_t* LangName(Lang lang);  // en su propio idioma: «English», «Español»

// Idioma de un código como «es», «es-ES» o «pt-BR». Si aún no hay traducción a ese idioma, el inglés.
Lang LangFromCode(const std::wstring& code);
// El elegido en Configuración avanzada (código), o con él vacío el de Windows (system, como «es-ES»).
Lang ResolveLang(const std::wstring& setting, const std::wstring& system);
bool IsLangCode(const std::wstring& code);

void SetLang(Lang lang);
Lang CurrentLang();

// El texto de key en el idioma actual, con {1}, {2}… cambiados por args.
std::wstring Tr(const char* key, std::initializer_list<std::wstring> args = {});

// Separador decimal del idioma actual: «48,0 Mbps» en español, «48.0 Mbps» en inglés.
wchar_t DecimalSeparator();

// Claves de la tabla a las que les falta algún idioma (para las pruebas).
std::vector<std::string> MissingTranslations();
