#include "capture.hpp"
#include "config.hpp"
#include "editor.hpp"
#include "exporter.hpp"
#include "unit_detector.hpp"

#include <fstream>
#include <iostream>
#include <wincodec.h>

namespace {

int failures = 0;

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #condition "\n"; ++failures; \
} } while (false)

void TestHotkeys() {
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
  CHECK(loaded.hotkeys.size() == 2);
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
  CHECK(loaded.hotkeys.size() == 1);
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
  TestCoordinatesAndHdrIntersection();
  TestFilenameAndHalfFloat();
  TestJpegColorLayout();
  if (failures) std::cerr << failures << " test(s) failed\n";
  else std::cout << "All RC-ScreenShot core tests passed\n";
  return failures ? 1 : 0;
}
