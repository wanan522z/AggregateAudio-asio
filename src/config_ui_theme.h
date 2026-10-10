#pragma once

#include <windows.h>
#include <commctrl.h>

#include <string_view>

namespace config_theme {

inline constexpr COLORREF kCanvas = RGB(247, 247, 245);
inline constexpr COLORREF kSurface = RGB(255, 255, 255);
inline constexpr COLORREF kInk = RGB(35, 35, 35);
inline constexpr COLORREF kMuted = RGB(112, 112, 108);
inline constexpr COLORREF kBorder = RGB(229, 229, 225);
inline constexpr COLORREF kHover = RGB(243, 243, 240);
inline constexpr COLORREF kSelected = RGB(233, 242, 237);
inline constexpr COLORREF kAccent = RGB(29, 112, 84);
inline constexpr COLORREF kLog = RGB(250, 250, 248);

// Owns the fonts, brushes, and list images for one window and its DPI.
struct Theme {
  UINT dpi{96};
  HFONT body{};
  HFONT small_font{};
  HFONT strong{};
  HFONT title{};
  HBRUSH canvas{};
  HBRUSH surface{};
  HBRUSH log{};
  HIMAGELIST rows{};
  HIMAGELIST checks{};

  Theme();
  ~Theme();
  Theme(const Theme&) = delete;
  Theme& operator=(const Theme&) = delete;
  int px(int value) const;
  void set_dpi(UINT value);
  void style_button(HWND button);
  void style_combo(HWND combo);
  void style_list(HWND list);
  void update_list_images(HWND list);
};

void rounded_rect(HDC dc, RECT rect, int radius, COLORREF fill,
                  COLORREF border = CLR_INVALID);
void text(HDC dc, std::wstring_view value, RECT rect, HFONT font,
          COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
void draw_brand(HDC dc, RECT rect);
void draw_button(const Theme& theme, const DRAWITEMSTRUCT& item, bool primary);
void draw_combo_item(const Theme& theme, const DRAWITEMSTRUCT& item);
LRESULT draw_list(const Theme& theme, NMLVCUSTOMDRAW& draw);

} // namespace config_theme
