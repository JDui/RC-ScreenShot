#pragma once

#include "common.hpp"

namespace rc {

struct ColorSetting {
  uint32_t rgba = 0xFF3B30FF;  // RRGGBBAA
};

struct HotkeySetting {
  UINT modifiers = MOD_CONTROL | MOD_NOREPEAT;
  UINT virtualKey = VK_OEM_5;
  bool enabled = true;
};

struct StrokeSetting {
  ColorSetting color{};
  float width = 4.0f;
  float opacity = 1.0f;
};

struct ShapeSetting {
  StrokeSetting stroke{};
  ColorSetting fill{0x00000000};
  float fillOpacity = 0.0f;
};

struct TextSetting {
  ColorSetting color{0xFFFFFFFF};
  float size = 28.0f;
  float opacity = 1.0f;
  bool vertical = false;
  std::wstring fontFamily = L"Microsoft YaHei UI";
};

enum class DefaultAction { Copy, Save };
enum class MosaicStyle { Pixel, Blur };

struct AppConfig {
  int schemaVersion = 1;
  std::vector<HotkeySetting> hotkeys{{}};
  bool launchAtLogin = false;
  bool silentAtLogin = true;

  std::wstring outputDirectory = L"Screenshots";
  std::wstring filenameTemplate = L"RC_yyyyMMdd_HHmmss_fff.jpg";
  int jpegQuality = 95;
  DefaultAction defaultAction = DefaultAction::Copy;
  bool autoSaveOnCopy = false;

  StrokeSetting pen{};
  ShapeSetting rectangle{};
  ShapeSetting ellipse{};
  StrokeSetting line{};
  StrokeSetting arrow{};
  TextSetting text{};
  StrokeSetting frame{};
  bool frameEnabled = false;
  MosaicStyle mosaicStyle = MosaicStyle::Pixel;
  float mosaicBrushSize = 32.0f;
  int mosaicPixelSize = 16;
  float mosaicBlurRadius = 12.0f;

  bool windowShadow = true;
  std::wstring language = L"zh-CN";
  std::wstring theme = L"dark";
  std::wstring toolbarPosition = L"auto";
};

class ConfigStore {
 public:
  explicit ConfigStore(std::filesystem::path executablePath);

  const std::filesystem::path& path() const { return path_; }
  const std::filesystem::path& executable_directory() const { return executableDirectory_; }
  AppConfig Load(std::wstring* warning = nullptr) const;
  bool Save(const AppConfig& config, std::wstring* error = nullptr) const;
  std::filesystem::path ResolveOutputDirectory(const AppConfig& config) const;

 private:
  std::filesystem::path executableDirectory_;
  std::filesystem::path path_;
};

std::wstring FormatHotkey(const HotkeySetting& hotkey);
bool ParseHotkeyText(std::wstring_view text, HotkeySetting& out);

}  // namespace rc
