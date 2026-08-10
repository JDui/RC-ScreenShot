#include "capture.hpp"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>

namespace rc {
namespace {

struct OutputInfo {
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput> output;
  DXGI_OUTPUT_DESC1 description{};
};

std::vector<OutputInfo> EnumerateOutputs() {
  std::vector<OutputInfo> outputs;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return outputs;
  for (UINT ai = 0;; ++ai) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    for (UINT oi = 0;; ++oi) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
      ComPtr<IDXGIOutput6> output6;
      DXGI_OUTPUT_DESC1 desc{};
      if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc)) || !desc.AttachedToDesktop) continue;
      outputs.push_back({adapter, output, desc});
    }
  }
  return outputs;
}

bool IsHdrColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
  return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
}

float SrgbToLinear(float value) {
  return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

float PqToNits(float value) {
  constexpr float m1 = 2610.0f / 16384.0f;
  constexpr float m2 = 2523.0f / 32.0f;
  constexpr float c1 = 3424.0f / 4096.0f;
  constexpr float c2 = 2413.0f / 128.0f;
  constexpr float c3 = 2392.0f / 128.0f;
  const float p = std::pow(std::clamp(value, 0.0f, 1.0f), 1.0f / m2);
  return 10000.0f * std::pow(std::max(p - c1, 0.0f) / std::max(c2 - c3 * p, 0.000001f), 1.0f / m1);
}

std::array<float, 3> Rec2020To709(float r, float g, float b) {
  return {
      1.6605f * r - 0.5876f * g - 0.0728f * b,
     -0.1246f * r + 1.1329f * g - 0.0083f * b,
     -0.0182f * r - 0.1006f * g + 1.1187f * b};
}

std::array<float, 3> ToneMap(float r203, float g203, float b203, float peakNits) {
  const float white = std::max(1.5f, peakNits / 203.0f);
  const float whiteSquared = white * white;
  const float normalize = 2.0f / (1.0f + 1.0f / whiteSquared);
  const float luminance = std::max(0.00001f, 0.2126f * r203 + 0.7152f * g203 + 0.0722f * b203);
  const float mappedLuminance = luminance * (1.0f + luminance / whiteSquared) /
                                (1.0f + luminance) * normalize;
  const float scale = mappedLuminance / luminance;
  return {LinearToSrgb(r203 * scale), LinearToSrgb(g203 * scale), LinearToSrgb(b203 * scale)};
}

void StorePixel(DesktopSnapshot& snapshot, int x, int y, float r203, float g203, float b203,
                float peakNits) {
  if (x < 0 || y < 0 || x >= snapshot.width || y >= snapshot.height) return;
  auto sdr = ToneMap(r203, g203, b203, peakNits);
  uint8_t* pixel = snapshot.bgra.data() + static_cast<size_t>(y * snapshot.bgraStride + x * 4);
  pixel[0] = ClampByte(sdr[2] * 255.0f);
  pixel[1] = ClampByte(sdr[1] * 255.0f);
  pixel[2] = ClampByte(sdr[0] * 255.0f);
  pixel[3] = 255;
  uint16_t* hdr = snapshot.hdrRgba.data() + static_cast<size_t>((y * snapshot.width + x) * 4);
  hdr[0] = FloatToHalf(std::max(0.0f, r203));
  hdr[1] = FloatToHalf(std::max(0.0f, g203));
  hdr[2] = FloatToHalf(std::max(0.0f, b203));
  hdr[3] = FloatToHalf(1.0f);
}

void StoreSdrPixel(DesktopSnapshot& snapshot, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || y < 0 || x >= snapshot.width || y >= snapshot.height) return;
  uint8_t* pixel = snapshot.bgra.data() + static_cast<size_t>(y * snapshot.bgraStride + x * 4);
  // Desktop pixels are always opaque. DXGI/GDI leave the alpha channel undefined and commonly
  // return zero; treating that byte as real alpha made the frozen desktop disappear in Direct2D.
  pixel[0] = b; pixel[1] = g; pixel[2] = r; pixel[3] = 255;
  uint16_t* hdr = snapshot.hdrRgba.data() + static_cast<size_t>((y * snapshot.width + x) * 4);
  hdr[0] = FloatToHalf(SrgbToLinear(r / 255.0f));
  hdr[1] = FloatToHalf(SrgbToLinear(g / 255.0f));
  hdr[2] = FloatToHalf(SrgbToLinear(b / 255.0f));
  hdr[3] = FloatToHalf(1.0f);
}

// BGRA-only variant used on SDR systems where the half-float HDR buffer is never consumed.
void StoreBgraPixel(DesktopSnapshot& snapshot, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || y < 0 || x >= snapshot.width || y >= snapshot.height) return;
  uint8_t* pixel = snapshot.bgra.data() + static_cast<size_t>(y * snapshot.bgraStride + x * 4);
  pixel[0] = b; pixel[1] = g; pixel[2] = r; pixel[3] = 255;
}

// True when every sampled color is black. Alpha is intentionally ignored: BGRA desktop frames
// commonly leave it at zero, while an empty HDR frame may still carry an opaque alpha channel.
bool FrameIsBlack(const D3D11_MAPPED_SUBRESOURCE& mapped, const D3D11_TEXTURE2D_DESC& desc) {
  const size_t stepX = std::max<size_t>(1, desc.Width / 64);
  const size_t stepY = std::max<size_t>(1, desc.Height / 64);
  for (size_t y = 0; y < desc.Height; y += stepY) {
    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
    for (size_t x = 0; x < desc.Width; x += stepX) {
      if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const uint16_t* pixel = reinterpret_cast<const uint16_t*>(row) + x * 4;
        if (pixel[0] || pixel[1] || pixel[2]) return false;
      } else if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        if ((reinterpret_cast<const uint32_t*>(row)[x] & 0x3FFFFFFFu) != 0) return false;
      } else {
        const uint8_t* pixel = row + x * 4;
        if (pixel[0] || pixel[1] || pixel[2]) return false;
      }
    }
  }
  return true;
}

POINT MapRotated(int x, int y, int sourceWidth, int sourceHeight, DXGI_MODE_ROTATION rotation) {
  switch (rotation) {
    case DXGI_MODE_ROTATION_ROTATE90: return {y, sourceHeight - 1 - x};
    case DXGI_MODE_ROTATION_ROTATE180: return {sourceWidth - 1 - x, sourceHeight - 1 - y};
    case DXGI_MODE_ROTATION_ROTATE270: return {sourceWidth - 1 - y, x};
    default: return {x, y};
  }
}

BOOL CALLBACK WindowEnumerator(HWND hwnd, LPARAM parameter) {
  auto* windows = reinterpret_cast<std::vector<WindowCandidate>*>(parameter);
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
  if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
  DWORD cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
  RECT bounds{};
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
    if (!GetWindowRect(hwnd, &bounds)) return TRUE;
  }
  if (bounds.right - bounds.left < 32 || bounds.bottom - bounds.top < 32) return TRUE;
  wchar_t title[512]{};
  GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
  windows->push_back({hwnd, bounds, title});
  return TRUE;
}

bool SnapshotIsBlankImpl(const DesktopSnapshot& snapshot) {
  if (!snapshot.IsValid()) return true;
  const int stepX = std::max(1, snapshot.width / 64);
  const int stepY = std::max(1, snapshot.height / 64);
  int samples = 0;
  int nonBlack = 0;
  for (int y = 0; y < snapshot.height; y += stepY) {
    for (int x = 0; x < snapshot.width; x += stepX) {
      const uint8_t* pixel = snapshot.bgra.data() + static_cast<size_t>(y * snapshot.bgraStride + x * 4);
      ++samples;
      if (pixel[0] > 3 || pixel[1] > 3 || pixel[2] > 3) ++nonBlack;
    }
  }
  // A completely black duplication frame is a known transient state immediately after creating
  // a duplication session. A real desktop may be dark, so only classify it as empty when every
  // sampled pixel is black.
  return samples > 0 && nonBlack == 0;
}

}  // namespace

bool SnapshotIsBlank(const DesktopSnapshot& snapshot) {
  return SnapshotIsBlankImpl(snapshot);
}

float HalfToFloat(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
  uint32_t exponent = (half >> 10) & 0x1F;
  uint32_t mantissa = half & 0x3FF;
  uint32_t result = 0;
  if (exponent == 0) {
    if (mantissa == 0) result = sign;
    else {
      exponent = 1;
      while ((mantissa & 0x400) == 0) { mantissa <<= 1; --exponent; }
      mantissa &= 0x3FF;
      result = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    result = sign | 0x7F800000 | (mantissa << 13);
  } else {
    result = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float value = 0;
  memcpy(&value, &result, sizeof(value));
  return value;
}

uint16_t FloatToHalf(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000;
  int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFF;
  if (exponent <= 0) {
    if (exponent < -10) return static_cast<uint16_t>(sign);
    mantissa = (mantissa | 0x800000) >> (1 - exponent);
    return static_cast<uint16_t>(sign | ((mantissa + 0x1000) >> 13));
  }
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00);
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) |
                               ((mantissa + 0x1000) >> 13));
}

bool DesktopCapture::Capture(DesktopSnapshot& snapshot, std::wstring& error) {
  snapshot = {};
  bool emptyDxgiFrame = false;
  std::wstring lastDxgiError;
  for (int attempt = 0; attempt < 3; ++attempt) {
    DesktopSnapshot candidate;
    std::wstring attemptError;
    if (CaptureDxgi(candidate, attemptError)) {
      if (!SnapshotIsBlank(candidate)) {
        snapshot = std::move(candidate);
        return true;
      }
      emptyDxgiFrame = true;
      snapshot = std::move(candidate);
    } else {
      lastDxgiError = std::move(attemptError);
    }
    if (attempt < 2) Sleep(30);
  }
  error = lastDxgiError;
  const std::vector<OutputInfo> outputs = EnumerateOutputs();
  const bool anyHdr = std::any_of(outputs.begin(), outputs.end(), [](const OutputInfo& output) {
    return IsHdrColorSpace(output.description.ColorSpace);
  });
  if (anyHdr) {
    error += L"\nHDR 显示器上不使用会破坏亮度信息的 GDI 回退。";
    return false;
  }
  std::wstring gdiError;
  bool emptyGdiFrame = false;
  for (int attempt = 0; attempt < 3; ++attempt) {
    DesktopSnapshot candidate;
    std::wstring attemptError;
    if (CaptureGdi(candidate, attemptError)) {
      if (!SnapshotIsBlank(candidate)) {
        candidate.usedGdiFallback = true;
        snapshot = std::move(candidate);
        if (emptyDxgiFrame) error += L"\nDXGI 帧为空，已使用 GDI 重新捕获。";
        return true;
      }
      emptyGdiFrame = true;
    } else {
      gdiError = std::move(attemptError);
    }
    if (attempt < 2) Sleep(40);
  }
  if (!gdiError.empty()) error += L"\nGDI 回退也失败：" + gdiError;
  if (emptyGdiFrame) error += L"\nGDI 连续返回空画面，已取消本次截图以避免黑屏。";
  return false;
}

bool DesktopCapture::CaptureDxgi(DesktopSnapshot& snapshot, std::wstring& error) {
  const auto outputs = EnumerateOutputs();
  if (outputs.empty()) { error = L"没有找到活动显示器。"; return false; }
  // The half-float HDR buffer is only consumed when at least one output carries an HDR color
  // space. On plain SDR systems it is never read, so skip allocating and filling it entirely.
  const bool anyHdr = std::any_of(outputs.begin(), outputs.end(), [](const OutputInfo& info) {
    return IsHdrColorSpace(info.description.ColorSpace);
  });
  // Use the same coordinate source as the captured outputs. Mixing GetSystemMetrics with DXGI
  // monitor coordinates under mixed-DPI layouts can shift portrait/negative-coordinate screens.
  RECT virtualBounds = outputs.front().description.DesktopCoordinates;
  for (size_t i = 1; i < outputs.size(); ++i) {
    RECT united{};
    UnionRect(&united, &virtualBounds, &outputs[i].description.DesktopCoordinates);
    virtualBounds = united;
  }
  snapshot.virtualBounds = virtualBounds;
  snapshot.width = virtualBounds.right - virtualBounds.left;
  snapshot.height = virtualBounds.bottom - virtualBounds.top;
  snapshot.bgraStride = snapshot.width * 4;
  snapshot.bgra.assign(static_cast<size_t>(snapshot.bgraStride * snapshot.height), 0);
  for (size_t i = 3; i < snapshot.bgra.size(); i += 4) snapshot.bgra[i] = 255;
  if (anyHdr) {
    snapshot.hdrRgba.assign(static_cast<size_t>(snapshot.width * snapshot.height * 4), 0);
    for (size_t i = 3; i < snapshot.hdrRgba.size(); i += 4) snapshot.hdrRgba[i] = FloatToHalf(1.0f);
  }

  for (const OutputInfo& info : outputs) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(info.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) { error = L"创建 D3D11 设备失败：" + HResultMessage(hr); return false; }
    ComPtr<IDXGIOutput5> output5;
    if (FAILED(info.output.As(&output5))) { error = L"显示驱动不支持 DXGI 1.5。"; return false; }
    const bool hdrColorSpace = IsHdrColorSpace(info.description.ColorSpace);
    const DXGI_FORMAT hdrFormats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      DXGI_FORMAT_R10G10B10A2_UNORM,
                                      DXGI_FORMAT_B8G8R8A8_UNORM};
    const DXGI_FORMAT sdrFormats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
    ComPtr<IDXGIOutputDuplication> duplication;
    hr = output5->DuplicateOutput1(device.Get(), 0,
                                   hdrColorSpace ? static_cast<UINT>(std::size(hdrFormats))
                                                 : static_cast<UINT>(std::size(sdrFormats)),
                                   hdrColorSpace ? hdrFormats : sdrFormats, &duplication);
    if (FAILED(hr) && !hdrColorSpace) {
      // Some SDR display/indirect-display drivers expose IDXGIOutput5 but reject
      // DuplicateOutput1. DXGI 1.2 duplication is still lossless for their BGRA output.
      ComPtr<IDXGIOutput1> output1;
      if (SUCCEEDED(info.output.As(&output1))) hr = output1->DuplicateOutput(device.Get(), &duplication);
    }
    if (FAILED(hr)) { error = L"创建桌面复制会话失败：" + HResultMessage(hr); return false; }
    DXGI_OUTDUPL_DESC duplicationDesc{};
    duplication->GetDesc(&duplicationDesc);
    const RECT monitor = info.description.DesktopCoordinates;
    const int destinationWidth = monitor.right - monitor.left;
    const int destinationHeight = monitor.bottom - monitor.top;
    const float peakNits = info.description.MaxLuminance > 0 ? info.description.MaxLuminance : 1000.0f;

    // The first frame of a fresh duplication session is a known transient all-black frame.
    // Re-acquiring on the same session fixes it cheaply; only fall back to the outer loop's
    // device+session rebuild when the black-out persists. A monitor that is legitimately all
    // black pays at most one extra (short-timeout) acquire.
    constexpr int kMaximumFrameAttempts = 4;
    bool capturedOutput = false;
    for (int frameAttempt = 0; frameAttempt < kMaximumFrameAttempts; ++frameAttempt) {
      DXGI_OUTDUPL_FRAME_INFO frameInfo{};
      ComPtr<IDXGIResource> resource;
      hr = duplication->AcquireNextFrame(frameAttempt == 0 ? 300 : 120, &frameInfo, &resource);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT && frameAttempt + 1 < kMaximumFrameAttempts) continue;
      if (FAILED(hr)) { error = L"获取桌面帧失败：" + HResultMessage(hr); return false; }
      ScopeExit release{[&] { duplication->ReleaseFrame(); }};
      ComPtr<ID3D11Texture2D> texture;
      if (FAILED(resource.As(&texture))) { error = L"桌面帧不是 D3D11 纹理。"; return false; }
      D3D11_TEXTURE2D_DESC desc{};
      texture->GetDesc(&desc);
      D3D11_TEXTURE2D_DESC stagingDesc = desc;
      stagingDesc.Usage = D3D11_USAGE_STAGING;
      stagingDesc.BindFlags = 0;
      stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      stagingDesc.MiscFlags = 0;
      ComPtr<ID3D11Texture2D> staging;
      if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        error = L"创建桌面帧读回纹理失败。"; return false;
      }
      context->CopyResource(staging.Get(), texture.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
      if (FAILED(hr)) { error = L"读取桌面帧失败：" + HResultMessage(hr); return false; }
      ScopeExit unmap{[&] { context->Unmap(staging.Get(), 0); }};

      if (FrameIsBlack(mapped, desc)) {
        if (frameAttempt + 1 < kMaximumFrameAttempts) continue;
        error = L"显示器连续返回空画面，已重建捕获会话。";
        return false;
      }

      const bool hdrOutput = hdrColorSpace &&
                             (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                              desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
      if (hdrOutput) {
        snapshot.hasHdr = true;
        snapshot.peakLuminanceNits = std::max(snapshot.peakLuminanceNits, peakNits);
        snapshot.hdrRegions.push_back(monitor);
      }
      if (!anyHdr && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
          duplicationDesc.Rotation == DXGI_MODE_ROTATION_IDENTITY &&
          static_cast<int>(desc.Width) == destinationWidth &&
          static_cast<int>(desc.Height) == destinationHeight) {
        // SDR fast path: the BGRA8 frame maps 1:1 into the virtual-desktop buffer, so copy whole
        // rows instead of converting every pixel. The desktop alpha byte is undefined (commonly
        // zero), so force it opaque just like the per-pixel path does.
        for (int dy = 0; dy < destinationHeight; ++dy) {
          const auto* source = static_cast<const uint8_t*>(mapped.pData) +
                               static_cast<size_t>(dy * mapped.RowPitch);
          uint8_t* destination = snapshot.bgra.data() +
              static_cast<size_t>((monitor.top - virtualBounds.top + dy) * snapshot.bgraStride +
                                  (monitor.left - virtualBounds.left) * 4);
          memcpy(destination, source, static_cast<size_t>(destinationWidth * 4));
          for (int dx = 0; dx < destinationWidth; ++dx) destination[dx * 4 + 3] = 255;
        }
      } else {
        for (int dy = 0; dy < destinationHeight; ++dy) {
          for (int dx = 0; dx < destinationWidth; ++dx) {
            POINT source = MapRotated(dx, dy, static_cast<int>(desc.Width), static_cast<int>(desc.Height),
                                      duplicationDesc.Rotation);
            source.x = std::clamp(source.x, 0L, static_cast<LONG>(desc.Width) - 1);
            source.y = std::clamp(source.y, 0L, static_cast<LONG>(desc.Height) - 1);
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) +
                                 static_cast<size_t>(source.y * mapped.RowPitch);
            const int vx = monitor.left - virtualBounds.left + dx;
            const int vy = monitor.top - virtualBounds.top + dy;
            if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
              const uint16_t* pixel = reinterpret_cast<const uint16_t*>(row) + source.x * 4;
              // Windows scRGB uses 1.0 == 80 nits; Ultra HDR linear RGB uses 1.0 == 203 nits.
              const float r = HalfToFloat(pixel[0]) * (80.0f / 203.0f);
              const float g = HalfToFloat(pixel[1]) * (80.0f / 203.0f);
              const float b = HalfToFloat(pixel[2]) * (80.0f / 203.0f);
              StorePixel(snapshot, vx, vy, r, g, b, peakNits);
            } else if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
              const uint32_t packed = reinterpret_cast<const uint32_t*>(row)[source.x];
              const float pr = static_cast<float>(packed & 0x3FF) / 1023.0f;
              const float pg = static_cast<float>((packed >> 10) & 0x3FF) / 1023.0f;
              const float pb = static_cast<float>((packed >> 20) & 0x3FF) / 1023.0f;
              const auto rgb = Rec2020To709(PqToNits(pr) / 203.0f, PqToNits(pg) / 203.0f,
                                            PqToNits(pb) / 203.0f);
              StorePixel(snapshot, vx, vy, rgb[0], rgb[1], rgb[2], peakNits);
            } else {
              const uint8_t* pixel = row + source.x * 4;
              if (anyHdr) StoreSdrPixel(snapshot, vx, vy, pixel[2], pixel[1], pixel[0]);
              else StoreBgraPixel(snapshot, vx, vy, pixel[2], pixel[1], pixel[0]);
            }
          }
        }
      }
      capturedOutput = true;
      break;
    }
    if (!capturedOutput) { error = L"没有取得有效的桌面画面。"; return false; }
  }
  return snapshot.IsValid();
}

bool DesktopCapture::CaptureGdi(DesktopSnapshot& snapshot, std::wstring& error) {
  const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 0 || height <= 0) { error = L"虚拟桌面尺寸无效。"; return false; }
  HDC screen = GetDC(nullptr);
  if (!screen) { error = L"GetDC 失败。"; return false; }
  ScopeExit releaseScreen{[&] { ReleaseDC(nullptr, screen); }};
  HDC memory = CreateCompatibleDC(screen);
  if (!memory) { error = L"CreateCompatibleDC 失败。"; return false; }
  ScopeExit deleteMemory{[&] { DeleteDC(memory); }};
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) { error = L"CreateDIBSection 失败。"; return false; }
  ScopeExit deleteBitmap{[&] { DeleteObject(bitmap); }};
  HGDIOBJ old = SelectObject(memory, bitmap);
  ScopeExit restore{[&] { SelectObject(memory, old); }};
  if (!BitBlt(memory, 0, 0, width, height, screen, x, y, SRCCOPY | CAPTUREBLT) &&
      !BitBlt(memory, 0, 0, width, height, screen, x, y, SRCCOPY)) {
    error = L"BitBlt 失败：" + HResultMessage(HRESULT_FROM_WIN32(GetLastError())); return false;
  }
  snapshot.virtualBounds = {x, y, x + width, y + height};
  snapshot.width = width; snapshot.height = height; snapshot.bgraStride = width * 4;
  snapshot.bgra.assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) +
                       static_cast<size_t>(snapshot.bgraStride * height));
  // The GDI fallback never produces HDR regions, so snapshot.hasHdr stays false and the
  // half-float HDR buffer is never consumed by the exporter. Just fix the undefined alpha.
  for (int py = 0; py < height; ++py) {
    uint8_t* row = snapshot.bgra.data() + static_cast<size_t>(py * snapshot.bgraStride);
    for (int px = 0; px < width; ++px) row[px * 4 + 3] = 255;
  }
  return true;
}

void EnumerateWindows(DesktopSnapshot& snapshot) {
  snapshot.windows.clear();
  EnumWindows(WindowEnumerator, reinterpret_cast<LPARAM>(&snapshot.windows));
  snapshot.windows.erase(std::remove_if(snapshot.windows.begin(), snapshot.windows.end(), [&](const WindowCandidate& candidate) {
    RECT intersection{};
    return !IntersectRect(&intersection, &candidate.bounds, &snapshot.virtualBounds);
  }), snapshot.windows.end());
}

}  // namespace rc
