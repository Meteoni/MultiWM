// Direct2D resources for the retained overlays. Owned by the presenting thread.
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace PopupSurface {

using Microsoft::WRL::ComPtr;

template <class Function>
static Function LoadExport(const wchar_t* library, const char* name) {
  HMODULE module = GetModuleHandleW(library);
  if (!module) module = LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}

static D2D1_COLOR_F Color(COLORREF color, float alpha) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, alpha);
}

struct Device {
  ComPtr<ID2D1DeviceContext> drawing;
  ComPtr<IDCompositionDevice> composition;
  ComPtr<IDWriteFactory> text;
  ComPtr<IWICImagingFactory> imaging;

  bool InitializeDrawing() {
    if (composition) return true;
    using CreateD3d = decltype(&D3D11CreateDevice);
    using CreateD2d = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    using CreateComposition = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);
    static const auto createD3d = LoadExport<CreateD3d>(L"d3d11.dll", "D3D11CreateDevice");
    static const auto createD2d = LoadExport<CreateD2d>(L"d2d1.dll", "D2D1CreateFactory");
    static const auto createComposition = LoadExport<CreateComposition>(L"dcomp.dll", "DCompositionCreateDevice");
    if (!createD3d || !createD2d || !createComposition) return false;
    ComPtr<ID3D11Device> graphics;
    for (auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
      if (SUCCEEDED(createD3d(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                              D3D11_SDK_VERSION, &graphics, nullptr, nullptr))) break;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> drawingDevice;
    if (!graphics || FAILED(graphics.As(&dxgi)) ||
        FAILED(createD2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                         reinterpret_cast<void**>(factory.GetAddressOf()))) ||
        FAILED(factory->CreateDevice(dxgi.Get(), &drawingDevice)) ||
        FAILED(drawingDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &drawing)) ||
        FAILED(createComposition(dxgi.Get(), IID_PPV_ARGS(&composition)))) return false;
    drawing->SetDpi(96.0f, 96.0f);
    drawing->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
  }

  ComPtr<IDWriteTextLayout> LayoutText(const std::wstring& label, float size, float maxWidth, float height) {
    if (!text) {
      using CreateFactory = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
      static const auto create = LoadExport<CreateFactory>(L"dwrite.dll", "DWriteCreateFactory");
      if (!create || FAILED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(text.GetAddressOf())))) return {};
    }
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return {};
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (FAILED(text->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()), format.Get(),
                                      maxWidth, height, &layout))) return {};
    return layout;
  }

  ComPtr<ID2D1Bitmap> IconBitmap(ID2D1RenderTarget& target, HICON icon) {
    if (!imaging) {
      constexpr CLSID factory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
      if (FAILED(CoCreateInstance(factory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging)))) return {};
    }
    ComPtr<IWICBitmap> source;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(imaging->CreateBitmapFromHICON(icon, &source)) ||
        FAILED(imaging->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeMedianCut)) ||
        FAILED(target.CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) return {};
    return bitmap;
  }
};

struct Surface {
  ComPtr<ID2D1DeviceContext> target;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<IDCompositionTarget> host;
  ComPtr<IDCompositionVisual> visual;
  ComPtr<IDCompositionSurface> content;
  D2D1_SIZE_U size{};

  bool Attach(Device& device, HWND window, D2D1_SIZE_U requestedSize) {
    if (content && size.width == requestedSize.width && size.height == requestedSize.height) return true;
    if (!device.InitializeDrawing()) return false;
    auto& composition = *device.composition.Get();
    if (!host) {
      if (FAILED(composition.CreateTargetForHwnd(window, TRUE, &host)) ||
          FAILED(composition.CreateVisual(&visual)) || FAILED(host->SetRoot(visual.Get()))) return false;
      target = device.drawing;
      if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0.0f), &brush))) return false;
    }
    if (FAILED(composition.CreateSurface(requestedSize.width, requestedSize.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                         DXGI_ALPHA_MODE_PREMULTIPLIED, content.ReleaseAndGetAddressOf())) ||
        FAILED(visual->SetContent(content.Get()))) return false;
    size = requestedSize;
    return true;
  }

  bool BeginDraw() {
    ComPtr<IDXGISurface> buffer;
    POINT offset{};
    if (FAILED(content->BeginDraw(nullptr, IID_PPV_ARGS(&buffer), &offset))) return false;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(target->CreateBitmapFromDxgiSurface(buffer.Get(), &properties, &bitmap))) {
      content->EndDraw();
      return false;
    }
    target->SetTarget(bitmap.Get());
    target->BeginDraw();
    // DirectComposition can allocate this surface in a shared atlas.
    target->SetTransform(D2D1::Matrix3x2F::Translation(offset.x, offset.y));
    target->PushAxisAlignedClip(D2D1::RectF(0, 0, size.width, size.height), D2D1_ANTIALIAS_MODE_ALIASED);
    return true;
  }

  bool EndDraw(Device& device) {
    target->PopAxisAlignedClip();
    const HRESULT drawn = target->EndDraw();
    target->SetTarget(nullptr);
    const HRESULT submitted = content->EndDraw();
    return SUCCEEDED(drawn) && SUCCEEDED(submitted) && SUCCEEDED(device.composition->Commit());
  }

  void Fill(const D2D1_RECT_F& bounds, float radius, COLORREF color, float alpha) {
    if (alpha <= 0 || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    radius = std::min({radius, (bounds.right - bounds.left) / 2, (bounds.bottom - bounds.top) / 2});
    brush->SetColor(Color(color, alpha));
    target->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), brush.Get());
  }

  void Outline(D2D1_RECT_F bounds, float radius, float width, COLORREF color, float alpha) {
    const float half = width / 2;
    radius = std::min({radius, (bounds.right - bounds.left) / 2, (bounds.bottom - bounds.top) / 2});
    bounds.left += half;
    bounds.top += half;
    bounds.right -= half;
    bounds.bottom -= half;
    if (alpha <= 0 || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    radius = std::max(0.0f, radius - half);
    brush->SetColor(Color(color, alpha));
    target->DrawRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), brush.Get(), width);
  }
};

}  // namespace PopupSurface
