#include "config.hpp"

#include <charconv>
#include <fstream>
#include <shlobj.h>
#include <sstream>

namespace rc {
namespace {

struct Json {
  using Object = std::map<std::string, Json, std::less<>>;
  using Array = std::vector<Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value{nullptr};

  const Json* Find(std::string_view key) const {
    const auto* object = std::get_if<Object>(&value);
    if (!object) return nullptr;
    const auto it = object->find(key);
    return it == object->end() ? nullptr : &it->second;
  }
  const Object* AsObject() const { return std::get_if<Object>(&value); }
  const Array* AsArray() const { return std::get_if<Array>(&value); }
  std::optional<bool> AsBool() const {
    if (const auto* v = std::get_if<bool>(&value)) return *v;
    return std::nullopt;
  }
  std::optional<double> AsNumber() const {
    if (const auto* v = std::get_if<double>(&value)) return *v;
    return std::nullopt;
  }
  std::optional<std::string> AsString() const {
    if (const auto* v = std::get_if<std::string>(&value)) return *v;
    return std::nullopt;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view source) : source_(source) {}
  std::optional<Json> Parse() {
    SkipWhitespace();
    auto value = ParseValue();
    SkipWhitespace();
    if (!value || position_ != source_.size()) return std::nullopt;
    return value;
  }

 private:
  void SkipWhitespace() {
    while (position_ < source_.size() &&
           (source_[position_] == ' ' || source_[position_] == '\n' ||
            source_[position_] == '\r' || source_[position_] == '\t')) ++position_;
  }
  bool Consume(char expected) {
    SkipWhitespace();
    if (position_ >= source_.size() || source_[position_] != expected) return false;
    ++position_;
    return true;
  }
  std::optional<Json> ParseValue() {
    SkipWhitespace();
    if (position_ >= source_.size()) return std::nullopt;
    const char c = source_[position_];
    if (c == '{') return ParseObject();
    if (c == '[') return ParseArray();
    if (c == '"') { auto text = ParseString(); return text ? Json{*text} : std::optional<Json>{}; }
    if (source_.substr(position_, 4) == "true") { position_ += 4; return Json{true}; }
    if (source_.substr(position_, 5) == "false") { position_ += 5; return Json{false}; }
    if (source_.substr(position_, 4) == "null") { position_ += 4; return Json{nullptr}; }
    return ParseNumber();
  }
  std::optional<Json> ParseObject() {
    if (!Consume('{')) return std::nullopt;
    Json::Object object;
    SkipWhitespace();
    if (Consume('}')) return Json{object};
    while (true) {
      auto key = ParseString();
      if (!key || !Consume(':')) return std::nullopt;
      auto value = ParseValue();
      if (!value) return std::nullopt;
      object.emplace(std::move(*key), std::move(*value));
      SkipWhitespace();
      if (Consume('}')) break;
      if (!Consume(',')) return std::nullopt;
    }
    return Json{std::move(object)};
  }
  std::optional<Json> ParseArray() {
    if (!Consume('[')) return std::nullopt;
    Json::Array array;
    SkipWhitespace();
    if (Consume(']')) return Json{array};
    while (true) {
      auto value = ParseValue();
      if (!value) return std::nullopt;
      array.push_back(std::move(*value));
      SkipWhitespace();
      if (Consume(']')) break;
      if (!Consume(',')) return std::nullopt;
    }
    return Json{std::move(array)};
  }
  std::optional<std::string> ParseString() {
    if (!Consume('"')) return std::nullopt;
    std::string result;
    while (position_ < source_.size()) {
      char c = source_[position_++];
      if (c == '"') return result;
      if (c == '\\') {
        if (position_ >= source_.size()) return std::nullopt;
        const char escape = source_[position_++];
        switch (escape) {
          case '"': result.push_back('"'); break;
          case '\\': result.push_back('\\'); break;
          case '/': result.push_back('/'); break;
          case 'b': result.push_back('\b'); break;
          case 'f': result.push_back('\f'); break;
          case 'n': result.push_back('\n'); break;
          case 'r': result.push_back('\r'); break;
          case 't': result.push_back('\t'); break;
          case 'u': {
            if (position_ + 4 > source_.size()) return std::nullopt;
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = source_[position_++];
              code <<= 4;
              if (h >= '0' && h <= '9') code += h - '0';
              else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
              else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
              else return std::nullopt;
            }
            wchar_t wide[2] = {static_cast<wchar_t>(code), 0};
            result += ToUtf8(wide);
            break;
          }
          default: return std::nullopt;
        }
      } else {
        if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
        result.push_back(c);
      }
    }
    return std::nullopt;
  }
  std::optional<Json> ParseNumber() {
    const size_t start = position_;
    while (position_ < source_.size()) {
      const char c = source_[position_];
      if (!(c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' || (c >= '0' && c <= '9'))) break;
      ++position_;
    }
    if (start == position_) return std::nullopt;
    double value = 0.0;
    const auto [end, ec] = std::from_chars(source_.data() + start, source_.data() + position_, value);
    if (ec != std::errc{} || end != source_.data() + position_) return std::nullopt;
    return Json{value};
  }

  std::string_view source_;
  size_t position_ = 0;
};

std::string Escape(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output.push_back(c); break;
    }
  }
  return output;
}

bool ReadBool(const Json* parent, std::string_view name, bool fallback) {
  if (!parent) return fallback;
  const Json* node = parent->Find(name);
  return node ? node->AsBool().value_or(fallback) : fallback;
}
int ReadInt(const Json* parent, std::string_view name, int fallback, int low, int high) {
  if (!parent) return fallback;
  const Json* node = parent->Find(name);
  if (!node) return fallback;
  auto number = node->AsNumber();
  return number ? std::clamp(static_cast<int>(*number), low, high) : fallback;
}
float ReadFloat(const Json* parent, std::string_view name, float fallback, float low, float high) {
  if (!parent) return fallback;
  const Json* node = parent->Find(name);
  if (!node) return fallback;
  auto number = node->AsNumber();
  return number ? std::clamp(static_cast<float>(*number), low, high) : fallback;
}
std::wstring ReadWide(const Json* parent, std::string_view name, const std::wstring& fallback) {
  if (!parent) return fallback;
  const Json* node = parent->Find(name);
  if (!node) return fallback;
  auto string = node->AsString();
  return string ? FromUtf8(*string) : fallback;
}
uint32_t ReadColor(const Json* parent, std::string_view name, uint32_t fallback) {
  const std::wstring text = ReadWide(parent, name, L"");
  if (text.size() != 9 || text[0] != L'#') return fallback;
  uint32_t value = 0;
  for (size_t i = 1; i < text.size(); ++i) {
    value <<= 4;
    const wchar_t c = text[i];
    if (c >= L'0' && c <= L'9') value += c - L'0';
    else if (c >= L'a' && c <= L'f') value += c - L'a' + 10;
    else if (c >= L'A' && c <= L'F') value += c - L'A' + 10;
    else return fallback;
  }
  return value;
}

void LoadStroke(const Json* json, StrokeSetting& stroke) {
  stroke.color.rgba = ReadColor(json, "color", stroke.color.rgba);
  stroke.width = ReadFloat(json, "width", stroke.width, 1.0f, 128.0f);
  stroke.opacity = ReadFloat(json, "opacity", stroke.opacity, 0.0f, 1.0f);
}

void LoadShape(const Json* json, ShapeSetting& shape) {
  if (!json) return;
  LoadStroke(json->Find("stroke"), shape.stroke);
  shape.fill.rgba = ReadColor(json, "fillColor", shape.fill.rgba);
  shape.fillOpacity = ReadFloat(json, "fillOpacity", shape.fillOpacity, 0.0f, 1.0f);
}

std::string ColorText(uint32_t rgba) {
  char buffer[10]{};
  snprintf(buffer, sizeof(buffer), "#%08X", rgba);
  return buffer;
}

void WriteStroke(std::ostringstream& stream, const StrokeSetting& stroke, int indent) {
  const std::string pad(static_cast<size_t>(indent), ' ');
  stream << "{\n" << pad << "  \"color\": \"" << ColorText(stroke.color.rgba) << "\",\n"
         << pad << "  \"width\": " << stroke.width << ",\n"
         << pad << "  \"opacity\": " << stroke.opacity << "\n" << pad << "}";
}

void WriteShape(std::ostringstream& stream, const ShapeSetting& shape, int indent) {
  const std::string pad(static_cast<size_t>(indent), ' ');
  stream << "{\n" << pad << "  \"stroke\": ";
  WriteStroke(stream, shape.stroke, indent + 2);
  stream << ",\n" << pad << "  \"fillColor\": \"" << ColorText(shape.fill.rgba) << "\",\n"
         << pad << "  \"fillOpacity\": " << shape.fillOpacity << "\n" << pad << "}";
}

}  // namespace

std::filesystem::path DefaultOutputDirectory() {
  PWSTR raw = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
    const std::filesystem::path pictures(raw);
    CoTaskMemFree(raw);
    return pictures / L"RCSS";
  }

  wchar_t profile[MAX_PATH]{};
  const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, _countof(profile));
  if (length > 0 && length < _countof(profile)) {
    return std::filesystem::path(profile) / L"Pictures" / L"RCSS";
  }
  return std::filesystem::current_path() / L"RCSS";
}

ConfigStore::ConfigStore(std::filesystem::path executablePath) {
  executableDirectory_ = std::filesystem::absolute(executablePath).parent_path();
  path_ = executableDirectory_ / L"RC-ScreenShot.config.json";
}

AppConfig ConfigStore::Load(std::wstring* warning) const {
  AppConfig config;
  std::ifstream input(path_, std::ios::binary);
  if (!input) return config;
  std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
      static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) {
    source.erase(0, 3);
  }
  auto rootValue = JsonParser(source).Parse();
  if (!rootValue || !rootValue->AsObject()) {
    if (warning) *warning = L"配置文件格式无效，已使用默认设置。";
    return config;
  }
  const Json* root = &*rootValue;
  const int storedSchema = ReadInt(root, "schemaVersion", 1, 1, 3);
  config.schemaVersion = storedSchema;
  if (const Json* keys = root->Find("hotkeys"); keys && keys->AsArray()) {
    std::vector<HotkeySetting> parsed;
    for (const Json& entry : *keys->AsArray()) {
      HotkeySetting hotkey;
      hotkey.modifiers = static_cast<UINT>(ReadInt(&entry, "modifiers", hotkey.modifiers, 0, 0xFFFF));
      hotkey.virtualKey = static_cast<UINT>(ReadInt(&entry, "virtualKey", hotkey.virtualKey, 1, 0xFF));
      hotkey.enabled = ReadBool(&entry, "enabled", true);
      parsed.push_back(hotkey);
    }
    std::vector<HotkeySetting> normalized;
    for (const HotkeySetting& key : parsed) {
      if (!key.enabled || normalized.size() >= 2) continue;
      normalized.push_back(key);
    }
    if (!normalized.empty()) config.hotkeys = std::move(normalized);
  }
  if (const Json* startup = root->Find("startup")) {
    config.launchAtLogin = ReadBool(startup, "launchAtLogin", config.launchAtLogin);
    config.silentAtLogin = ReadBool(startup, "silentAtLogin", config.silentAtLogin);
  }
  if (const Json* output = root->Find("output")) {
    config.outputDirectory = ReadWide(output, "directory", config.outputDirectory);
    config.filenameTemplate = ReadWide(output, "filenameTemplate", config.filenameTemplate);
    config.jpegQuality = ReadInt(output, "jpegQuality", config.jpegQuality, 1, 100);
    config.autoSaveOnCopy = ReadBool(output, "autoSaveOnCopy", config.autoSaveOnCopy);
    config.defaultAction = ReadWide(output, "defaultAction", L"copy") == L"save"
                               ? DefaultAction::Save : DefaultAction::Copy;
  }
  if (const Json* editor = root->Find("editor")) {
    LoadStroke(editor->Find("pen"), config.pen);
    LoadShape(editor->Find("rectangle"), config.rectangle);
    LoadShape(editor->Find("ellipse"), config.ellipse);
    LoadStroke(editor->Find("line"), config.line);
    LoadStroke(editor->Find("arrow"), config.arrow);
    if (const Json* text = editor->Find("text")) {
      config.text.color.rgba = ReadColor(text, "color", config.text.color.rgba);
      config.text.size = ReadFloat(text, "size", config.text.size, 8.0f, 256.0f);
      config.text.opacity = ReadFloat(text, "opacity", config.text.opacity, 0.0f, 1.0f);
      config.text.vertical = ReadBool(text, "vertical", config.text.vertical);
      config.text.shadow = ReadBool(text, "shadow", config.text.shadow);
      config.text.fontFamily = ReadWide(text, "fontFamily", config.text.fontFamily);
    }
    LoadStroke(editor->Find("frame"), config.frame);
    config.frameEnabled = ReadBool(editor, "frameEnabled", config.frameEnabled);
    if (const Json* mosaic = editor->Find("mosaic")) {
      config.mosaicStyle = ReadWide(mosaic, "style", L"pixel") == L"blur"
                               ? MosaicStyle::Blur : MosaicStyle::Pixel;
      config.mosaicBrushSize = ReadFloat(mosaic, "brushSize", config.mosaicBrushSize, 4.0f, 256.0f);
      config.mosaicPixelSize = ReadInt(mosaic, "pixelSize", config.mosaicPixelSize, 2, 128);
      config.mosaicBlurRadius = ReadFloat(mosaic, "blurRadius", config.mosaicBlurRadius, 1.0f, 64.0f);
    }
  }
  if (const Json* window = root->Find("windowCapture")) {
    config.windowShadow = ReadBool(window, "shadow", config.windowShadow);
  }
  if (const Json* ui = root->Find("ui")) {
    config.language = ReadWide(ui, "language", config.language);
    config.theme = ReadWide(ui, "theme", config.theme);
    config.toolbarPosition = ReadWide(ui, "toolbarPosition", config.toolbarPosition);
  }
  // Version 1 used a portable-folder-relative path and JPEG quality 95 as
  // defaults.  Migrate only those exact legacy defaults so user-customized
  // settings remain untouched.
  if (storedSchema < 2) {
    if (config.outputDirectory.empty() || config.outputDirectory == L"Screenshots")
      config.outputDirectory = DefaultOutputDirectory().wstring();
    if (config.jpegQuality == 95) config.jpegQuality = 80;
  }
  if (config.outputDirectory.empty()) config.outputDirectory = DefaultOutputDirectory().wstring();
  return config;
}

bool ConfigStore::Save(const AppConfig& config, std::wstring* error) const {
  std::ostringstream stream;
  stream << "{\n  \"schemaVersion\": 3,\n  \"hotkeys\": [";
  size_t writtenHotkeys = 0;
  for (const auto& key : config.hotkeys) {
    if (!key.enabled || writtenHotkeys >= 2) continue;
    stream << (writtenHotkeys++ == 0 ? "\n    " : ",\n    ");
    stream << "{\"modifiers\": " << key.modifiers << ", \"virtualKey\": "
           << key.virtualKey << ", \"enabled\": " << (key.enabled ? "true" : "false") << "}";
  }
  stream << "\n  ],\n  \"startup\": {\"launchAtLogin\": "
         << (config.launchAtLogin ? "true" : "false") << ", \"silentAtLogin\": "
         << (config.silentAtLogin ? "true" : "false") << "},\n  \"output\": {\n"
         << "    \"directory\": \"" << Escape(ToUtf8(config.outputDirectory)) << "\",\n"
         << "    \"filenameTemplate\": \"" << Escape(ToUtf8(config.filenameTemplate)) << "\",\n"
         << "    \"jpegQuality\": " << config.jpegQuality << ",\n"
         << "    \"defaultAction\": \"" << (config.defaultAction == DefaultAction::Save ? "save" : "copy") << "\",\n"
         << "    \"autoSaveOnCopy\": " << (config.autoSaveOnCopy ? "true" : "false") << "\n  },\n"
         << "  \"editor\": {\n    \"pen\": ";
  WriteStroke(stream, config.pen, 4);
  stream << ",\n    \"rectangle\": "; WriteShape(stream, config.rectangle, 4);
  stream << ",\n    \"ellipse\": "; WriteShape(stream, config.ellipse, 4);
  stream << ",\n    \"line\": "; WriteStroke(stream, config.line, 4);
  stream << ",\n    \"arrow\": "; WriteStroke(stream, config.arrow, 4);
  stream << ",\n    \"text\": {\"color\": \"" << ColorText(config.text.color.rgba)
         << "\", \"size\": " << config.text.size
         << ", \"opacity\": " << config.text.opacity
         << ", \"vertical\": " << (config.text.vertical ? "true" : "false")
         << ", \"shadow\": " << (config.text.shadow ? "true" : "false")
         << ", \"fontFamily\": \"" << Escape(ToUtf8(config.text.fontFamily)) << "\"}";
  stream << ",\n    \"frame\": "; WriteStroke(stream, config.frame, 4);
  stream << ",\n    \"frameEnabled\": " << (config.frameEnabled ? "true" : "false");
  stream << ",\n    \"mosaic\": {\"style\": \""
         << (config.mosaicStyle == MosaicStyle::Blur ? "blur" : "pixel")
         << "\", \"brushSize\": " << config.mosaicBrushSize
         << ", \"pixelSize\": " << config.mosaicPixelSize
         << ", \"blurRadius\": " << config.mosaicBlurRadius << "}\n  },\n"
         << "  \"windowCapture\": {\"shadow\": " << (config.windowShadow ? "true" : "false") << "},\n"
         << "  \"ui\": {\"language\": \"" << Escape(ToUtf8(config.language))
         << "\", \"theme\": \"" << Escape(ToUtf8(config.theme))
         << "\", \"toolbarPosition\": \"" << Escape(ToUtf8(config.toolbarPosition)) << "\"}\n}\n";

  const auto temp = path_.wstring() + L".tmp";
  {
    std::ofstream output(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
    if (!output) {
      if (error) *error = L"无法在程序目录写入配置临时文件。";
      return false;
    }
    const std::string text = stream.str();
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
      if (error) *error = L"写入配置文件失败。";
      return false;
    }
  }
  if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temp.c_str());
    if (error) *error = L"无法原子替换配置文件：" + HResultMessage(HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

std::filesystem::path ConfigStore::ResolveOutputDirectory(const AppConfig& config) const {
  std::filesystem::path directory = config.outputDirectory.empty()
                                        ? DefaultOutputDirectory()
                                        : std::filesystem::path(config.outputDirectory);
  return directory.is_absolute() ? directory : executableDirectory_ / directory;
}

std::wstring FormatHotkey(const HotkeySetting& hotkey) {
  std::wstring text;
  if (hotkey.modifiers & MOD_CONTROL) text += L"Ctrl+";
  if (hotkey.modifiers & MOD_ALT) text += L"Alt+";
  if (hotkey.modifiers & MOD_SHIFT) text += L"Shift+";
  if (hotkey.modifiers & MOD_WIN) text += L"Win+";
  if (hotkey.virtualKey == VK_OEM_5) return text + L"\\";
  wchar_t keyName[64]{};
  const UINT scan = MapVirtualKeyW(hotkey.virtualKey, MAPVK_VK_TO_VSC) << 16;
  if (GetKeyNameTextW(static_cast<LONG>(scan), keyName, 64)) text += keyName;
  else text += L"VK_" + std::to_wstring(hotkey.virtualKey);
  return text;
}

bool ParseHotkeyText(std::wstring_view text, HotkeySetting& out) {
  std::wstring normalized(text);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), towlower);
  out = {};
  out.modifiers = MOD_NOREPEAT;
  const auto trim = [](std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return std::wstring{};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
  };
  const auto parseKey = [](const std::wstring& token, UINT& virtualKey) {
    if (token == L"\\") { virtualKey = VK_OEM_5; return true; }
    if (token.size() == 1 && token[0] >= L'a' && token[0] <= L'z') {
      virtualKey = token[0] - L'a' + 'A'; return true;
    }
    if (token.size() == 1 && token[0] >= L'0' && token[0] <= L'9') {
      virtualKey = token[0]; return true;
    }
    if (token.size() >= 2 && token[0] == L'f') {
      const int number = _wtoi(token.c_str() + 1);
      if (number >= 1 && number <= 24 && token == L"f" + std::to_wstring(number)) {
        virtualKey = VK_F1 + number - 1; return true;
      }
    }
    const std::array<std::pair<std::wstring_view, UINT>, 19> names{{
        {L"esc", VK_ESCAPE}, {L"escape", VK_ESCAPE}, {L"space", VK_SPACE},
        {L"tab", VK_TAB}, {L"enter", VK_RETURN}, {L"return", VK_RETURN},
        {L"backspace", VK_BACK}, {L"delete", VK_DELETE}, {L"del", VK_DELETE},
        {L"insert", VK_INSERT}, {L"home", VK_HOME}, {L"end", VK_END},
        {L"pageup", VK_PRIOR}, {L"pagedown", VK_NEXT}, {L"up", VK_UP},
        {L"down", VK_DOWN}, {L"left", VK_LEFT}, {L"right", VK_RIGHT},
        {L"capslock", VK_CAPITAL}}};
    for (const auto& [name, value] : names) {
      if (token == name) { virtualKey = value; return true; }
    }
    if (token.rfind(L"vk_", 0) == 0) {
      const int value = _wtoi(token.c_str() + 3);
      if (value > 0 && value <= 0xFF) { virtualKey = static_cast<UINT>(value); return true; }
    }
    return false;
  };
  size_t start = 0;
  while (start < normalized.size()) {
    const size_t end = normalized.find(L'+', start);
    const std::wstring token = trim(normalized.substr(start, end == std::wstring::npos ? end : end - start));
    if (token == L"ctrl" || token == L"control") out.modifiers |= MOD_CONTROL;
    else if (token == L"alt") out.modifiers |= MOD_ALT;
    else if (token == L"shift") out.modifiers |= MOD_SHIFT;
    else if (token == L"win" || token == L"windows") out.modifiers |= MOD_WIN;
    else if (!parseKey(token, out.virtualKey)) return false;
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return out.virtualKey != 0 && (out.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN));
}

}  // namespace rc
