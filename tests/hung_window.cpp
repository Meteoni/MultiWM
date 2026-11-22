#include <windows.h>

namespace {

constexpr int kHangButtonId = 1;

LRESULT CALLBACK HungWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE: {
      const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
      const HWND button = CreateWindowExW(
          0, L"BUTTON", L"Hang for 60 seconds", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
          16, 16, 160, 32, window, reinterpret_cast<HMENU>(kHangButtonId), creation->hInstance, nullptr);
      return button ? 0 : -1;
    }
    case WM_COMMAND:
      if (LOWORD(wParam) == kHangButtonId && HIWORD(wParam) == BN_CLICKED) {
        // Block the window's message pump to trigger Windows' real hung-window handling.
        Sleep(60'000);
        return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  constexpr wchar_t kWindowClass[] = L"MultiWMHungWindowTest";
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = HungWindowProc;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  windowClass.lpszClassName = kWindowClass;
  if (!RegisterClassW(&windowClass)) return 1;

  const HWND window = CreateWindowExW(
      0, kWindowClass, L"MultiWM Hung Window Test", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, 480, 320, nullptr, nullptr, instance, nullptr);
  if (!window) return 1;
  ShowWindow(window, showCommand);

  MSG message{};
  BOOL result;
  while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return result == 0 ? static_cast<int>(message.wParam) : 1;
}
