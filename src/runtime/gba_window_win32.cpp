// A Game Boy Advance screen moved out of the game window into a window of its
// own (right-click a GBA screen, "Move to its own window"). Plain Win32 and
// GDI: the frames are tiny, and the game's renderer stays out of it.
#include "gba_screen_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Core/NetPlay/NetPlayProto.h"

namespace moderngekko::gba_screen::detail
{
namespace
{
struct Window
{
  HWND hwnd = nullptr;
  std::vector<std::uint32_t> bgra;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t frame = 0;
  int alpha = -1;
};

std::array<Window, 4> s_windows;
constexpr wchar_t kClassName[] = L"ModernGekkoGameBoyAdvance";

constexpr std::array<COLORREF, 4> kPlayerColors = {RGB(222, 58, 58), RGB(58, 112, 232),
                                                   RGB(232, 186, 34), RGB(52, 176, 88)};

enum : UINT
{
  kBackToGame = 1,
  kReset,
  kNumbers,
  kOpacityFirst = 100,  // + percent
};

int PortOf(HWND hwnd)
{
  return static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)) - 1;
}

void Paint(HWND hwnd, int port)
{
  PAINTSTRUCT ps;
  HDC target = BeginPaint(hwnd, &ps);
  RECT client;
  GetClientRect(hwnd, &client);
  const int cw = client.right, ch = client.bottom;
  // Drawn off screen first, so a resize does not flicker.
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(cw, 1), std::max(ch, 1));
  HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

  const Window& window = s_windows[port];
  if (window.width && window.height && !window.bgra.empty())
  {
    // Fit the screen, keeping its shape.
    int w = cw, h = cw * static_cast<int>(window.height) / static_cast<int>(window.width);
    if (h > ch)
    {
      h = ch;
      w = ch * static_cast<int>(window.width) / static_cast<int>(window.height);
    }
    const int x = (cw - w) / 2, y = (ch - h) / 2;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(window.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(window.height);  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);  // sharp pixels
    StretchDIBits(dc, x, y, w, h, 0, 0, static_cast<int>(window.width),
                  static_cast<int>(window.height), window.bgra.data(), &info, DIB_RGB_COLORS,
                  SRCCOPY);

    if (GetSettings().show_numbers)
    {
      const int size = std::max(16, h / 12);
      HFONT font = CreateFontW(size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
      HGDIOBJ old_font = SelectObject(dc, font);
      const std::wstring text = L"P" + std::to_wstring(port + 1);
      SIZE extent;
      GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
      RECT badge = {x + 6, y + 6, x + 6 + extent.cx + size / 2, y + 6 + extent.cy};
      HBRUSH brush = CreateSolidBrush(kPlayerColors[port]);
      FillRect(dc, &badge, brush);
      DeleteObject(brush);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &badge,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old_font);
      DeleteObject(font);
    }
  }

  BitBlt(target, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(dc);
  EndPaint(hwnd, &ps);
}

void BackToGame(int port)
{
  View view = GetSettings().views[port];
  view.detached = false;
  SetView(port, view, true);
}

void ShowMenu(HWND hwnd, int port, POINT at)
{
  const Settings settings = GetSettings();
  const View& view = settings.views[port];
  HMENU menu = CreatePopupMenu();
  HMENU opacity = CreatePopupMenu();
  for (int percent = 100; percent >= 10; percent -= 10)
    AppendMenuW(opacity, MF_STRING | (view.opacity == percent ? MF_CHECKED : 0),
                kOpacityFirst + percent, (std::to_wstring(percent) + L"%").c_str());
  AppendMenuW(menu, MF_STRING, kBackToGame, L"Move back into the game window");
  AppendMenuW(menu, MF_STRING | (NetPlay::IsNetPlayRunning() ? MF_GRAYED : 0), kReset,
              L"Reset GBA (off and on)\tL+R+Start+Select");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (settings.show_numbers ? MF_CHECKED : 0), kNumbers,
              L"Show player numbers");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), L"Opacity");
  const UINT choice = static_cast<UINT>(TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, nullptr));
  DestroyMenu(menu);  // and the opacity submenu with it

  if (choice == kBackToGame)
    BackToGame(port);
  else if (choice == kReset)
    ResetGba(port);
  else if (choice == kNumbers)
  {
    SetShowNumbers(!settings.show_numbers);
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  else if (choice > kOpacityFirst && choice <= kOpacityFirst + 100)
  {
    View changed = view;
    changed.opacity = static_cast<int>(choice - kOpacityFirst);
    SetView(port, changed, false);
  }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch (msg)
  {
  case WM_NCCREATE:
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(
                          reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    break;
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    Paint(hwnd, PortOf(hwnd));
    return 0;
  case WM_SIZE:
    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;
  case WM_CONTEXTMENU:
  {
    POINT at = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (at.x == -1 && at.y == -1)  // from the keyboard
      GetCursorPos(&at);
    ShowMenu(hwnd, PortOf(hwnd), at);
    return 0;
  }
  case WM_CLOSE:
    // Closing the window puts the screen back in the game window.
    BackToGame(PortOf(hwnd));
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND Create(int port)
{
  static const bool registered = [] {
    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    cls.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    cls.lpszClassName = kClassName;
    return RegisterClassExW(&cls) != 0;
  }();
  if (!registered)
    return nullptr;
  RECT frame = {0, 0, 480, 320};  // twice the GBA's size
  AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_LAYERED);
  const std::wstring title = L"Game Boy Advance - Player " + std::to_wstring(port + 1);
  HWND hwnd = CreateWindowExW(WS_EX_LAYERED, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
                              frame.bottom - frame.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr),
                              reinterpret_cast<LPVOID>(static_cast<INT_PTR>(port + 1)));
  if (hwnd)
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  return hwnd;
}
}  // namespace

void TickWindows()
{
  const Settings settings = GetSettings();
  for (int port = 0; port < 4; ++port)
  {
    Window& window = s_windows[port];
    const bool running =
        CopyFrame(port, &window.bgra, &window.width, &window.height, &window.frame);
    const bool wanted = running && settings.views[port].detached;
    if (!wanted)
    {
      if (window.hwnd)
      {
        DestroyWindow(window.hwnd);
        window = {};
      }
      continue;
    }
    if (!window.hwnd)
    {
      window.hwnd = Create(port);
      window.alpha = -1;
      if (!window.hwnd)
        continue;
    }
    const int alpha = settings.views[port].opacity * 255 / 100;
    if (alpha != window.alpha)
    {
      SetLayeredWindowAttributes(window.hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
      window.alpha = alpha;
    }
    static std::array<std::uint64_t, 4> s_painted{};
    if (s_painted[port] != window.frame)
    {
      s_painted[port] = window.frame;
      InvalidateRect(window.hwnd, nullptr, FALSE);
    }
    // The game window's loop only takes its own messages.
    MSG msg;
    while (window.hwnd && PeekMessageW(&msg, window.hwnd, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}
}  // namespace moderngekko::gba_screen::detail

#elif !defined(__APPLE__)  // macOS: gba_window_macos.mm

namespace moderngekko::gba_screen::detail
{
void TickWindows()
{
}
}  // namespace moderngekko::gba_screen::detail

#endif
