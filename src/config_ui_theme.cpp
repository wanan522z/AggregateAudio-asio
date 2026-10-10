#include "config_ui_theme.h"

#include <objidl.h>
#include <gdiplus.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace config_theme {
namespace {

constexpr wchar_t kHotProperty[] = L"AggregateAudio.ControlHot";

Gdiplus::Color color(COLORREF value) {
  return Gdiplus::Color(255, GetRValue(value), GetGValue(value), GetBValue(value));
}

void round_path(Gdiplus::GraphicsPath& path, RECT rect, int radius) {
  const auto x = static_cast<Gdiplus::REAL>(rect.left) + 0.5f;
  const auto y = static_cast<Gdiplus::REAL>(rect.top) + 0.5f;
  const auto width = static_cast<Gdiplus::REAL>(rect.right - rect.left) - 1.0f;
  const auto height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top) - 1.0f;
  const auto diameter = std::min(static_cast<Gdiplus::REAL>(radius * 2), std::min(width, height));
  path.AddArc(x, y, diameter, diameter, 180, 90);
  path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
  path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
  path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
  path.CloseFigure();
}

HFONT make_font(UINT dpi, int size, int weight) {
  return CreateFontW(-MulDiv(size, static_cast<int>(dpi), 96), 0, 0, 0, weight,
      FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void fill(HDC dc, RECT rect, COLORREF value) {
  const auto brush = CreateSolidBrush(value);
  FillRect(dc, &rect, brush);
  DeleteObject(brush);
}

std::wstring window_text(HWND window) {
  const int length = GetWindowTextLengthW(window);
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(window, result.data(), length + 1);
  result.resize(static_cast<size_t>(length));
  return result;
}

void draw_combo_face(HWND window, HDC dc, const Theme& theme) {
  RECT rect{};
  GetClientRect(window, &rect);
  fill(dc, rect, kSurface);
  const bool enabled = IsWindowEnabled(window) != FALSE;
  const bool focused = GetFocus() == window;
  const bool hot = GetPropW(window, kHotProperty) != nullptr;
  rounded_rect(dc, rect, theme.px(8), enabled ? kLog : kHover,
               focused ? kAccent : hot ? RGB(186, 186, 179) : kBorder);
  auto label = rect;
  label.left += theme.px(12);
  label.right -= theme.px(32);
  text(dc, window_text(window), label, theme.body, enabled ? kInk : kMuted);
  Gdiplus::Graphics graphics(dc);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  Gdiplus::Pen pen(color(enabled ? kMuted : RGB(174, 174, 169)), static_cast<float>(theme.px(1)) + 0.5f);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  const auto x = static_cast<float>(rect.right - theme.px(18));
  const auto y = static_cast<float>((rect.bottom + rect.top) / 2);
  graphics.DrawLine(&pen, x - theme.px(4), y - theme.px(2), x, y + theme.px(2));
  graphics.DrawLine(&pen, x, y + theme.px(2), x + theme.px(4), y - theme.px(2));
}

LRESULT CALLBACK control_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                             UINT_PTR kind, DWORD_PTR data) {
  auto& theme = *reinterpret_cast<Theme*>(data);
  switch (message) {
    case BM_SETSTYLE:
      if (kind == 1) {
        // Dialog keyboard navigation changes the default button style. Keep
        // owner drawing so Tab/Enter never replaces the modern button face.
        return DefSubclassProc(window, message, (wparam & ~BS_TYPEMASK) | BS_OWNERDRAW, lparam);
      }
      break;
    case WM_MOUSEMOVE:
      if (!GetPropW(window, kHotProperty)) {
        SetPropW(window, kHotProperty, reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      RemovePropW(window, kHotProperty);
      InvalidateRect(window, nullptr, FALSE);
      break;
    case WM_PAINT:
      if (kind == 2) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        draw_combo_face(window, dc, theme);
        EndPaint(window, &paint);
        return 0;
      }
      break;
    case WM_PRINTCLIENT:
      if (kind == 2) {
        draw_combo_face(window, reinterpret_cast<HDC>(wparam), theme);
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      if (kind == 2) return 1;
      break;
    case WM_NCDESTROY:
      RemovePropW(window, kHotProperty);
      RemoveWindowSubclass(window, control_proc, kind);
      break;
  }
  const auto result = DefSubclassProc(window, message, wparam, lparam);
  if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
      message == CB_SETCURSEL || message == CB_SHOWDROPDOWN || message == WM_LBUTTONUP) {
    InvalidateRect(window, nullptr, FALSE);
  }
  return result;
}

LRESULT CALLBACK header_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                            UINT_PTR, DWORD_PTR data) {
  const auto& theme = *reinterpret_cast<Theme*>(data);
  const auto result = DefSubclassProc(window, message, wparam, lparam);
  if (message == HDM_LAYOUT && result) {
    auto* layout = reinterpret_cast<HDLAYOUT*>(lparam);
    layout->pwpos->cy = theme.px(36);
    layout->prc->top = theme.px(36);
  }
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, header_proc, 1);
  return result;
}

// Keep the controls' real nonclient scrollbar geometry and scroll ranges. Only
// the face and pointer tracking are themed; wheel/keyboard/accessibility still
// use the native ListView and EDIT controls.
enum class ScrollPart { none, previous, next, page_previous, page_next, thumb };
struct ScrollSkin {
  Theme* theme{};
  bool list{};
  bool redraw{true};
  int hot_bar{-1};
  ScrollPart hot{ScrollPart::none};
  int active_bar{-1};
  ScrollPart pressed{ScrollPart::none};
  int drag_offset{};
  POINT pointer{}; // Screen coordinates, also used by held arrow/page buttons.
};
constexpr UINT_PTR kScrollTimer = 0xAA53;

struct ScrollGeometry {
  RECT bar{}; // Window coordinates, suitable for GetWindowDC / WM_PRINT.
  SCROLLINFO range{sizeof(SCROLLINFO), SIF_ALL};
  int start{}, end{}, track_start{}, track_end{}, thumb_start{}, thumb_end{};
  int maximum{};
  bool enabled{};
};

bool scroll_geometry(HWND window, int bar, ScrollGeometry& geometry) {
  const auto style = GetWindowLongPtrW(window, GWL_STYLE);
  if (!(style & (bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL))) return false;
  SCROLLBARINFO info{sizeof(info)};
  if (!GetScrollBarInfo(window, bar == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &info) ||
      !GetScrollInfo(window, bar, &geometry.range)) return false;
  RECT bounds{};
  GetWindowRect(window, &bounds);
  geometry.bar = info.rcScrollBar;
  OffsetRect(&geometry.bar, -bounds.left, -bounds.top);
  geometry.start = bar == SB_VERT ? geometry.bar.top : geometry.bar.left;
  geometry.end = bar == SB_VERT ? geometry.bar.bottom : geometry.bar.right;
  geometry.track_start = geometry.start + info.dxyLineButton;
  geometry.track_end = geometry.end - info.dxyLineButton;
  geometry.thumb_start = geometry.start + info.xyThumbTop;
  geometry.thumb_end = geometry.start + info.xyThumbBottom;
  geometry.maximum = std::max(geometry.range.nMin,
      geometry.range.nMax - static_cast<int>(std::max(1u, geometry.range.nPage)) + 1);
  geometry.enabled = IsWindowEnabled(window) && geometry.maximum > geometry.range.nMin &&
      !(info.rgstate[0] & STATE_SYSTEM_UNAVAILABLE);
  return geometry.end > geometry.start;
}

ScrollPart scroll_hit(HWND window, int bar, POINT screen) {
  ScrollGeometry geometry;
  if (!scroll_geometry(window, bar, geometry) || !geometry.enabled) return ScrollPart::none;
  RECT bounds{};
  GetWindowRect(window, &bounds);
  POINT point{screen.x - bounds.left, screen.y - bounds.top};
  if (!PtInRect(&geometry.bar, point)) return ScrollPart::none;
  const int axis = bar == SB_VERT ? point.y : point.x;
  if (axis < geometry.track_start) return ScrollPart::previous;
  if (axis >= geometry.track_end) return ScrollPart::next;
  if (axis < geometry.thumb_start) return ScrollPart::page_previous;
  if (axis >= geometry.thumb_end) return ScrollPart::page_next;
  return ScrollPart::thumb;
}

void draw_scrollbars(HWND window, HDC dc, const ScrollSkin& skin) {
  for (const int bar : {SB_VERT, SB_HORZ}) {
    ScrollGeometry geometry;
    if (!scroll_geometry(window, bar, geometry)) continue;
    fill(dc, geometry.bar, kLog);
    if (geometry.enabled && geometry.thumb_end > geometry.thumb_start) {
      auto thumb = geometry.bar;
      const int thickness = bar == SB_VERT ? thumb.right - thumb.left : thumb.bottom - thumb.top;
      const int inset = std::max(2, (thickness - skin.theme->px(7)) / 2);
      if (bar == SB_VERT) {
        thumb.top = geometry.thumb_start + 1;
        thumb.bottom = geometry.thumb_end - 1;
        InflateRect(&thumb, -inset, 0);
      } else {
        thumb.left = geometry.thumb_start + 1;
        thumb.right = geometry.thumb_end - 1;
        InflateRect(&thumb, 0, -inset);
      }
      const auto shade = skin.active_bar == bar && skin.pressed == ScrollPart::thumb ? kAccent :
          skin.hot_bar == bar && skin.hot == ScrollPart::thumb ? RGB(148, 154, 145) : RGB(197, 201, 191);
      rounded_rect(dc, thumb, skin.theme->px(5), shade);
    }
    // Small chevrons retain the native arrow targets without square buttons.
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (const auto part : {ScrollPart::previous, ScrollPart::next}) {
      const auto shade = !geometry.enabled ? kBorder :
          skin.active_bar == bar && skin.pressed == part ? kAccent : kMuted;
      Gdiplus::Pen pen(color(shade), 1.3f * static_cast<float>(skin.theme->dpi) / 96.0f);
      pen.SetStartCap(Gdiplus::LineCapRound);
      pen.SetEndCap(Gdiplus::LineCapRound);
      const float axis = part == ScrollPart::previous ?
          (geometry.start + geometry.track_start) / 2.0f : (geometry.track_end + geometry.end) / 2.0f;
      const float cross = bar == SB_VERT ? (geometry.bar.left + geometry.bar.right) / 2.0f :
          (geometry.bar.top + geometry.bar.bottom) / 2.0f;
      const float size = std::min(3.0f * skin.theme->dpi / 96.0f,
          (geometry.track_start - geometry.start) / 4.0f);
      const float direction = part == ScrollPart::previous ? -1.0f : 1.0f;
      if (bar == SB_VERT) {
        graphics.DrawLine(&pen, cross - size, axis - direction * size / 2, cross, axis + direction * size / 2);
        graphics.DrawLine(&pen, cross, axis + direction * size / 2, cross + size, axis - direction * size / 2);
      } else {
        graphics.DrawLine(&pen, axis - direction * size / 2, cross - size, axis + direction * size / 2, cross);
        graphics.DrawLine(&pen, axis + direction * size / 2, cross, axis - direction * size / 2, cross + size);
      }
    }
  }
  ScrollGeometry vertical, horizontal;
  if (scroll_geometry(window, SB_VERT, vertical) && scroll_geometry(window, SB_HORZ, horizontal))
    fill(dc, {vertical.bar.left, horizontal.bar.top, vertical.bar.right, horizontal.bar.bottom}, kLog);
}

void repaint_scrollbars(HWND window, const ScrollSkin& skin) {
  if (!skin.redraw || !(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE)) return;
  const auto dc = GetWindowDC(window);
  if (dc) { draw_scrollbars(window, dc, skin); ReleaseDC(window, dc); }
}

void scroll_step(HWND window, const ScrollSkin& skin) {
  const int command = skin.pressed == ScrollPart::previous ? SB_LINEUP :
      skin.pressed == ScrollPart::next ? SB_LINEDOWN :
      skin.pressed == ScrollPart::page_previous ? SB_PAGEUP : SB_PAGEDOWN;
  SendMessageW(window, skin.active_bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, command, 0);
}

void drag_scrollbar(HWND window, const ScrollSkin& skin) {
  ScrollGeometry geometry;
  if (!scroll_geometry(window, skin.active_bar, geometry) || !geometry.enabled) return;
  RECT bounds{};
  GetWindowRect(window, &bounds);
  const int axis = skin.active_bar == SB_VERT ? skin.pointer.y - bounds.top : skin.pointer.x - bounds.left;
  const int travel = geometry.track_end - geometry.track_start - (geometry.thumb_end - geometry.thumb_start);
  if (travel <= 0) return;
  const int position = geometry.range.nMin + MulDiv(
      std::clamp(axis - skin.drag_offset - geometry.track_start, 0, travel),
      geometry.maximum - geometry.range.nMin, travel);
  const int delta = position - geometry.range.nPos;
  if (!delta) return;
  if (skin.list && (skin.active_bar == SB_HORZ || ListView_IsGroupViewEnabled(window))) {
    // Grouped report views expose pixel ranges; LVM_SCROLL avoids the 16-bit
    // WM_*SCROLL thumb-position limit and leaves selection/collapse untouched.
    ListView_Scroll(window, skin.active_bar == SB_HORZ ? delta : 0,
        skin.active_bar == SB_VERT ? delta : 0);
  } else if (!skin.list) {
    SendMessageW(window, EM_LINESCROLL, skin.active_bar == SB_HORZ ? delta : 0,
        skin.active_bar == SB_VERT ? delta : 0);
  } else {
    SendMessageW(window, skin.active_bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL,
        MAKEWPARAM(SB_THUMBPOSITION, position), 0);
  }
}

LRESULT CALLBACK scroll_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                            UINT_PTR id, DWORD_PTR data) {
  auto& skin = *reinterpret_cast<ScrollSkin*>(data);
  if (message == WM_NCDESTROY) {
    KillTimer(window, kScrollTimer);
    RemoveWindowSubclass(window, scroll_proc, id);
    delete &skin;
    return DefSubclassProc(window, message, wparam, lparam);
  }
  if (message == WM_SETREDRAW) skin.redraw = wparam != 0;
  if (message == WM_SETCURSOR && (LOWORD(lparam) == HTVSCROLL || LOWORD(lparam) == HTHSCROLL)) {
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    return TRUE;
  }
  if (message == WM_NCMOUSEMOVE) {
    skin.pointer = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    skin.hot_bar = wparam == HTVSCROLL ? SB_VERT : wparam == HTHSCROLL ? SB_HORZ : -1;
    skin.hot = skin.hot_bar < 0 ? ScrollPart::none : scroll_hit(window, skin.hot_bar, skin.pointer);
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, window, 0};
    TrackMouseEvent(&track);
    repaint_scrollbars(window, skin);
  }
  if (message == WM_NCMOUSELEAVE) {
    skin.hot_bar = -1;
    skin.hot = ScrollPart::none;
    repaint_scrollbars(window, skin);
  }
  if ((message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONDBLCLK) &&
      (wparam == HTVSCROLL || wparam == HTHSCROLL)) {
    skin.pointer = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    skin.active_bar = wparam == HTVSCROLL ? SB_VERT : SB_HORZ;
    skin.pressed = scroll_hit(window, skin.active_bar, skin.pointer);
    if (skin.pressed == ScrollPart::none) { skin.active_bar = -1; return 0; }
    SetFocus(window);
    SetCapture(window);
    if (skin.pressed == ScrollPart::thumb) {
      ScrollGeometry geometry;
      RECT bounds{};
      GetWindowRect(window, &bounds);
      scroll_geometry(window, skin.active_bar, geometry);
      skin.drag_offset = (skin.active_bar == SB_VERT ? skin.pointer.y - bounds.top :
          skin.pointer.x - bounds.left) - geometry.thumb_start;
    } else {
      scroll_step(window, skin);
      SetTimer(window, kScrollTimer, 400, nullptr);
    }
    repaint_scrollbars(window, skin);
    return 0;
  }
  if (message == WM_MOUSEMOVE && skin.active_bar >= 0) {
    skin.pointer = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ClientToScreen(window, &skin.pointer);
    if (skin.pressed == ScrollPart::thumb) drag_scrollbar(window, skin);
    repaint_scrollbars(window, skin);
    return 0;
  }
  if (message == WM_TIMER && wparam == kScrollTimer && skin.active_bar >= 0) {
    if (skin.pressed != ScrollPart::thumb &&
        scroll_hit(window, skin.active_bar, skin.pointer) == skin.pressed) scroll_step(window, skin);
    SetTimer(window, kScrollTimer, 60, nullptr);
    repaint_scrollbars(window, skin);
    return 0;
  }
  if (skin.active_bar >= 0 && (message == WM_LBUTTONUP || message == WM_CANCELMODE ||
      message == WM_CAPTURECHANGED || (message == WM_SHOWWINDOW && !wparam) ||
      (message == WM_ENABLE && !wparam))) {
    const int bar = skin.active_bar;
    skin.active_bar = -1;
    skin.pressed = ScrollPart::none;
    KillTimer(window, kScrollTimer);
    if (GetCapture() == window) ReleaseCapture();
    SendMessageW(window, bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, SB_ENDSCROLL, 0);
    repaint_scrollbars(window, skin);
    if (message == WM_LBUTTONUP) return 0;
  }
  const auto result = DefSubclassProc(window, message, wparam, lparam);
  if (message == WM_PRINT && (lparam & PRF_NONCLIENT)) {
    draw_scrollbars(window, reinterpret_cast<HDC>(wparam), skin);
  } else if (message == WM_NCPAINT || message == WM_PAINT || message == WM_SIZE ||
      message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE ||
      message == WM_VSCROLL || message == WM_HSCROLL || message == WM_MOUSEWHEEL ||
      message == WM_MOUSEHWHEEL || message == WM_SETFONT || message == WM_ENABLE ||
      message == WM_SETTEXT || message == EM_REPLACESEL || message == EM_LINESCROLL ||
      message == EM_SCROLLCARET || message == LVM_SCROLL || (message == WM_SETREDRAW && wparam)) {
    repaint_scrollbars(window, skin);
  }
  return result;
}

LRESULT CALLBACK list_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                          UINT_PTR, DWORD_PTR data) {
  const auto& theme = *reinterpret_cast<Theme*>(data);
  if (message == WM_NOTIFY) {
    const auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header->hwndFrom == ListView_GetHeader(window) && header->code == NM_CUSTOMDRAW) {
      auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lparam);
      if (draw->dwDrawStage == CDDS_PREPAINT) {
        RECT rect{};
        GetClientRect(header->hwndFrom, &rect);
        fill(draw->hdc, rect, kLog);
        return CDRF_NOTIFYITEMDRAW;
      }
      if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
        fill(draw->hdc, draw->rc, kLog);
        std::array<wchar_t, 128> label{};
        HDITEMW item{};
        item.mask = HDI_TEXT;
        item.pszText = label.data();
        item.cchTextMax = static_cast<int>(label.size());
        Header_GetItem(header->hwndFrom, static_cast<int>(draw->dwItemSpec), &item);
        auto rect = draw->rc;
        rect.left += theme.px(12);
        text(draw->hdc, label.data(), rect, theme.small_font, kMuted);
        return CDRF_SKIPDEFAULT;
      }
    }
  }
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, list_proc, 1);
  return DefSubclassProc(window, message, wparam, lparam);
}

} // namespace

Theme::Theme() {
  canvas = CreateSolidBrush(kCanvas);
  surface = CreateSolidBrush(kSurface);
  log = CreateSolidBrush(kLog);
  set_dpi(96);
}

Theme::~Theme() {
  for (auto font : {body, small_font, strong, title}) if (font) DeleteObject(font);
  for (auto brush : {canvas, surface, log}) if (brush) DeleteObject(brush);
  if (rows) ImageList_Destroy(rows);
  if (checks) ImageList_Destroy(checks);
}

int Theme::px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }

void Theme::set_dpi(UINT value) {
  dpi = value ? value : 96;
  for (auto font : {body, small_font, strong, title}) if (font) DeleteObject(font);
  body = make_font(dpi, 14, FW_NORMAL);
  small_font = make_font(dpi, 12, FW_NORMAL);
  strong = make_font(dpi, 15, FW_SEMIBOLD);
  title = make_font(dpi, 20, FW_SEMIBOLD);
}

void Theme::style_button(HWND button) {
  SetWindowSubclass(button, control_proc, 1, reinterpret_cast<DWORD_PTR>(this));
}

void Theme::style_combo(HWND combo) {
  SetWindowTheme(combo, L"", L"");
  SetWindowSubclass(combo, control_proc, 2, reinterpret_cast<DWORD_PTR>(this));
  SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(30));
  SendMessageW(combo, CB_SETITEMHEIGHT, 0, px(32));
}

void Theme::style_list(HWND list) {
  SetWindowTheme(list, L"", L"");
  ListView_SetBkColor(list, kSurface);
  ListView_SetTextBkColor(list, CLR_NONE);
  ListView_SetTextColor(list, kInk);
  const auto header = ListView_GetHeader(list);
  SetWindowTheme(header, L"", L"");
  SetWindowSubclass(header, header_proc, 1, reinterpret_cast<DWORD_PTR>(this));
  SetWindowSubclass(list, list_proc, 1, reinterpret_cast<DWORD_PTR>(this));
  style_scrollbars(list);
  update_list_images(list);
}

void Theme::style_scrollbars(HWND control) {
  DWORD_PTR existing{};
  if (GetWindowSubclass(control, scroll_proc, 3, &existing)) return;
  SetWindowTheme(control, L"", L"");
  auto skin = std::make_unique<ScrollSkin>();
  skin->theme = this;
  std::array<wchar_t, 64> klass{};
  GetClassNameW(control, klass.data(), static_cast<int>(klass.size()));
  skin->list = std::wcscmp(klass.data(), WC_LISTVIEWW) == 0;
  if (SetWindowSubclass(control, scroll_proc, 3, reinterpret_cast<DWORD_PTR>(skin.get()))) skin.release();
}

void Theme::update_list_images(HWND list) {
  const auto previous_rows = rows;
  const auto previous_checks = checks;
  // ListView adds its own vertical padding to this image height.
  rows = ImageList_Create(1, px(26), ILC_COLOR32, 1, 1);
  ListView_SetImageList(list, rows, LVSIL_SMALL);

  const int size = px(22);
  checks = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 2, 0);
  const auto screen = GetDC(nullptr);
  const auto dc = CreateCompatibleDC(screen);
  const auto bitmap = CreateCompatibleBitmap(screen, size * 2, size);
  const auto old_bitmap = SelectObject(dc, bitmap);
  // Antialiasing blends edge pixels with this color before the exact mask is
  // removed. Use a neutral canvas color so unchecked boxes have no pink fringe.
  const COLORREF mask = kCanvas;
  fill(dc, {0, 0, size * 2, size}, mask);
  for (int checked = 0; checked < 2; ++checked) {
    const int x = checked * size;
    RECT rect{x + px(3), px(3), x + size - px(3), size - px(3)};
    rounded_rect(dc, rect, px(4), checked ? kInk : kSurface,
                 checked ? kInk : RGB(199, 199, 193));
    if (checked) {
      Gdiplus::Graphics graphics(dc);
      graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      Gdiplus::Pen pen(color(kSurface), static_cast<float>(px(2)));
      pen.SetStartCap(Gdiplus::LineCapRound);
      pen.SetEndCap(Gdiplus::LineCapRound);
      graphics.DrawLine(&pen, static_cast<float>(x + px(7)), static_cast<float>(px(11)),
                        static_cast<float>(x + px(10)), static_cast<float>(px(14)));
      graphics.DrawLine(&pen, static_cast<float>(x + px(10)), static_cast<float>(px(14)),
                        static_cast<float>(x + px(15)), static_cast<float>(px(8)));
    }
  }
  SelectObject(dc, old_bitmap);
  ImageList_AddMasked(checks, bitmap, mask);
  DeleteObject(bitmap);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  const auto replaced_checks = ListView_SetImageList(list, checks, LVSIL_STATE);
  if (replaced_checks && replaced_checks != previous_checks) ImageList_Destroy(replaced_checks);
  if (previous_rows) ImageList_Destroy(previous_rows);
  if (previous_checks) ImageList_Destroy(previous_checks);
  LVGROUPMETRICS metrics{};
  metrics.cbSize = sizeof(metrics);
  metrics.mask = LVGMF_BORDERSIZE | LVGMF_TEXTCOLOR;
  metrics.Top = px(12);
  metrics.Bottom = px(8);
  metrics.crHeader = kInk;
  SendMessageW(list, LVM_SETGROUPMETRICS, 0, reinterpret_cast<LPARAM>(&metrics));
}

void rounded_rect(HDC dc, RECT rect, int radius, COLORREF fill_color, COLORREF border) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return;
  Gdiplus::Graphics graphics(dc);
  RECT clip{};
  if (GetClipBox(dc, &clip) != ERROR) {
    graphics.SetClip(Gdiplus::Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
  }
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  Gdiplus::GraphicsPath path;
  round_path(path, rect, radius);
  Gdiplus::SolidBrush brush(color(fill_color));
  graphics.FillPath(&brush, &path);
  if (border != CLR_INVALID) {
    Gdiplus::Pen pen(color(border), 1.0f);
    graphics.DrawPath(&pen, &path);
  }
}

void text(HDC dc, std::wstring_view value, RECT rect, HFONT font, COLORREF value_color, UINT format) {
  const auto previous = SelectObject(dc, font);
  const auto previous_color = SetTextColor(dc, value_color);
  const int previous_mode = SetBkMode(dc, TRANSPARENT);
  DrawTextW(dc, value.data(), static_cast<int>(value.size()), &rect, format | DT_NOPREFIX);
  SetBkMode(dc, previous_mode);
  SetTextColor(dc, previous_color);
  SelectObject(dc, previous);
}

void draw_brand(HDC dc, RECT rect) {
  rounded_rect(dc, rect, (rect.right - rect.left) / 3, kInk);
  Gdiplus::Graphics graphics(dc);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  const float width = static_cast<float>(rect.right - rect.left);
  const float middle = static_cast<float>(rect.top + rect.bottom) / 2.0f;
  Gdiplus::Pen pen(color(kSurface), width * 0.055f);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  constexpr std::array<float, 5> heights{0.16f, 0.34f, 0.52f, 0.30f, 0.16f};
  for (size_t i = 0; i < heights.size(); ++i) {
    const float x = static_cast<float>(rect.left) + width * (0.25f + 0.125f * static_cast<float>(i));
    graphics.DrawLine(&pen, x, middle - width * heights[i] / 2,
                      x, middle + width * heights[i] / 2);
  }
}

void draw_button(const Theme& theme, const DRAWITEMSTRUCT& item, bool primary) {
  fill(item.hDC, item.rcItem, kCanvas);
  RECT rect = item.rcItem;
  InflateRect(&rect, -theme.px(1), -theme.px(1));
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const bool hot = GetPropW(item.hwndItem, kHotProperty) != nullptr;
  COLORREF background = primary ? kInk : kSurface;
  COLORREF foreground = primary ? kSurface : kInk;
  if (disabled) {
    background = kHover;
    foreground = RGB(162, 162, 155);
  } else if (pressed) {
    background = primary ? RGB(12, 12, 12) : RGB(231, 231, 226);
  } else if (hot) {
    background = primary ? RGB(64, 64, 62) : kHover;
  }
  rounded_rect(item.hDC, rect, theme.px(10), background,
               primary && !disabled ? background : kBorder);
  text(item.hDC, window_text(item.hwndItem), rect, theme.body, foreground,
       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
    InflateRect(&rect, -theme.px(4), -theme.px(4));
    DrawFocusRect(item.hDC, &rect);
  }
}

void draw_combo_item(const Theme& theme, const DRAWITEMSTRUCT& item) {
  // ODS_COMBOBOXEDIT is the closed face, painted by the combo subclass.
  if (item.itemState & ODS_COMBOBOXEDIT) {
    draw_combo_face(item.hwndItem, item.hDC, theme);
    return;
  }
  const bool selected = (item.itemState & ODS_SELECTED) != 0;
  fill(item.hDC, item.rcItem, selected ? kSelected : kSurface);
  if (item.itemID == static_cast<UINT>(-1)) return;
  const auto length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
  if (length == CB_ERR) return;
  std::wstring label(static_cast<size_t>(length) + 1, L'\0');
  SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
  label.resize(static_cast<size_t>(length));
  auto rect = item.rcItem;
  rect.left += theme.px(12);
  text(item.hDC, label, rect, theme.body, (item.itemState & ODS_DISABLED) ? kMuted : kInk);
}

LRESULT draw_list(const Theme& theme, NMLVCUSTOMDRAW& draw) {
  if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
  if (draw.nmcd.dwDrawStage == CDDS_POSTPAINT) {
    const auto list = draw.nmcd.hdr.hwndFrom;
    const int count = static_cast<int>(ListView_GetGroupCount(list));
    for (int index = 0; index < count; ++index) {
      std::array<wchar_t, 1024> label{};
      LVGROUP group{};
      group.cbSize = sizeof(group);
      group.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
      group.pszHeader = label.data();
      group.cchHeader = static_cast<int>(label.size());
      group.stateMask = LVGS_COLLAPSED;
      RECT rect{};
      if (!SendMessageW(list, LVM_GETGROUPINFOBYINDEX, index, reinterpret_cast<LPARAM>(&group)) ||
          !ListView_GetGroupRect(list, group.iGroupId, LVGGR_HEADER, &rect)) continue;
      const int saved = SaveDC(draw.nmcd.hdc);
      RECT header{};
      GetClientRect(ListView_GetHeader(list), &header);
      RECT client{};
      GetClientRect(list, &client);
      IntersectClipRect(draw.nmcd.hdc, 0, header.bottom, client.right, client.bottom);
      fill(draw.nmcd.hdc, rect, kSurface);
      auto title_rect = rect;
      title_rect.left += theme.px(10);
      title_rect.right -= theme.px(34);
      text(draw.nmcd.hdc, label.data(), title_rect, theme.strong, kInk);
      Gdiplus::Graphics graphics(draw.nmcd.hdc);
      RECT clip{};
      if (GetClipBox(draw.nmcd.hdc, &clip) != ERROR) {
        graphics.SetClip(Gdiplus::Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
      }
      graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      Gdiplus::Pen pen(color(kMuted), static_cast<float>(theme.px(1)) + 0.5f);
      const float x = static_cast<float>(rect.right - theme.px(16));
      const float y = static_cast<float>((rect.top + rect.bottom) / 2);
      const float offset = static_cast<float>(theme.px(3));
      const float direction = (group.state & LVGS_COLLAPSED) ? -1.0f : 1.0f;
      graphics.DrawLine(&pen, x - offset, y - offset * direction / 2, x, y + offset * direction / 2);
      graphics.DrawLine(&pen, x, y + offset * direction / 2, x + offset, y - offset * direction / 2);
      graphics.Flush();
      RestoreDC(draw.nmcd.hdc, saved);
    }
    return CDRF_DODEFAULT;
  }
  if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT && draw.dwItemType == LVCDI_ITEM) {
    const auto list = draw.nmcd.hdr.hwndFrom;
    const int row = static_cast<int>(draw.nmcd.dwItemSpec);
    const auto state = ListView_GetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED | LVIS_STATEIMAGEMASK);
    RECT bounds{};
    if (!ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS)) return CDRF_SKIPDEFAULT;
    RECT body{};
    GetClientRect(list, &body);
    RECT header{};
    GetWindowRect(ListView_GetHeader(list), &header);
    MapWindowPoints(nullptr, list, reinterpret_cast<POINT*>(&header), 2);
    body.top = std::max(body.top, header.bottom);
    RECT visible{};
    if (!IntersectRect(&visible, &bounds, &body)) return CDRF_SKIPDEFAULT;
    const int saved = SaveDC(draw.nmcd.hdc);
    if (!saved) return CDRF_SKIPDEFAULT;
    IntersectClipRect(draw.nmcd.hdc, visible.left, visible.top, visible.right, visible.bottom);
    fill(draw.nmcd.hdc, bounds, kSurface);
    if (state & LVIS_SELECTED) {
      auto selected = bounds;
      InflateRect(&selected, 0, -theme.px(2));
      rounded_rect(draw.nmcd.hdc, selected, theme.px(6), kSelected);
    }
    const int check_y = (bounds.top + bounds.bottom - theme.px(22)) / 2;
    if (theme.checks) ImageList_Draw(theme.checks, ((state & LVIS_STATEIMAGEMASK) >> 12) == 2 ? 1 : 0,
        draw.nmcd.hdc, bounds.left + theme.px(2), check_y, ILD_TRANSPARENT);
    const int count = Header_GetItemCount(ListView_GetHeader(list));
    for (int column = 0; column < count; ++column) {
      RECT cell{};
      if (!ListView_GetSubItemRect(list, row, column, LVIR_LABEL, &cell)) continue;
      // Grouped ListViews may return stale vertical subitem coordinates after
      // scrolling. Use the validated row bounds and clip every draw to its body.
      cell.top = bounds.top;
      cell.bottom = bounds.bottom;
      if (column == 0) {
        cell.left = bounds.left + theme.px(32);
        cell.right = bounds.left + ListView_GetColumnWidth(list, 0);
      } else {
        cell.left += theme.px(12);
      }
      cell.right -= theme.px(10);
      std::array<wchar_t, 1024> label{};
      ListView_GetItemText(list, row, column, label.data(), static_cast<int>(label.size()));
      text(draw.nmcd.hdc, label.data(), cell, theme.body,
           column == 0 ? kInk : column == 1 ? kAccent : kMuted);
    }
    if ((state & LVIS_FOCUSED) && GetFocus() == list &&
        !(SendMessageW(list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
      InflateRect(&bounds, -theme.px(2), -theme.px(3));
      DrawFocusRect(draw.nmcd.hdc, &bounds);
    }
    RestoreDC(draw.nmcd.hdc, saved);
    return CDRF_SKIPDEFAULT;
  }
  return CDRF_DODEFAULT;
}

} // namespace config_theme
