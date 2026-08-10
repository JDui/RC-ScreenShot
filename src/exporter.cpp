#include "exporter.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <objidl.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <fstream>

#if RC_ENABLE_ULTRAHDR
#include <ultrahdr_api.h>
#endif

namespace rc {
namespace {

D2D1_COLOR_F D2DColor(const ColorSetting& color, float opacity = 1.0f) {
  const uint32_t value = color.rgba;
  return D2D1::ColorF(((value >> 24) & 0xFF) / 255.0f, ((value >> 16) & 0xFF) / 255.0f,
                      ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f * opacity);
}

bool WriteAllAtomic(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                    std::wstring& error) {
  const auto temporary = path.wstring() + L".tmp";
  {
    std::ofstream output(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    if (!output) { error = L"无法创建输出文件。"; return false; }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) { error = L"写入输出文件失败。"; return false; }
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    error = L"提交输出文件失败：" + HResultMessage(HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

bool EncodeWicToStream(const RenderedImage& image, REFGUID container, IStream* stream,
                       float quality, std::wstring& error) {
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr)) { error = L"创建 WIC 失败：" + HResultMessage(hr); return false; }
  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(container, nullptr, &encoder);
  if (FAILED(hr) || FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
    error = L"初始化图像编码器失败。"; return false;
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  if (FAILED(encoder->CreateNewFrame(&frame, &properties))) { error = L"创建图像帧失败。"; return false; }
  if (container == GUID_ContainerFormatJpeg && properties) {
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value{};
    VariantInit(&value);
    value.vt = VT_R4; value.fltVal = std::clamp(quality, 0.01f, 1.0f);
    properties->Write(1, &option, &value);
    VariantClear(&value);
  }
  if (FAILED(frame->Initialize(properties.Get())) ||
      FAILED(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)))) {
    error = L"初始化图像帧失败。"; return false;
  }
  const bool jpeg = container == GUID_ContainerFormatJpeg;
  WICPixelFormatGUID format = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format)) ||
      (jpeg && format != GUID_WICPixelFormat24bppBGR) ||
      (!jpeg && format != GUID_WICPixelFormat32bppBGRA)) {
    error = L"设置图像像素格式失败。"; return false;
  }
  std::vector<uint8_t> bgr;
  BYTE* pixels = const_cast<BYTE*>(image.bgra.data());
  UINT writeStride = static_cast<UINT>(image.stride);
  UINT writeSize = static_cast<UINT>(image.bgra.size());
  if (jpeg) {
    writeStride = static_cast<UINT>(image.width * 3);
    bgr.resize(static_cast<size_t>(writeStride * image.height));
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
      const uint8_t* source = image.bgra.data() + static_cast<size_t>(y * image.stride + x * 4);
      uint8_t* destination = bgr.data() + static_cast<size_t>(y * writeStride + x * 3);
      destination[0] = source[0]; destination[1] = source[1]; destination[2] = source[2];
    }
    pixels = bgr.data(); writeSize = static_cast<UINT>(bgr.size());
  }
  hr = frame->WritePixels(static_cast<UINT>(image.height), writeStride, writeSize, pixels);
  if (FAILED(hr) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    error = L"编码图像失败：" + HResultMessage(hr); return false;
  }
  return true;
}

void DrawVectorCommands(ID2D1RenderTarget* target, ID2D1Factory* d2dFactory,
                        std::span<const EditCommand> commands, float offsetX, float offsetY) {
  target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  ComPtr<IDWriteFactory> dwriteFactory;
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                      reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
  ComPtr<ID2D1StrokeStyle> roundStroke;
  if (d2dFactory) {
    d2dFactory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                    10.0f, D2D1_DASH_STYLE_SOLID, 0.0f), nullptr, 0, &roundStroke);
  }
  const auto drawRoundPath = [&](const std::vector<PointF>& points, const D2D1_COLOR_F& color,
                                 float width) {
    if (points.empty()) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(color, &brush);
    if (points.size() == 1) {
      target->FillEllipse({{points.front().x + offsetX, points.front().y + offsetY},
                           width * 0.5f, width * 0.5f}, brush.Get());
      return;
    }
    if (!d2dFactory) return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(d2dFactory->CreatePathGeometry(&geometry))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return;
    sink->BeginFigure({points.front().x + offsetX, points.front().y + offsetY}, D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); ++i)
      sink->AddLine({points[i].x + offsetX, points[i].y + offsetY});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    target->DrawGeometry(geometry.Get(), brush.Get(), width, roundStroke.Get());
  };
  for (const EditCommand& command : commands) {
    if (const auto* pen = std::get_if<PenCommand>(&command)) {
      drawRoundPath(pen->points, D2DColor(pen->style.color, pen->style.opacity), pen->style.width);
    } else if (const auto* shape = std::get_if<ShapeCommand>(&command)) {
      const RectF rect = NormalizeRect(shape->start, shape->end);
      const D2D1_RECT_F d2dRect{rect.left + offsetX, rect.top + offsetY,
                               rect.right + offsetX, rect.bottom + offsetY};
      ComPtr<ID2D1SolidColorBrush> stroke;
      target->CreateSolidColorBrush(D2DColor(shape->style.stroke.color, shape->style.stroke.opacity), &stroke);
      ComPtr<ID2D1SolidColorBrush> fill;
      if (shape->style.fillOpacity > 0) {
        target->CreateSolidColorBrush(D2DColor(shape->style.fill, shape->style.fillOpacity), &fill);
      }
      switch (shape->kind) {
        case ShapeKind::Rectangle:
          if (fill) target->FillRectangle(d2dRect, fill.Get());
          target->DrawRectangle(d2dRect, stroke.Get(), shape->style.stroke.width); break;
        case ShapeKind::Ellipse: {
          D2D1_ELLIPSE ellipse{{(d2dRect.left + d2dRect.right) / 2, (d2dRect.top + d2dRect.bottom) / 2},
                               (d2dRect.right - d2dRect.left) / 2, (d2dRect.bottom - d2dRect.top) / 2};
          if (fill) target->FillEllipse(ellipse, fill.Get());
          target->DrawEllipse(ellipse, stroke.Get(), shape->style.stroke.width); break;
        }
        case ShapeKind::Line:
        case ShapeKind::Arrow: {
          const D2D1_POINT_2F start{shape->start.x + offsetX, shape->start.y + offsetY};
          const D2D1_POINT_2F end{shape->end.x + offsetX, shape->end.y + offsetY};
          target->DrawLine(start, end, stroke.Get(), shape->style.stroke.width);
          if (shape->kind == ShapeKind::Arrow) {
            const float angle = std::atan2(end.y - start.y, end.x - start.x);
            const float size = std::max(10.0f, shape->style.stroke.width * 4.0f);
            D2D1_POINT_2F left{end.x - size * std::cos(angle - 0.55f), end.y - size * std::sin(angle - 0.55f)};
            D2D1_POINT_2F right{end.x - size * std::cos(angle + 0.55f), end.y - size * std::sin(angle + 0.55f)};
            target->DrawLine(end, left, stroke.Get(), shape->style.stroke.width);
            target->DrawLine(end, right, stroke.Get(), shape->style.stroke.width);
          }
          break;
        }
      }
    } else if (const auto* text = std::get_if<TextCommand>(&command); text && dwriteFactory) {
      ComPtr<IDWriteTextFormat> format;
      if (FAILED(dwriteFactory->CreateTextFormat(text->style.fontFamily.c_str(), nullptr,
          DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          text->style.size, L"zh-CN", &format))) continue;
      format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      ComPtr<ID2D1SolidColorBrush> brush;
      target->CreateSolidColorBrush(D2DColor(text->style.color, text->style.opacity), &brush);
      const float originX = text->origin.x + offsetX, originY = text->origin.y + offsetY;
      if (!text->style.vertical) {
        target->DrawTextW(text->text.data(), static_cast<UINT32>(text->text.size()), format.Get(),
                          D2D1::RectF(originX, originY, originX + 4096, originY + 4096), brush.Get());
      } else {
        float x = originX, y = originY;
        const float advance = text->style.size * 1.16f;
        for (wchar_t character : text->text) {
          if (character == L'\r') continue;
          if (character == L'\n') { x += advance; y = originY; continue; }
          target->DrawTextW(&character, 1, format.Get(),
                            D2D1::RectF(x, y, x + advance, y + advance), brush.Get());
          y += advance;
        }
      }
    }
  }
}

}  // namespace

bool ImageExporter::Render(const DesktopSnapshot& snapshot, const RECT& selection,
                           const EditorDocument& document, const AppConfig& config,
                           bool addWindowShadow, RenderedImage& image, std::wstring& error) const {
  RECT clipped{};
  if (!IntersectRect(&clipped, &selection, &snapshot.virtualBounds)) {
    error = L"选区不在可捕获桌面范围内。"; return false;
  }
  const int sourceWidth = clipped.right - clipped.left;
  const int sourceHeight = clipped.bottom - clipped.top;
  const int frame = config.frameEnabled ? std::clamp(static_cast<int>(std::lround(config.frame.width)), 1, 128) : 0;
  const int shadow = addWindowShadow && config.windowShadow ? 18 : 0;
  const int leftMargin = frame + shadow, topMargin = frame + shadow;
  const int rightMargin = frame + shadow, bottomMargin = frame + shadow;
  image.width = sourceWidth + leftMargin + rightMargin;
  image.height = sourceHeight + topMargin + bottomMargin;
  image.stride = image.width * 4;
  image.bgra.assign(static_cast<size_t>(image.stride * image.height), 255);
  image.hdrRgba.assign(static_cast<size_t>(image.width * image.height * 4), FloatToHalf(1.0f));
  image.hasHdr = false;
  for (const RECT& hdrRegion : snapshot.hdrRegions) {
    RECT intersection{};
    if (IntersectRect(&intersection, &clipped, &hdrRegion)) {
      image.hasHdr = true;
      break;
    }
  }
  image.peakLuminanceNits = snapshot.peakLuminanceNits;

  std::vector<uint8_t> cropped(static_cast<size_t>(sourceWidth * sourceHeight * 4));
  std::vector<uint16_t> originalHdr(static_cast<size_t>(sourceWidth * sourceHeight * 4));
  const int sourceX = clipped.left - snapshot.virtualBounds.left;
  const int sourceY = clipped.top - snapshot.virtualBounds.top;
  for (int y = 0; y < sourceHeight; ++y) {
    memcpy(cropped.data() + static_cast<size_t>(y * sourceWidth * 4),
           snapshot.bgra.data() + static_cast<size_t>((sourceY + y) * snapshot.bgraStride + sourceX * 4),
           static_cast<size_t>(sourceWidth * 4));
    memcpy(originalHdr.data() + static_cast<size_t>(y * sourceWidth * 4),
           snapshot.hdrRgba.data() + static_cast<size_t>(((sourceY + y) * snapshot.width + sourceX) * 4),
           static_cast<size_t>(sourceWidth * 4 * sizeof(uint16_t)));
  }
  const std::vector<uint8_t> originalSdr = cropped;
  ApplyMosaics(cropped, sourceWidth, sourceHeight, sourceWidth * 4, document.Commands());

  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(hr) || FAILED(factory->CreateBitmap(static_cast<UINT>(image.width), static_cast<UINT>(image.height),
                                                 GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapCacheOnLoad, &bitmap))) {
    error = L"创建导出画布失败。"; return false;
  }
  ComPtr<ID2D1Factory> d2dFactory;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2dFactory)))) {
    error = L"创建 Direct2D 工厂失败。"; return false;
  }
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(d2dFactory->CreateWicBitmapRenderTarget(bitmap.Get(),
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     D2D1_ALPHA_MODE_PREMULTIPLIED)), &target))) {
    error = L"创建 Direct2D 导出目标失败。"; return false;
  }
  ComPtr<IWICBitmap> sourceBitmap;
  if (FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(sourceWidth), static_cast<UINT>(sourceHeight),
                                             GUID_WICPixelFormat32bppPBGRA,
                                             static_cast<UINT>(sourceWidth * 4),
                                             static_cast<UINT>(cropped.size()), cropped.data(), &sourceBitmap))) {
    error = L"创建截图片段失败。"; return false;
  }
  ComPtr<ID2D1Bitmap> d2dSource;
  if (FAILED(target->CreateBitmapFromWicBitmap(sourceBitmap.Get(), nullptr, &d2dSource))) {
    error = L"创建 Direct2D 截图位图失败。"; return false;
  }
  target->BeginDraw();
  target->Clear(D2D1::ColorF(D2D1::ColorF::White));
  if (shadow) {
    for (int i = shadow; i > 0; i -= 2) {
      const float alpha = 0.012f * (shadow - i + 2);
      ComPtr<ID2D1SolidColorBrush> brush;
      target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, alpha), &brush);
      target->FillRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(static_cast<float>(leftMargin - i), static_cast<float>(topMargin - i),
                      static_cast<float>(leftMargin + sourceWidth + i),
                      static_cast<float>(topMargin + sourceHeight + i)), 5.0f, 5.0f), brush.Get());
    }
  }
  if (frame) {
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(D2DColor(config.frame.color, config.frame.opacity), &brush);
    target->FillRectangle(D2D1::RectF(static_cast<float>(shadow), static_cast<float>(shadow),
                                      static_cast<float>(image.width - shadow),
                                      static_cast<float>(image.height - shadow)), brush.Get());
  }
  target->DrawBitmap(d2dSource.Get(),
                     D2D1::RectF(static_cast<float>(leftMargin), static_cast<float>(topMargin),
                                 static_cast<float>(leftMargin + sourceWidth),
                                 static_cast<float>(topMargin + sourceHeight)), 1.0f,
                     D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
  target->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(leftMargin), static_cast<float>(topMargin),
                                          static_cast<float>(leftMargin + sourceWidth),
                                          static_cast<float>(topMargin + sourceHeight)),
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  DrawVectorCommands(target.Get(), d2dFactory.Get(), document.Commands(),
                     static_cast<float>(leftMargin), static_cast<float>(topMargin));
  target->PopAxisAlignedClip();
  hr = target->EndDraw();
  if (FAILED(hr)) { error = L"渲染导出图像失败：" + HResultMessage(hr); return false; }

  WICRect lockRect{0, 0, image.width, image.height};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockRead, &lock))) { error = L"读取导出画布失败。"; return false; }
  UINT size = 0, stride = 0; BYTE* data = nullptr;
  lock->GetStride(&stride); lock->GetDataPointer(&size, &data);
  for (int y = 0; y < image.height; ++y) {
    memcpy(image.bgra.data() + static_cast<size_t>(y * image.stride),
           data + static_cast<size_t>(y * stride), static_cast<size_t>(image.stride));
  }

  // Preserve original HDR pixels where the SDR rendering stayed untouched. Changed pixels are
  // lifted to linear 203-nit reference white so annotations and mosaic remain geometrically exact.
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const int localX = x - leftMargin, localY = y - topMargin;
      uint16_t* output = image.hdrRgba.data() + static_cast<size_t>((y * image.width + x) * 4);
      const uint8_t* finalPixel = image.bgra.data() + static_cast<size_t>(y * image.stride + x * 4);
      bool preserve = localX >= 0 && localY >= 0 && localX < sourceWidth && localY < sourceHeight;
      if (preserve) {
        const uint8_t* original = originalSdr.data() + static_cast<size_t>((localY * sourceWidth + localX) * 4);
        preserve = memcmp(original, finalPixel, 4) == 0;
      }
      if (preserve) {
        const uint16_t* source = originalHdr.data() + static_cast<size_t>((localY * sourceWidth + localX) * 4);
        memcpy(output, source, 4 * sizeof(uint16_t));
      } else {
        const auto linear = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
        output[0] = FloatToHalf(linear(finalPixel[2] / 255.0f));
        output[1] = FloatToHalf(linear(finalPixel[1] / 255.0f));
        output[2] = FloatToHalf(linear(finalPixel[0] / 255.0f));
        output[3] = FloatToHalf(finalPixel[3] / 255.0f);
      }
    }
  }
  return true;
}

bool ImageExporter::CopyToClipboard(HWND owner, const RenderedImage& image, std::wstring& error) const {
  if (!OpenClipboard(owner)) { error = L"无法打开剪贴板。"; return false; }
  ScopeExit close{[] { CloseClipboard(); }};
  if (!EmptyClipboard()) { error = L"无法清空剪贴板。"; return false; }

  const size_t dibSize = sizeof(BITMAPV5HEADER) + static_cast<size_t>(image.stride * image.height);
  HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, dibSize);
  if (!dib) { error = L"剪贴板内存不足。"; return false; }
  auto* header = static_cast<BITMAPV5HEADER*>(GlobalLock(dib));
  ZeroMemory(header, sizeof(*header));
  header->bV5Size = sizeof(*header); header->bV5Width = image.width; header->bV5Height = -image.height;
  header->bV5Planes = 1; header->bV5BitCount = 32; header->bV5Compression = BI_BITFIELDS;
  header->bV5RedMask = 0x00FF0000; header->bV5GreenMask = 0x0000FF00;
  header->bV5BlueMask = 0x000000FF; header->bV5AlphaMask = 0xFF000000;
  header->bV5CSType = LCS_sRGB;
  memcpy(reinterpret_cast<uint8_t*>(header) + sizeof(*header), image.bgra.data(),
         static_cast<size_t>(image.stride * image.height));
  GlobalUnlock(dib);
  if (!SetClipboardData(CF_DIBV5, dib)) { GlobalFree(dib); error = L"写入 CF_DIBV5 失败。"; return false; }

  ComPtr<IStream> stream;
  if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, FALSE, &stream))) {
    std::wstring pngError;
    if (EncodeWicToStream(image, GUID_ContainerFormatPng, stream.Get(), 1.0f, pngError)) {
      HGLOBAL png = nullptr;
      if (SUCCEEDED(GetHGlobalFromStream(stream.Get(), &png))) {
        const UINT format = RegisterClipboardFormatW(L"PNG");
        // The stream was created without automatic HGLOBAL deletion. Release the stream before
        // handing the allocation to the clipboard, which then assumes ownership on success.
        stream.Reset();
        if (!SetClipboardData(format, png)) GlobalFree(png);
      }
    }
  }
  return true;
}

bool ImageExporter::Save(const RenderedImage& image, const std::filesystem::path& directory,
                         std::wstring_view filenameTemplate, int jpegQuality,
                         std::filesystem::path& savedPath, std::wstring& error) const {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) { error = L"无法创建截图目录：" + directory.wstring(); return false; }
  SYSTEMTIME time{}; GetLocalTime(&time);
  std::wstring filename = MakeFilename(filenameTemplate, time);
  if (std::filesystem::path(filename).extension().empty()) filename += L".jpg";
  savedPath = directory / filename;
  for (int suffix = 1; std::filesystem::exists(savedPath); ++suffix) {
    const std::filesystem::path base(filename);
    savedPath = directory / (base.stem().wstring() + L"_" + std::to_wstring(suffix) + base.extension().wstring());
  }
  if (image.hasHdr) return SaveUltraHdr(image, savedPath, jpegQuality, error);
  return SaveJpegWic(image, savedPath, jpegQuality, error);
}

std::wstring ImageExporter::MakeFilename(std::wstring_view filenameTemplate, const SYSTEMTIME& time) {
  std::wstring output(filenameTemplate);
  const auto replace = [&](std::wstring_view token, int value, int digits) {
    wchar_t number[16]{};
    swprintf_s(number, L"%0*d", digits, value);
    size_t position = 0;
    while ((position = output.find(token, position)) != std::wstring::npos) {
      output.replace(position, token.size(), number); position += wcslen(number);
    }
  };
  replace(L"yyyy", time.wYear, 4); replace(L"MM", time.wMonth, 2); replace(L"dd", time.wDay, 2);
  replace(L"HH", time.wHour, 2); replace(L"mm", time.wMinute, 2); replace(L"ss", time.wSecond, 2);
  replace(L"fff", time.wMilliseconds, 3);
  for (wchar_t& c : output) if (wcschr(L"<>:\"/\\|?*", c)) c = L'_';
  return output.empty() ? L"RC-ScreenShot.jpg" : output;
}

bool ImageExporter::SaveJpegWic(const RenderedImage& image, const std::filesystem::path& path,
                                int quality, std::wstring& error) const {
  const auto temporary = path.wstring() + L".tmp";
  ComPtr<IStream> stream;
  HRESULT hr = SHCreateStreamOnFileEx(temporary.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                      FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
  if (FAILED(hr)) { error = L"无法创建 JPEG 文件：" + HResultMessage(hr); return false; }
  if (!EncodeWicToStream(image, GUID_ContainerFormatJpeg, stream.Get(), quality / 100.0f, error)) {
    stream.Reset(); DeleteFileW(temporary.c_str()); return false;
  }
  stream.Reset();
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str()); error = L"提交 JPEG 文件失败。"; return false;
  }
  return true;
}

bool ImageExporter::SaveUltraHdr(const RenderedImage& image, const std::filesystem::path& path,
                                 int quality, std::wstring& error) const {
#if RC_ENABLE_ULTRAHDR
  std::vector<uint8_t> rgba(static_cast<size_t>(image.width * image.height * 4));
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const uint8_t* source = image.bgra.data() + static_cast<size_t>(y * image.stride + x * 4);
      uint8_t* dest = rgba.data() + static_cast<size_t>((y * image.width + x) * 4);
      dest[0] = source[2]; dest[1] = source[1]; dest[2] = source[0]; dest[3] = source[3];
    }
  }
  uhdr_raw_image_t hdr{};
  hdr.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat; hdr.cg = UHDR_CG_BT_709;
  hdr.ct = UHDR_CT_LINEAR; hdr.range = UHDR_CR_FULL_RANGE;
  hdr.w = static_cast<unsigned>(image.width); hdr.h = static_cast<unsigned>(image.height);
  hdr.planes[UHDR_PLANE_PACKED] = const_cast<uint16_t*>(image.hdrRgba.data());
  hdr.stride[UHDR_PLANE_PACKED] = static_cast<unsigned>(image.width);
  uhdr_raw_image_t sdr{};
  sdr.fmt = UHDR_IMG_FMT_32bppRGBA8888; sdr.cg = UHDR_CG_BT_709;
  sdr.ct = UHDR_CT_SRGB; sdr.range = UHDR_CR_FULL_RANGE;
  sdr.w = static_cast<unsigned>(image.width); sdr.h = static_cast<unsigned>(image.height);
  sdr.planes[UHDR_PLANE_PACKED] = rgba.data(); sdr.stride[UHDR_PLANE_PACKED] = static_cast<unsigned>(image.width);
  uhdr_codec_private_t* encoder = uhdr_create_encoder();
  if (!encoder) { error = L"无法创建 Ultra HDR 编码器。"; return false; }
  ScopeExit release{[&] { uhdr_release_encoder(encoder); }};
  const auto check = [&](uhdr_error_info_t result) {
    if (result.error_code == UHDR_CODEC_OK) return true;
    error = result.has_detail ? FromUtf8(result.detail) : L"Ultra HDR 编码器返回错误。";
    return false;
  };
  if (!check(uhdr_enc_set_raw_image(encoder, &hdr, UHDR_HDR_IMG)) ||
      !check(uhdr_enc_set_raw_image(encoder, &sdr, UHDR_SDR_IMG)) ||
      !check(uhdr_enc_set_quality(encoder, std::clamp(quality, 1, 100), UHDR_BASE_IMG)) ||
      !check(uhdr_enc_set_quality(encoder, std::clamp(quality - 5, 1, 100), UHDR_GAIN_MAP_IMG)) ||
      !check(uhdr_enc_set_preset(encoder, UHDR_USAGE_REALTIME)) ||
      !check(uhdr_enc_set_output_format(encoder, UHDR_CODEC_JPG)) ||
      !check(uhdr_encode(encoder))) return false;
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(encoder);
  if (!output || !output->data || !output->data_sz) { error = L"Ultra HDR 编码器没有产生数据。"; return false; }
  return WriteAllAtomic(path, {static_cast<const uint8_t*>(output->data), output->data_sz}, error);
#else
  (void)image;
  (void)path;
  (void)quality;
  error = L"此构建未启用 libultrahdr，已拒绝将 HDR 截图降级保存为普通 JPEG。";
  return false;
#endif
}

}  // namespace rc
