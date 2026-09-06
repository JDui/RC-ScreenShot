// Full-frame D2D replica probe: replays the real overlay's Paint sequence
// (desktop bitmap, dim, selection border, toolbar layer + backdrop + tool
// buttons + copy/save + long entry panel) and then the offending DRAW commands
// that the real frame flushes with 0x88990012 (D2DERR_EXCEEDS_ACTIVE_PIXELS).
// Usage: probe_d2d.exe [variant]
//   variant 0 = full replica (default)
//   variant 1 = full replica minus the desktop bitmap draw
//   variant 2 = full replica minus the layer/mask push
//   variant 3 = tool-only bar (no copy/save/entry) then the entry glyph
//   variant 4 = just entry fill+glyph on a 1280x800 window
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static const wchar_t* Hr(HRESULT hr) {
  static wchar_t buf[32];
  swprintf_s(buf, L"%#010lx", hr);
  return buf;
}

static ID2D1StrokeStyle* RoundCapStyle(ID2D1Factory* f) {
  ID2D1StrokeStyle* style = nullptr;
  f->CreateStrokeStyle(
      D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                  D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f),
      nullptr, 0, &style);
  return style;
}

struct IconGrid {
  float unit = 0, ox = 0, oy = 0;
  static IconGrid For(const D2D1_RECT_F& rect) {
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const float unit = (width < height ? width : height) / 24.0f;
    return {unit, rect.left + (width - 24.0f * unit) * 0.5f,
            rect.top + (height - 24.0f * unit) * 0.5f};
  }
  D2D1_POINT_2F P(float x, float y) const { return {ox + x * unit, oy + y * unit}; }
};

struct GlyphPen {
  ID2D1RenderTarget* rt = nullptr;
  ID2D1SolidColorBrush* brush = nullptr;
  ID2D1StrokeStyle* style = nullptr;
  IconGrid grid;
  float Weight(float units) const { return units * grid.unit; }
  void Line(float x1, float y1, float x2, float y2, float weight = 1.7f) const {
    rt->DrawLine(grid.P(x1, y1), grid.P(x2, y2), brush, Weight(weight), style);
  }
  void RoundedRect(float left, float top, float right, float bottom, float radius,
                   float weight = 1.7f) const {
    rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(grid.ox + left * grid.unit, grid.oy + top * grid.unit,
                                      grid.ox + right * grid.unit, grid.oy + bottom * grid.unit),
                          Weight(radius), Weight(radius)),
        brush, Weight(weight), style);
  }
  void Circle(float cx, float cy, float radius, float weight = 1.7f) const {
    D2D1_ELLIPSE ellipse{grid.P(cx, cy), Weight(radius), Weight(radius)};
    rt->DrawEllipse(ellipse, brush, Weight(weight), style);
  }
  void Dot(float cx, float cy, float radius) const {
    D2D1_ELLIPSE ellipse{grid.P(cx, cy), Weight(radius), Weight(radius)};
    rt->FillEllipse(ellipse, brush);
  }
};

static ID2D1RenderTarget* rt = nullptr;

#define PROBE(label, expr)                                                  \
  do {                                                                      \
    expr;                                                                   \
    HRESULT fhr = rt->Flush();                                              \
    if (FAILED(fhr)) printf("%-42s FAILED %s\n", label, Hr(fhr));          \
    else printf("%-42s ok\n", label);                                       \
  } while (0)

// Real geometry from the live overlay: toolbar (40,262)-(660,426).
static const D2D1_RECT_F kToolbar = D2D1::RectF(40, 262, 660, 426);
static const D2D1_RECT_F kEntry = D2D1::RectF(519, 294, 555, 330);
static const D2D1_RECT_F kCopy = D2D1::RectF(560, 294, 600, 330);
static const D2D1_RECT_F kSave = D2D1::RectF(605, 294, 645, 330);

static void DrawToolIcon(ID2D1Factory* factory, ID2D1SolidColorBrush* dimIcon,
                         ID2D1SolidColorBrush* brightIcon, ID2D1StrokeStyle* rounded,
                         const D2D1_RECT_F& rect) {
  GlyphPen pen{rt, dimIcon, rounded, IconGrid::For(rect)};
  (void)factory; (void)brightIcon;
  // Pen tool glyph.
  pen.Line(5.5f, 18.5f, 16.4f, 4.4f, 1.9f);
  pen.Line(16.4f, 4.4f, 19.6f, 7.6f, 1.9f);
  pen.Line(19.6f, 7.6f, 5.5f, 18.5f, 1.9f);
  pen.Line(7.4f, 13.4f, 10.6f, 16.6f, 1.4f);
}

static void DrawActionIcon(bool save, const D2D1_RECT_F& rect, ID2D1SolidColorBrush* icon,
                           ID2D1StrokeStyle* rounded) {
  GlyphPen pen{rt, icon, rounded, IconGrid::For(rect)};
  if (!save) {
    pen.RoundedRect(8.5f, 4.5f, 19.5f, 15.5f, 2.0f, 1.7f);
    pen.RoundedRect(4.5f, 8.5f, 15.5f, 19.5f, 2.0f, 1.7f);
    return;
  }
  pen.Line(12, 4.5f, 12, 13.4f, 1.9f);
  pen.Line(8.8f, 10.4f, 12, 13.6f, 1.9f);
  pen.Line(12, 13.6f, 15.2f, 10.4f, 1.9f);
}

static void DrawLongGlyph(const D2D1_RECT_F& rect, ID2D1SolidColorBrush* white,
                          ID2D1StrokeStyle* rounded, bool perStroke) {
  GlyphPen pen{rt, white, rounded, IconGrid::For(rect)};
  if (!perStroke) {
    pen.RoundedRect(5, 3.5f, 15, 16.5f, 2.0f, 1.5f);
    pen.Line(7, 6.8f, 13, 6.8f, 1.4f);
    pen.Line(7, 9.6f, 11.2f, 9.6f, 1.4f);
    pen.Line(7, 12.4f, 9.5f, 12.4f, 1.4f);
    pen.Line(14.75f, 8.75f, 18, 12, 1.9f);
    pen.Line(18, 12, 21.25f, 8.75f, 1.9f);
    pen.Line(4.5f, 19.25f, 19.5f, 19.25f, 1.5f);
    pen.Line(4.5f, 17.75f, 4.5f, 19.25f, 1.5f);
    pen.Line(19.5f, 17.75f, 19.5f, 19.25f, 1.5f);
  } else {
    PROBE("glyph rr", pen.RoundedRect(5, 3.5f, 15, 16.5f, 2.0f, 1.5f));
    PROBE("glyph l1", pen.Line(7, 6.8f, 13, 6.8f, 1.4f));
    PROBE("glyph l2", pen.Line(7, 9.6f, 11.2f, 9.6f, 1.4f));
    PROBE("glyph l3", pen.Line(7, 12.4f, 9.5f, 12.4f, 1.4f));
    PROBE("glyph l4", pen.Line(14.75f, 8.75f, 18, 12, 1.9f));
    PROBE("glyph l5", pen.Line(18, 12, 21.25f, 8.75f, 1.9f));
    PROBE("glyph l6", pen.Line(4.5f, 19.25f, 19.5f, 19.25f, 1.5f));
    PROBE("glyph l7", pen.Line(4.5f, 17.75f, 4.5f, 19.25f, 1.5f));
    PROBE("glyph l8", pen.Line(19.5f, 17.75f, 19.5f, 19.25f, 1.5f));
  }
}

int main(int argc, char** argv) {
  const int variant = argc > 1 ? atoi(argv[1]) : 0;
  const int W = 1280, H = 800;

  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
  };
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"ProbeWindow";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ProbeWindow", L"probe",
                              WS_POPUP, 40, 40, W, H, nullptr, nullptr, nullptr, nullptr);

  ID2D1Factory* factory = nullptr;
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory));
  ID2D1HwndRenderTarget* rtRaw = nullptr;
  HRESULT hr = factory->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
      D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(W, H), D2D1_PRESENT_OPTIONS_IMMEDIATELY),
      &rtRaw);
  if (FAILED(hr)) { printf("create target failed %s\n", Hr(hr)); return 1; }
  rtRaw->SetDpi(96, 96);
  rt = rtRaw;
  ID2D1StrokeStyle* rounded = RoundCapStyle(factory);

  ID2D1SolidColorBrush *white = nullptr, *dimIcon = nullptr, *brightIcon = nullptr,
                       *blue = nullptr, *blueOutline = nullptr, *dim = nullptr,
                       *border = nullptr, *toolBg = nullptr, *activeBg = nullptr,
                       *actionC = nullptr, *actionS = nullptr, *iconAct = nullptr;
  rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
  rt->CreateSolidColorBrush(D2D1::ColorF(.74f, .80f, .90f, 1.0f), &dimIcon);
  rt->CreateSolidColorBrush(D2D1::ColorF(.98f, .99f, 1.0f, 1.0f), &brightIcon);
  rt->CreateSolidColorBrush(D2D1::ColorF(.12f, .55f, .95f, .95f), &blue);
  rt->CreateSolidColorBrush(D2D1::ColorF(.30f, .62f, .98f, 1.0f), &blueOutline);
  rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, .48f), &dim);
  rt->CreateSolidColorBrush(D2D1::ColorF(.10f, .65f, 1.0f, 1.0f), &border);
  rt->CreateSolidColorBrush(D2D1::ColorF(.11f, .14f, .19f, .95f), &toolBg);
  rt->CreateSolidColorBrush(D2D1::ColorF(.12f, .55f, .95f, .95f), &activeBg);
  rt->CreateSolidColorBrush(D2D1::ColorF(.10f, .33f, .25f, .98f), &actionC);
  rt->CreateSolidColorBrush(D2D1::ColorF(.10f, .28f, .48f, .98f), &actionS);
  rt->CreateSolidColorBrush(D2D1::ColorF(.94f, .98f, 1.0f, 1.0f), &iconAct);

  // Desktop snapshot bitmap (the overlay paints capture pixels at 1:1).
  ID2D1Bitmap* desktop = nullptr;
  {
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
      px[i] = 0x55; px[i + 1] = 0x66; px[i + 2] = 0x77; px[i + 3] = 0xFF;
    }
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    rt->CreateBitmap(D2D1::SizeU(W, H), px.data(), W * 4, bp, &desktop);
  }

  rt->BeginDraw();
  rt->SetTransform(D2D1::Matrix3x2F::Identity());
  PROBE("draw desktop bitmap", rt->DrawBitmap(desktop, D2D1::RectF(0, 0, W, H)));
  const D2D1_RECT_F sel = D2D1::RectF(40, 40, 520, 250);
  if (variant != 1) {
    rt->FillRectangle(D2D1::RectF(0, 0, W, sel.top), dim);
    rt->FillRectangle(D2D1::RectF(0, sel.bottom, W, H), dim);
    rt->FillRectangle(D2D1::RectF(0, sel.top, sel.left, sel.bottom), dim);
    rt->FillRectangle(D2D1::RectF(sel.right, sel.top, W, sel.bottom), dim);
  }
  PROBE("dim + border", rt->DrawRectangle(sel, border, 2.0f));

  // Toolbar backdrop (mip + blur is skipped; solid fill stands in).
  if (variant != 2 && variant != 4) {
    rt->FillRoundedRectangle(D2D1::RoundedRect(kToolbar, 12, 12), toolBg);
  }
  PROBE("toolbar bg fill", S_OK);

  // Tool button row: first two tool buttons (fills + icons).
  if (variant != 4) {
    for (int i = 0; i < 2; ++i) {
      const D2D1_RECT_F item = D2D1::RectF(48 + i * 41.f, 296.f, 84 + i * 41.f, 332.f);
      PROBE(i == 0 ? "tool0 fill" : "tool1 fill",
            rt->FillRoundedRectangle(D2D1::RoundedRect(item, 9, 9), i == 0 ? activeBg : toolBg));
      PROBE(i == 0 ? "tool0 icon" : "tool1 icon",
            DrawToolIcon(factory, dimIcon, brightIcon, rounded, item));
    }
  }
  // Copy / save.
  if (variant != 3 && variant != 4) {
    PROBE("copy fill", rt->FillRoundedRectangle(D2D1::RoundedRect(kCopy, 9, 9), actionC));
    PROBE("copy icon", DrawActionIcon(false, kCopy, iconAct, rounded));
    PROBE("save fill", rt->FillRoundedRectangle(D2D1::RoundedRect(kSave, 9, 9), actionS));
    PROBE("save icon", DrawActionIcon(true, kSave, iconAct, rounded));
  }
  // Entry panel then glyph (the real failing point).
  if (variant != 3) {
    PROBE("entry fill", rt->FillRoundedRectangle(D2D1::RoundedRect(kEntry, 9, 9), blue));
    PROBE("entry outline", rt->DrawRoundedRectangle(D2D1::RoundedRect(kEntry, 9, 9), blueOutline, 1.0f));
    DrawLongGlyph(kEntry, white, rounded, true);
  } else {
    DrawLongGlyph(kEntry, white, rounded, true);
  }

  HRESULT final = rt->EndDraw();
  printf("EndDraw %s\n", Hr(final));
  return 0;
}