// The drag preview drawn with DirectComposition: the fill, outline and app
// badges are composition visuals in one host window, drawn with Direct2D on
// the GPU, and fades are composition animations, which DWM runs at the
// display's refresh rate whatever MultiWM's threads are doing.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.
// Self-contained (Win32, Direct3D 11, Direct2D, DirectComposition, WIC), so it
// can be exercised on its own.
//
// d3d11.dll, d2d1.dll and dcomp.dll are loaded on first use, so the mod pays
// for them only once a preview is shown. Any failure (no GPU, a lost device
// after a driver update, Remote Desktop) leaves Attached() false; the caller
// then draws the preview another way.

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace PreviewComposition {

using Microsoft::WRL::ComPtr;

// One app badge: a rounded square with the app's icon centred in it.
struct BadgeScene {
  RECT frame{};  // Screen, physical pixels.
  HICON icon = nullptr;
  LONG iconSize = 0;
  LONG radius = 0;
  COLORREF background = 0;
};

// What to show, in physical screen pixels. An element with an empty frame or
// no opacity is not shown.
struct Scene {
  RECT host{};  // The host window's frame; every element lies inside it.
  COLORREF accent = 0;
  RECT fill{};
  float fillOpacity = 0;
  LONG fillRadius = 0;
  RECT outline{};
  float outlineOpacity = 0;
  LONG outlineWidth = 1;
  LONG outlineRadius = 0;
  std::optional<BadgeScene> badges[2];
};

// The extended styles a host window needs: on top, never activated, ignored
// by hit testing (layered and transparent), and without a GDI redirection
// surface, since DirectComposition supplies all of its content.
constexpr DWORD kHostExStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                               WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP;

namespace Detail {

using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                              UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D2D1CreateFactoryFn = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
using DCompositionCreateDeviceFn = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);

template <class Function>
Function LoadExport(const wchar_t* library, const char* name) {
  HMODULE module = GetModuleHandleW(library);
  if (!module) module = LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}

// CLSID_WICImagingFactory, spelled out so no import library is needed.
constexpr CLSID kWicImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};

inline D2D1_COLOR_F Color(COLORREF color, float alpha) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, alpha);
}

inline LONG Width(const RECT& rect) { return rect.right - rect.left; }
inline LONG Height(const RECT& rect) { return rect.bottom - rect.top; }

}  // namespace Detail

class Presenter {
 public:
  Presenter() = default;
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;
  ~Presenter() { Reset(); }

  // Creates the devices and binds them to `host`, a window with kHostExStyle.
  // False, leaving nothing attached, when any step fails.
  bool Attach(HWND host) {
    Reset();
    host_ = host;
    if (!CreateDevices() || !CreateTree()) {
      Reset();
      return false;
    }
    // A layered window shows nothing until its alpha is set.
    SetLayeredWindowAttributes(host_, 0, 255, LWA_ALPHA);
    return true;
  }

  bool Attached() const { return composition_ != nullptr; }
  HWND Host() const { return host_; }

  // Shows `scene` and fades the whole preview from `fromOpacity` to
  // `toOpacity` over `fadeMs`, in one composition frame. False on failure
  // (the device may be lost); the caller should Reset and draw another way.
  bool Present(const Scene& scene, float fromOpacity, float toOpacity, UINT fadeMs) {
    if (!Healthy()) return false;
    if (!PlaceHost(scene.host)) return false;
    const bool drawn =
        DrawLayer(fill_, scene.fillOpacity > 0 ? scene.fill : RECT{}, scene.host,
                  [&](D2D1_SIZE_F size) { DrawFill(scene, size); }) &&
        DrawLayer(outline_, scene.outlineOpacity > 0 ? scene.outline : RECT{}, scene.host,
                  [&](D2D1_SIZE_F size) { DrawOutline(scene, size); }) &&
        DrawBadge(badges_[0], scene.badges[0], scene.host) && DrawBadge(badges_[1], scene.badges[1], scene.host);
    return drawn && SetFade(fromOpacity, toOpacity, fadeMs) && SUCCEEDED(composition_->Commit());
  }

  // Fades what is shown from `fromOpacity` to `toOpacity` over `fadeMs`.
  bool Fade(float fromOpacity, float toOpacity, UINT fadeMs) {
    return Healthy() && SetFade(fromOpacity, toOpacity, fadeMs) && SUCCEEDED(composition_->Commit());
  }

  // Hides the host and releases the surfaces, which can be as large as a tile.
  void Hide() {
    if (host_) ShowWindow(host_, SW_HIDE);
    for (Layer* layer : {&fill_, &outline_, &badges_[0], &badges_[1]}) ClearLayer(*layer);
    if (composition_) composition_->Commit();
  }

  // Releases everything; the host window stays the caller's.
  void Reset() {
    for (Layer* layer : {&fill_, &outline_, &badges_[0], &badges_[1]}) *layer = {};
    group_.Reset();
    root_.Reset();
    target_.Reset();
    composition_.Reset();
    context_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    d3d_.Reset();
    wic_.Reset();
    host_ = nullptr;
  }

 private:
  struct Layer {
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionSurface> surface;
    SIZE size{};
  };

  bool CreateDevices() {
    const auto createD3d = Detail::LoadExport<Detail::D3D11CreateDeviceFn>(L"d3d11.dll", "D3D11CreateDevice");
    const auto createD2d = Detail::LoadExport<Detail::D2D1CreateFactoryFn>(L"d2d1.dll", "D2D1CreateFactory");
    const auto createComposition =
        Detail::LoadExport<Detail::DCompositionCreateDeviceFn>(L"dcomp.dll", "DCompositionCreateDevice");
    if (!createD3d || !createD2d || !createComposition) return false;
    // Hardware first; WARP (software) keeps the preview working without a GPU.
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
      hr = createD3d(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                     &d3d_, nullptr, nullptr);
      if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi;
    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(d3d_.As(&dxgi)) ||
        FAILED(createD2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                         reinterpret_cast<void**>(d2dFactory_.GetAddressOf()))) ||
        FAILED(d2dFactory_->CreateDevice(dxgi.Get(), &d2dDevice_)) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_)) ||
        FAILED(createComposition(dxgi.Get(), __uuidof(IDCompositionDevice),
                                 reinterpret_cast<void**>(composition_.GetAddressOf())))) {
      return false;
    }
    context_->SetDpi(96.0f, 96.0f);  // Draw in physical pixels.
    // Icons need WIC; without it badges are skipped, the rest still works.
    CoCreateInstance(Detail::kWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    return true;
  }

  bool CreateTree() {
    if (FAILED(composition_->CreateTargetForHwnd(host_, TRUE, &target_)) ||
        FAILED(composition_->CreateVisual(&root_)) || FAILED(composition_->CreateEffectGroup(&group_)) ||
        FAILED(root_->SetEffect(group_.Get())) || FAILED(target_->SetRoot(root_.Get()))) {
      return false;
    }
    // Back to front: fill, outline, then the badges over both. With no
    // reference visual, insertAbove FALSE adds on top of the siblings.
    for (Layer* layer : {&fill_, &outline_, &badges_[0], &badges_[1]}) {
      if (FAILED(composition_->CreateVisual(&layer->visual)) ||
          FAILED(root_->AddVisual(layer->visual.Get(), FALSE, nullptr))) {
        return false;
      }
    }
    return true;
  }

  bool Healthy() const { return composition_ && d3d_ && d3d_->GetDeviceRemovedReason() == S_OK; }

  bool PlaceHost(const RECT& frame) {
    RECT current{};
    if (IsWindowVisible(host_) && GetWindowRect(host_, &current) && EqualRect(&current, &frame)) return true;
    return SetWindowPos(host_, HWND_TOPMOST, frame.left, frame.top, Detail::Width(frame), Detail::Height(frame),
                        SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
  }

  bool SetFade(float fromOpacity, float toOpacity, UINT fadeMs) {
    if (fadeMs == 0 || fromOpacity == toOpacity) return SUCCEEDED(group_->SetOpacity(toOpacity));
    // A fresh animation each time: it starts when this commit is shown.
    ComPtr<IDCompositionAnimation> fade;
    const double seconds = fadeMs / 1000.0;
    return SUCCEEDED(composition_->CreateAnimation(&fade)) &&
           SUCCEEDED(fade->AddCubic(0.0, fromOpacity, static_cast<float>((toOpacity - fromOpacity) / seconds), 0, 0)) &&
           SUCCEEDED(fade->End(seconds, toOpacity)) && SUCCEEDED(group_->SetOpacity(fade.Get()));
  }

  static void ClearLayer(Layer& layer) {
    if (layer.visual) layer.visual->SetContent(nullptr);
    layer.surface.Reset();
    layer.size = {};
  }

  // Draws `layer` at `frame` (screen) with `draw`, given the layer's size in
  // pixels, reusing its surface when the size is unchanged. An empty frame
  // hides the layer.
  template <class Draw>
  bool DrawLayer(Layer& layer, const RECT& frame, const RECT& host, Draw&& draw) {
    const LONG width = Detail::Width(frame);
    const LONG height = Detail::Height(frame);
    if (width <= 0 || height <= 0) {
      ClearLayer(layer);
      return true;
    }
    if (!layer.surface || layer.size.cx != width || layer.size.cy != height) {
      layer.surface.Reset();
      if (FAILED(composition_->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                                             &layer.surface))) {
        return false;
      }
      layer.size = {width, height};
    }
    ComPtr<IDXGISurface> target;
    POINT offset{};
    if (FAILED(layer.surface->BeginDraw(nullptr, IID_PPV_ARGS(&target), &offset))) return false;
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    HRESULT hr = context_->CreateBitmapFromDxgiSurface(target.Get(), &properties, &bitmap);
    if (SUCCEEDED(hr)) {
      const D2D1_SIZE_F size = D2D1::SizeF(static_cast<float>(width), static_cast<float>(height));
      context_->SetTarget(bitmap.Get());
      context_->BeginDraw();
      // The surface may be part of a shared atlas: draw only inside our part.
      context_->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
      context_->PushAxisAlignedClip(D2D1::RectF(0, 0, size.width, size.height), D2D1_ANTIALIAS_MODE_ALIASED);
      context_->Clear(D2D1::ColorF(0, 0, 0, 0));
      draw(size);
      context_->PopAxisAlignedClip();
      hr = context_->EndDraw();
      context_->SetTarget(nullptr);
    }
    layer.surface->EndDraw();
    if (FAILED(hr)) return false;
    return SUCCEEDED(layer.visual->SetContent(layer.surface.Get())) &&
           SUCCEEDED(layer.visual->SetOffsetX(static_cast<float>(frame.left - host.left))) &&
           SUCCEEDED(layer.visual->SetOffsetY(static_cast<float>(frame.top - host.top)));
  }

  void DrawFill(const Scene& scene, D2D1_SIZE_F size) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(context_->CreateSolidColorBrush(Detail::Color(scene.accent, scene.fillOpacity), &brush))) return;
    const float radius = static_cast<float>(scene.fillRadius);
    context_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, size.width, size.height), radius, radius),
                                   brush.Get());
  }

  // A stroke centred on a path inset by half its width, so the outer edge
  // follows the frame with the configured radius, as the GDI outline does.
  void DrawOutline(const Scene& scene, D2D1_SIZE_F size) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(context_->CreateSolidColorBrush(Detail::Color(scene.accent, scene.outlineOpacity), &brush))) return;
    const float width = static_cast<float>(std::max<LONG>(1, scene.outlineWidth));
    const float half = width / 2;
    const float radius = std::max(0.0f, static_cast<float>(scene.outlineRadius) - half);
    context_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(half, half, size.width - half, size.height - half), radius, radius), brush.Get(),
        width);
  }

  // `icon` as a Direct2D bitmap. Not cached: the caller owns its icons and may
  // destroy one after a drag, when Windows can reuse its handle for another.
  ComPtr<ID2D1Bitmap> IconBitmap(HICON icon) {
    ComPtr<ID2D1Bitmap> bitmap;
    if (!icon || !wic_) return bitmap;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateBitmapFromHICON(icon, &source)) || FAILED(wic_->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeMedianCut)) ||
        FAILED(context_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) {
      bitmap.Reset();
    }
    return bitmap;
  }

  bool DrawBadge(Layer& layer, const std::optional<BadgeScene>& badge, const RECT& host) {
    const ComPtr<ID2D1Bitmap> icon = badge ? IconBitmap(badge->icon) : nullptr;
    if (!icon) {
      ClearLayer(layer);
      return true;
    }
    return DrawLayer(layer, badge->frame, host, [&](D2D1_SIZE_F size) {
      ComPtr<ID2D1SolidColorBrush> brush;
      if (SUCCEEDED(context_->CreateSolidColorBrush(Detail::Color(badge->background, 1.0f), &brush))) {
        const float radius = static_cast<float>(badge->radius);
        context_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, size.width, size.height), radius, radius),
                                       brush.Get());
      }
      const float inset = (size.width - static_cast<float>(badge->iconSize)) / 2;
      context_->DrawBitmap(icon.Get(), D2D1::RectF(inset, inset, inset + badge->iconSize, inset + badge->iconSize), 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    });
  }

  HWND host_ = nullptr;
  ComPtr<ID3D11Device> d3d_;
  ComPtr<ID2D1Factory1> d2dFactory_;
  ComPtr<ID2D1Device> d2dDevice_;
  ComPtr<ID2D1DeviceContext> context_;
  ComPtr<IDCompositionDevice> composition_;
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionEffectGroup> group_;
  ComPtr<IWICImagingFactory> wic_;
  Layer fill_, outline_, badges_[2];
};

}  // namespace PreviewComposition
