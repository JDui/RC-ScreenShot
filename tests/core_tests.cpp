#include "capture.hpp"
#include "config.hpp"
#include "editor.hpp"
#include "exporter.hpp"
#include "unit_detector.hpp"
#include "uia_detector.hpp"

#include <fstream>
#include <iostream>
#include <wincodec.h>

namespace {

int failures = 0;

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #condition "\n"; ++failures; \
} } while (false)

bool LoadBgra(const std::filesystem::path& path, std::vector<uint8_t>& pixels,
              int& width, int& height) {
  rc::ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)))) return false;
  rc::ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder))) return false;
  rc::ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return false;
  rc::ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) return false;
  UINT imageWidth = 0, imageHeight = 0;
  if (FAILED(converter->GetSize(&imageWidth, &imageHeight))) return false;
  width = static_cast<int>(imageWidth); height = static_cast<int>(imageHeight);
  pixels.resize(static_cast<size_t>(width * height * 4));
  return SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(width * 4),
                                         static_cast<UINT>(pixels.size()), pixels.data()));
}

void TestHotkeys() {
  const rc::AppConfig defaults;
  CHECK(defaults.hotkeys.size() == 2);
  CHECK(rc::FormatHotkey(defaults.hotkeys[0]) == L"Ctrl+\\");
  CHECK(rc::FormatHotkey(defaults.hotkeys[1]) == L"Alt+\\");
  rc::HotkeySetting key;
  CHECK(rc::ParseHotkeyText(L"Ctrl+\\", key));
  CHECK((key.modifiers & MOD_CONTROL) != 0);
  CHECK(key.virtualKey == VK_OEM_5);
  CHECK(rc::FormatHotkey(key) == L"Ctrl+\\");
  CHECK(rc::ParseHotkeyText(L"Ctrl+Shift+A", key));
  CHECK(key.virtualKey == 'A');
  CHECK(!rc::ParseHotkeyText(L"JustSomeKey", key));
}

void TestConfigRoundTrip() {
  wchar_t tempRoot[MAX_PATH]{};
  GetTempPathW(MAX_PATH, tempRoot);
  const auto directory = std::filesystem::path(tempRoot) /
      (L"RC-ScreenShot-tests-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(directory);
  rc::ScopeExit cleanup{[&] { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }};
  rc::ConfigStore store(directory / L"RC-ScreenShot.exe");
  rc::AppConfig config;
  config.hotkeys.push_back({MOD_ALT | MOD_NOREPEAT, 'Q', true});
  config.burstCount = 12;
  config.burstIntervalSeconds = 0.37f;
  config.jpegQuality = 87;
  config.outputDirectory = L"我的截图";
  config.autoSaveOnCopy = true;
  config.frameEnabled = true;
  config.mosaicStyle = rc::MosaicStyle::Blur;
  config.text.size = 42;
  config.text.opacity = 0.75f;
  config.text.vertical = true;
  config.text.shadow = true;
  config.text.color.rgba = 0x34C759FF;
  std::wstring error;
  CHECK(store.Save(config, &error));
  const rc::AppConfig loaded = store.Load(&error);
  CHECK(loaded.schemaVersion == 4);
  CHECK(loaded.hotkeys.size() == 2);
  CHECK(loaded.burstCount == 12);
  CHECK(std::abs(loaded.burstIntervalSeconds - 0.37f) < 0.001f);
  CHECK(loaded.jpegQuality == 87);
  CHECK(loaded.outputDirectory == L"我的截图");
  CHECK(loaded.autoSaveOnCopy);
  CHECK(loaded.frameEnabled);
  CHECK(loaded.mosaicStyle == rc::MosaicStyle::Blur);
  CHECK(loaded.text.size == 42);
  CHECK(std::abs(loaded.text.opacity - 0.75f) < 0.001f);
  CHECK(loaded.text.vertical);
  CHECK(loaded.text.shadow);
  CHECK(loaded.text.color.rgba == 0x34C759FF);
}

void TestConfigDamagedFieldRecovery() {
  wchar_t tempRoot[MAX_PATH]{};
  GetTempPathW(MAX_PATH, tempRoot);
  const auto directory = std::filesystem::path(tempRoot) /
      (L"RC-ScreenShot-damaged-config-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(directory);
  rc::ScopeExit cleanup{[&] { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }};
  rc::ConfigStore store(directory / L"RC-ScreenShot.exe");
  std::ofstream output(store.path(), std::ios::binary | std::ios::trunc);
  output << R"({"schemaVersion":1,"startup":{"launchAtLogin":true},"output":{"jpegQuality":"bad","autoSaveOnCopy":true}})";
  output.close();
  const rc::AppConfig loaded = store.Load();
  CHECK(loaded.launchAtLogin);
  CHECK(loaded.autoSaveOnCopy);
  CHECK(loaded.jpegQuality == 80);
  CHECK(loaded.hotkeys.size() == 2);
  CHECK(loaded.burstCount == 6);
  CHECK(std::abs(loaded.burstIntervalSeconds - 0.08f) < 0.001f);

  output.open(store.path(), std::ios::binary | std::ios::trunc);
  output << R"({"schemaVersion":4,"burst":{"count":99,"intervalSeconds":0.001}})";
  output.close();
  const rc::AppConfig clamped = store.Load();
  CHECK(clamped.burstCount == 30);
  CHECK(std::abs(clamped.burstIntervalSeconds - 0.05f) < 0.001f);
}

void TestEditorHistory() {
  rc::EditorDocument document;
  rc::PenCommand pen{{{1, 2}, {3, 4}}, {}};
  document.Add(pen);
  document.Add(rc::ShapeCommand{});
  CHECK(document.Commands().size() == 2);
  CHECK(document.Undo());
  CHECK(document.Commands().size() == 1);
  CHECK(document.Redo());
  CHECK(document.Commands().size() == 2);
  CHECK(document.Undo());
  document.Add(rc::MosaicCommand{});
  CHECK(!document.CanRedo());
  CHECK(document.Commands().size() == 2);
  document.Add(rc::TextCommand{{5, 6}, L"文字", {}});
  CHECK(document.Commands().size() == 3);
}

void TestBlankCaptureGuard() {
  rc::DesktopSnapshot snapshot;
  snapshot.virtualBounds = {0, 0, 16, 16};
  snapshot.width = 16;
  snapshot.height = 16;
  snapshot.bgraStride = snapshot.width * 4;
  snapshot.bgra.assign(static_cast<size_t>(snapshot.bgraStride * snapshot.height), 0);
  for (size_t index = 3; index < snapshot.bgra.size(); index += 4) snapshot.bgra[index] = 255;
  CHECK(rc::SnapshotIsBlank(snapshot));

  // Values below four are treated as harmless capture noise; a genuinely visible pixel must
  // make the snapshot usable.
  snapshot.bgra[8 * snapshot.bgraStride + 8 * 4 + 1] = 32;
  CHECK(!rc::SnapshotIsBlank(snapshot));

  rc::DesktopSnapshot invalid;
  CHECK(rc::SnapshotIsBlank(invalid));
}

void TestPenPressureCurve() {
  const float slow = rc::PenWidthScaleForSpeed(0.0f);
  const float medium = rc::PenWidthScaleForSpeed(500.0f);
  const float fast = rc::PenWidthScaleForSpeed(1400.0f);
  CHECK(slow > medium);
  CHECK(medium > fast);
  CHECK(fast >= 0.5f);

  rc::StrokeSetting style;
  style.width = 10.0f;
  rc::PenCommand pen{{{0, 0}, {20, 0}, {40, 0}}, style, {1.35f, 0.9f, 0.55f}};
  CHECK(std::abs(rc::PenPointWidth(pen, 0) - 13.5f) < 0.01f);
  CHECK(std::abs(rc::PenPointWidth(pen, 2) - 5.5f) < 0.01f);
  CHECK(std::abs(rc::PenMaximumWidth(pen) - 13.5f) < 0.01f);
}

void TestTextRendering() {
  rc::DesktopSnapshot snapshot;
  snapshot.virtualBounds = {0, 0, 240, 120};
  snapshot.width = 240; snapshot.height = 120; snapshot.bgraStride = 240 * 4;
  snapshot.bgra.resize(static_cast<size_t>(snapshot.bgraStride * snapshot.height), 255);
  snapshot.hdrRgba.resize(static_cast<size_t>(snapshot.width * snapshot.height * 4), rc::FloatToHalf(1.0f));
  rc::EditorDocument document;
  rc::TextSetting style;
  style.color.rgba = 0xFF0000FF; style.size = 36; style.vertical = false;
  document.Add(rc::TextCommand{{12, 10}, L"RC文字", style});
  rc::ImageExporter exporter;
  rc::RenderedImage image;
  rc::AppConfig config;
  std::wstring error;
  CHECK(exporter.Render(snapshot, snapshot.virtualBounds, document, config, false, image, error));
  size_t redPixels = 0;
  for (size_t i = 0; i + 3 < image.bgra.size(); i += 4) {
    if (image.bgra[i + 2] > 180 && image.bgra[i + 1] < 180 && image.bgra[i] < 180) ++redPixels;
  }
  CHECK(redPixels > 20);

  // A pen path exercises the Direct2D geometry/stroke factory domain used by the editor.
  // Keep this regression test next to text rendering because both are exported vector content.
  rc::EditorDocument penDocument;
  rc::StrokeSetting penStyle;
  penStyle.color.rgba = 0xFF0000FF;
  penStyle.width = 8.0f;
  penDocument.Add(rc::PenCommand{{{20, 70}, {60, 70}, {100, 82}}, penStyle,
                                 {1.35f, 0.9f, 0.55f}});
  error.clear();
  CHECK(exporter.Render(snapshot, snapshot.virtualBounds, penDocument, config, false, image, error));
  size_t penPixels = 0;
  for (size_t i = 0; i + 3 < image.bgra.size(); i += 4) {
    if (image.bgra[i + 2] > 180 && image.bgra[i + 1] < 180 && image.bgra[i] < 180) ++penPixels;
  }
  CHECK(penPixels > 30);
}

void TestMosaic() {
  constexpr int width = 32, height = 32, stride = width * 4;
  std::vector<uint8_t> image(static_cast<size_t>(stride * height));
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    uint8_t* p = image.data() + y * stride + x * 4;
    p[0] = static_cast<uint8_t>(x * 7); p[1] = static_cast<uint8_t>(y * 7); p[2] = 120; p[3] = 255;
  }
  const auto original = image;
  rc::MosaicCommand mosaic;
  mosaic.brush = false; mosaic.bounds = {8, 8, 24, 24}; mosaic.pixelSize = 8;
  std::vector<rc::EditCommand> commands{mosaic};
  rc::ApplyMosaics(image, width, height, stride, commands);
  CHECK(image[0] == original[0]);
  CHECK(image[10 * stride + 10 * 4] != original[10 * stride + 10 * 4]);
  CHECK(image[10 * stride + 10 * 4] == image[11 * stride + 11 * 4]);

  image = original;
  mosaic.brush = true;
  mosaic.points = {{16, 16}};
  mosaic.bounds = {};
  mosaic.brushSize = 10.0f;
  mosaic.pixelSize = 4;
  rc::ApplyMosaics(image, width, height, stride, commands);
  CHECK(image[16 * stride + 16 * 4] != original[16 * stride + 16 * 4]);
  CHECK(image[0] == original[0]);

  image = original;
  mosaic.style = rc::MosaicStyle::Blur;
  mosaic.blurRadius = 6.0f;
  rc::ApplyMosaics(image, width, height, stride, commands);
  CHECK(image[16 * stride + 16 * 4] != original[16 * stride + 16 * 4]);
}

void TestMosaicBlur() {
  // A vertical bar (B=200) on a flat background (B=50) makes the blur visibly non-invariant:
  // a masked pixel just left of the bar blends the two, while far pixels stay untouched.
  // Keep seven bytes of row padding so the implementation is exercised with a stride
  // that is not width*4; those bytes must remain untouched by either blur pass.
  constexpr int width = 48, height = 48, stride = width * 4 + 7;
  std::vector<uint8_t> image(static_cast<size_t>(stride * height), 0xA5);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    uint8_t* p = image.data() + static_cast<size_t>(y * stride + x * 4);
    p[0] = (x >= 16 && x < 32) ? 200 : 50;
    p[1] = 120; p[2] = 120; p[3] = 255;
  }
  const auto original = image;
  rc::MosaicCommand mosaic;
  mosaic.brush = false; mosaic.bounds = {8, 8, 40, 40};
  mosaic.style = rc::MosaicStyle::Blur; mosaic.blurRadius = 5.0f;
  std::vector<rc::EditCommand> commands{mosaic};
  rc::ApplyMosaics(image, width, height, stride, commands);

  // (12,24) is inside the mask and 4px left of the bar; blurring must pull the two colors
  // together, so the channel sits strictly between the background (50) and the bar (200).
  const uint8_t* blended = image.data() + static_cast<size_t>(24 * stride + 12 * 4);
  CHECK(blended[0] > 60 && blended[0] < 190);
  CHECK(blended[0] != original[24 * stride + 12 * 4]);
  CHECK(blended[1] == 120 && blended[2] == 120 && blended[3] == 255);
  // The final low-resolution [1,2,1] pass should spread the transition over
  // neighboring samples rather than leave a one-pixel pulse.  Keep this
  // assertion in channel space so it also catches accidental alpha/stride
  // writes without depending on a particular mip level count.
  const auto blueAt = [&](int x) -> int {
    return image[static_cast<size_t>(24 * stride + x * 4)];
  };
  const int edge0 = blueAt(12);
  const int edge1 = blueAt(13);
  const int edge2 = blueAt(14);
  CHECK(edge0 < edge1 && edge1 < edge2);
  CHECK(edge1 - edge0 < 48 && edge2 - edge1 < 48);
  // Far from the mask nothing changed.
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
    const uint8_t* p = image.data() + static_cast<size_t>(y * stride + x * 4);
    const uint8_t* o = original.data() + static_cast<size_t>(y * stride + x * 4);
    CHECK(p[0] == o[0] && p[1] == o[1] && p[2] == o[2] && p[3] == o[3]);
  }
  // Every pixel outside the rectangular mask and every row-padding byte must be
  // byte-for-byte identical to the source image.
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (x >= 8 && x < 40 && y >= 8 && y < 40) continue;
      const uint8_t* p = image.data() + static_cast<size_t>(y * stride + x * 4);
      const uint8_t* o = original.data() + static_cast<size_t>(y * stride + x * 4);
      CHECK(std::equal(p, p + 4, o));
    }
    CHECK(std::equal(image.data() + static_cast<size_t>(y * stride + width * 4),
                     image.data() + static_cast<size_t>((y + 1) * stride),
                     original.data() + static_cast<size_t>(y * stride + width * 4)));
  }

  // Brush blur must also alter the stroke pixels without a crash or buffer overrun. (16,16)
  // sits on the bar edge, so blurring pulls the background into it.
  image = original;
  mosaic.brush = true;
  mosaic.points = {{16, 16}};
  mosaic.bounds = {};
  mosaic.brushSize = 10.0f;
  commands[0] = mosaic;
  rc::ApplyMosaics(image, width, height, stride, commands);
  CHECK(image[16 * stride + 16 * 4] != original[16 * stride + 16 * 4]);
  CHECK(image[0] == original[0]);
  for (int y = 0; y < height; ++y) {
    CHECK(std::equal(image.data() + static_cast<size_t>(y * stride + width * 4),
                     image.data() + static_cast<size_t>((y + 1) * stride),
                     original.data() + static_cast<size_t>(y * stride + width * 4)));
  }
}

void TestUnitDetection() {
  constexpr int width = 480, height = 320, stride = width * 4;
  std::vector<uint8_t> image(static_cast<size_t>(stride * height), 245);
  for (int y = 50; y <= 250; ++y) for (int x = 80; x <= 400; ++x) {
    uint8_t* p = image.data() + y * stride + x * 4;
    const bool border = x == 80 || x == 400 || y == 50 || y == 250;
    p[0] = p[1] = p[2] = border ? 10 : 220; p[3] = 255;
  }
  rc::UnitDetector detector;
  const auto candidates = detector.Detect(image, width, height, stride);
  CHECK(!candidates.empty());
  const auto chain = detector.CandidatesAt(candidates, {200, 150});
  CHECK(!chain.empty());
  bool containsPanel = false;
  for (size_t index : chain) {
    const RECT r = candidates[index].bounds;
    if (r.left <= 90 && r.right >= 390 && r.top <= 60 && r.bottom >= 240) containsPanel = true;
  }
  CHECK(containsPanel);
}

void TestUnitDetectionRejectsFalseGrid() {
  constexpr int width = 480, height = 320, stride = width * 4;
  std::vector<uint8_t> image(static_cast<size_t>(stride * height), 245);
  const auto drawRect = [&](int left, int top, int right, int bottom) {
    for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) {
      if (x != left && x != right && y != top && y != bottom) continue;
      uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
      pixel[0] = pixel[1] = pixel[2] = 10; pixel[3] = 255;
    }
  };
  drawRect(40, 40, 220, 180);
  drawRect(250, 30, 300, 80);
  // A short separator/text-like mark must not split the surrounding panel.
  for (int x = 80; x <= 155; ++x) {
    uint8_t* pixel = image.data() + static_cast<size_t>(110 * stride + x * 4);
    pixel[0] = pixel[1] = pixel[2] = 10;
  }

  rc::UnitDetector detector;
  const auto candidates = detector.Detect(image, width, height, stride);
  const auto chain = detector.CandidatesAt(candidates, {190, 150});
  CHECK(!chain.empty());
  const RECT selected = candidates[chain.front()].bounds;
  CHECK(selected.left <= 45 && selected.top <= 45);
  CHECK(selected.right >= 215 && selected.bottom >= 175);
  const auto smallChain = detector.CandidatesAt(candidates, {275, 55});
  CHECK(!smallChain.empty());
  const RECT smallSelected = candidates[smallChain.front()].bounds;
  CHECK(smallSelected.left <= 255 && smallSelected.top <= 35);
  CHECK(smallSelected.right >= 295 && smallSelected.bottom >= 75);
}

void TestUnitDetectionAsymmetricGridOuterBounds() {
  constexpr int width = 520, height = 360, stride = width * 4;
  std::vector<uint8_t> image(static_cast<size_t>(stride * height), 242);
  const auto darken = [&](int x, int y) {
    uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
    pixel[0] = pixel[1] = pixel[2] = 30; pixel[3] = 255;
  };
  for (int x = 40; x <= 460; ++x) {
    for (int y : {35, 120, 205, 300}) darken(x, y);
  }
  for (int y = 35; y <= 300; ++y) {
    for (int x : {40, 180, 320, 460}) darken(x, y);
  }
  rc::UnitDetector detector;
  const auto candidates = detector.Detect(image, width, height, stride);
  const auto chain = detector.CandidatesAt(candidates, {100, 80});
  CHECK(!chain.empty());
  CHECK(std::any_of(chain.begin(), chain.end(), [&](size_t index) {
    const RECT& r = candidates[index].bounds;
    return r.left <= 45 && r.top <= 40 && r.right >= 455 && r.bottom >= 295;
  }));
}

void TestUnitDetectionEllipse() {
  constexpr int width = 520, height = 360, stride = width * 4;
  std::vector<uint8_t> image(static_cast<size_t>(stride * height), 245);
  const auto drawFilledEllipse = [&](int centerX, int centerY, int radiusX, int radiusY) {
    for (int y = centerY - radiusY; y <= centerY + radiusY; ++y) {
      for (int x = centerX - radiusX; x <= centerX + radiusX; ++x) {
        const float dx = static_cast<float>(x - centerX) / radiusX;
        const float dy = static_cast<float>(y - centerY) / radiusY;
        if (dx * dx + dy * dy > 1.0f) continue;
        uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
        pixel[0] = pixel[1] = pixel[2] = 24; pixel[3] = 255;
      }
    }
  };
  drawFilledEllipse(130, 110, 54, 54);
  drawFilledEllipse(350, 210, 62, 88);
  drawFilledEllipse(475, 55, 12, 12);

  rc::UnitDetector detector;
  const auto candidates = detector.Detect(image, width, height, stride);
  const auto circleChain = detector.CandidatesAt(candidates, {130, 110});
  const auto ellipseChain = detector.CandidatesAt(candidates, {350, 210});
  const auto iconChain = detector.CandidatesAt(candidates, {475, 55});
  CHECK(!circleChain.empty());
  CHECK(!ellipseChain.empty());
  CHECK(!iconChain.empty());
  const RECT circle = candidates[circleChain.front()].bounds;
  const RECT ellipse = candidates[ellipseChain.front()].bounds;
  CHECK(circle.left <= 80 && circle.top <= 60 && circle.right >= 180 && circle.bottom >= 160);
  CHECK(ellipse.left <= 292 && ellipse.top <= 126 && ellipse.right >= 408 && ellipse.bottom >= 294);
  const RECT icon = candidates[iconChain.front()].bounds;
  CHECK(icon.left <= 465 && icon.top <= 45 && icon.right >= 485 && icon.bottom >= 65);
}

void TestUnitDetectionRoundedRectangles() {
  constexpr int width = 560, height = 340, stride = width * 4;
  std::vector<uint8_t> image(static_cast<size_t>(stride * height), 112);
  for (size_t i = 3; i < image.size(); i += 4) image[i] = 255;
  const auto drawRoundedRect = [&](int left, int top, int right, int bottom, int radius,
                                   uint8_t luma) {
    for (int y = top; y <= bottom; ++y) {
      for (int x = left; x <= right; ++x) {
        const int nearestX = std::clamp(x, left + radius, right - radius);
        const int nearestY = std::clamp(y, top + radius, bottom - radius);
        const int dx = x - nearestX, dy = y - nearestY;
        if (dx * dx + dy * dy > radius * radius) continue;
        uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
        pixel[0] = pixel[1] = pixel[2] = luma;
      }
    }
  };
  // Ten luma levels model subtle dark-theme cards that the old threshold missed.
  drawRoundedRect(45, 40, 255, 165, 24, 122);
  drawRoundedRect(340, 205, 440, 275, 18, 126);

  rc::UnitDetector detector;
  const auto candidates = detector.Detect(image, width, height, stride);
  const auto largeChain = detector.CandidatesAt(candidates, {150, 100});
  const auto smallChain = detector.CandidatesAt(candidates, {390, 240});
  CHECK(!largeChain.empty());
  CHECK(!smallChain.empty());
  const RECT large = candidates[largeChain.front()].bounds;
  const RECT smallRect = candidates[smallChain.front()].bounds;
  CHECK(large.left <= 50 && large.top <= 45 && large.right >= 250 && large.bottom >= 160);
  CHECK(smallRect.left <= 345 && smallRect.top <= 210 &&
        smallRect.right >= 435 && smallRect.bottom >= 270);
}

void TestUnitDetectionColorAndHighResolution() {
  constexpr int width = 2400, height = 1400, stride = width * 4;
  std::vector<uint8_t> image(static_cast<size_t>(stride * height));
  for (size_t i = 0; i < image.size(); i += 4) {
    image[i] = image[i + 1] = image[i + 2] = 112; image[i + 3] = 255;
  }
  const auto drawRoundedColor = [&](int left, int top, int right, int bottom, int radius,
                                    std::array<uint8_t, 3> bgr) {
    for (int y = top; y <= bottom; ++y) {
      for (int x = left; x <= right; ++x) {
        const int nearestX = std::clamp(x, left + radius, right - radius);
        const int nearestY = std::clamp(y, top + radius, bottom - radius);
        const int dx = x - nearestX, dy = y - nearestY;
        if (dx * dx + dy * dy > radius * radius) continue;
        uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
        pixel[0] = bgr[0]; pixel[1] = bgr[1]; pixel[2] = bgr[2];
      }
    }
  };
  // Nearly identical grayscale luminance but clear RGB separation.
  drawRoundedColor(1800, 1000, 1860, 1034, 8, {132, 102, 124});
  rc::UnitDetector detector;
  const auto candidates = detector.Detect(image, width, height, stride);
  const auto chain = detector.CandidatesAt(candidates, {1830, 1017});
  CHECK(!chain.empty());
  const RECT selected = candidates[chain.front()].bounds;
  CHECK(selected.left <= 1805 && selected.top <= 1005);
  CHECK(selected.right >= 1855 && selected.bottom >= 1029);
}

void TestUnitDetectionRealSettingsUi() {
  const auto imagePath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                         L"assets" / L"feature-settings.png";
  std::vector<uint8_t> image;
  int width = 0, height = 0;
  CHECK(LoadBgra(imagePath, image, width, height));
  if (image.empty()) return;
  rc::UnitDetector detector;
  const auto candidates = detector.Detect(image, width, height, width * 4);
  const auto hasBounds = [&](int left, int top, int right, int bottom, int tolerance) {
    return std::any_of(candidates.begin(), candidates.end(), [&](const rc::UnitCandidate& candidate) {
      const RECT& r = candidate.bounds;
      return std::abs(r.left - left) <= tolerance && std::abs(r.top - top) <= tolerance &&
             std::abs(r.right - right) <= tolerance && std::abs(r.bottom - bottom) <= tolerance;
    });
  };
  CHECK(hasBounds(8, 45, 614, 187, 4));
  CHECK(hasBounds(8, 193, 308, 341, 8));
  CHECK(hasBounds(312, 193, 614, 341, 4));
  CHECK(hasBounds(8, 347, 614, 427, 4));
}

void TestUiaCandidateNormalization() {
  const RECT virtualBounds{-100, -50, 900, 650};
  const POINT point{50, 50};
  const std::array<RECT, 5> bounds{{
      {-50, -20, 500, 400},
      {20, 30, 120, 90},
      {20, 30, 120, 90},
      {400, 400, 500, 500},
      {48, 48, 52, 52},
  }};
  const auto candidates = rc::UiaDetector::NormalizeCandidates(bounds, point, virtualBounds);
  CHECK(candidates.size() == 2);
  if (candidates.size() != 2) return;
  CHECK(candidates[0].bounds.left == 120);
  CHECK(candidates[0].bounds.top == 80);
  CHECK(candidates[0].bounds.right == 220);
  CHECK(candidates[0].bounds.bottom == 140);
  CHECK(candidates[0].parent == 1);
  CHECK(candidates[1].bounds.left == 50);
  CHECK(candidates[1].bounds.top == 30);
}

void TestCoordinatesAndHdrIntersection() {
  rc::DesktopSnapshot snapshot;
  snapshot.virtualBounds = {-100, -50, 100, 50};
  snapshot.width = 200;
  snapshot.height = 100;
  snapshot.bgraStride = snapshot.width * 4;
  snapshot.bgra.resize(static_cast<size_t>(snapshot.bgraStride * snapshot.height), 255);
  snapshot.hdrRgba.resize(static_cast<size_t>(snapshot.width * snapshot.height * 4), rc::FloatToHalf(1.0f));
  snapshot.hasHdr = true;
  snapshot.hdrRegions.push_back({0, -50, 100, 50});
  for (int y = 0; y < snapshot.height; ++y) {
    for (int x = 0; x < snapshot.width; ++x) {
      uint8_t* pixel = snapshot.bgra.data() + static_cast<size_t>(y * snapshot.bgraStride + x * 4);
      pixel[0] = static_cast<uint8_t>(x); pixel[1] = static_cast<uint8_t>(y); pixel[2] = 20; pixel[3] = 255;
    }
  }
  rc::EditorDocument document;
  rc::AppConfig config;
  rc::ImageExporter exporter;
  rc::RenderedImage image;
  std::wstring error;
  const bool renderedSdr = exporter.Render(snapshot, {-80, -30, -20, 20}, document, config, false, image, error);
  if (!renderedSdr) std::cerr << "SDR render error: " << rc::ToUtf8(error) << '\n';
  CHECK(renderedSdr);
  CHECK(image.width == 60 && image.height == 50);
  CHECK(!image.hasHdr);
  CHECK(image.bgra[0] == 20 && image.bgra[1] == 20 && image.bgra[2] == 20 && image.bgra[3] == 255);
  error.clear();
  const bool renderedHdr = exporter.Render(snapshot, {-20, -30, 20, 20}, document, config, false, image, error);
  if (!renderedHdr) std::cerr << "HDR render error: " << rc::ToUtf8(error) << '\n';
  CHECK(renderedHdr);
  CHECK(image.hasHdr);
}

void TestJpegColorLayout() {
  wchar_t tempRoot[MAX_PATH]{};
  GetTempPathW(MAX_PATH, tempRoot);
  const auto directory = std::filesystem::path(tempRoot) /
      (L"RC-ScreenShot-jpeg-test-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(directory);
  rc::ScopeExit cleanup{[&] { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }};
  rc::RenderedImage source;
  source.width = 16; source.height = 16; source.stride = 16 * 4;
  source.bgra.resize(static_cast<size_t>(source.stride * source.height));
  for (size_t i = 0; i < source.bgra.size(); i += 4) {
    source.bgra[i] = 12; source.bgra[i + 1] = 72; source.bgra[i + 2] = 198; source.bgra[i + 3] = 255;
  }
  rc::ImageExporter exporter;
  std::filesystem::path saved;
  std::wstring error;
  CHECK(exporter.Save(source, directory, L"color.jpg", 100, saved, error));
  rc::ComPtr<IWICImagingFactory> factory;
  CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))));
  rc::ComPtr<IWICBitmapDecoder> decoder;
  CHECK(SUCCEEDED(factory->CreateDecoderFromFilename(saved.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &decoder)));
  rc::ComPtr<IWICBitmapFrameDecode> frame;
  CHECK(SUCCEEDED(decoder->GetFrame(0, &frame)));
  rc::ComPtr<IWICFormatConverter> converter;
  CHECK(SUCCEEDED(factory->CreateFormatConverter(&converter)));
  CHECK(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)));
  WICRect pixelRect{8, 8, 1, 1};
  uint8_t pixel[4]{};
  CHECK(SUCCEEDED(converter->CopyPixels(&pixelRect, 4, 4, pixel)));
  CHECK(std::abs(static_cast<int>(pixel[0]) - 12) < 8);
  CHECK(std::abs(static_cast<int>(pixel[1]) - 72) < 8);
  CHECK(std::abs(static_cast<int>(pixel[2]) - 198) < 8);
}

void TestFilenameAndHalfFloat() {
  SYSTEMTIME time{};
  time.wYear = 2026; time.wMonth = 8; time.wDay = 10;
  time.wHour = 9; time.wMinute = 7; time.wSecond = 5; time.wMilliseconds = 42;
  CHECK(rc::ImageExporter::MakeFilename(L"RC_yyyyMMdd_HHmmss_fff.jpg", time) ==
        L"RC_20260810_090705_042.jpg");
  for (float value : {0.0f, 0.5f, 1.0f, 4.0f, 16.0f}) {
    CHECK(std::abs(rc::HalfToFloat(rc::FloatToHalf(value)) - value) < 0.01f);
  }
}

}  // namespace

int wmain() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  rc::ScopeExit uninitialize{[] { CoUninitialize(); }};
  TestHotkeys();
  TestConfigRoundTrip();
  TestConfigDamagedFieldRecovery();
  TestEditorHistory();
  TestBlankCaptureGuard();
  TestPenPressureCurve();
  TestTextRendering();
  TestMosaic();
  TestMosaicBlur();
  TestUnitDetection();
  TestUnitDetectionRejectsFalseGrid();
  TestUnitDetectionAsymmetricGridOuterBounds();
  TestUnitDetectionEllipse();
  TestUnitDetectionRoundedRectangles();
  TestUnitDetectionColorAndHighResolution();
  TestUnitDetectionRealSettingsUi();
  TestUiaCandidateNormalization();
  TestCoordinatesAndHdrIntersection();
  TestFilenameAndHalfFloat();
  TestJpegColorLayout();
  if (failures) std::cerr << failures << " test(s) failed\n";
  else std::cout << "All RC-ScreenShot core tests passed\n";
  return failures ? 1 : 0;
}
