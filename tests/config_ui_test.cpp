// Exercise real native controls with an in-memory catalog and profile. No audio
// devices are opened and no user settings are read or saved.
#include <cstdio>
#define enumerate_endpoints ui_fixture_endpoints
#define load_audio_settings ui_fixture_settings
#include "../src/config_ui.cpp"
#undef enumerate_endpoints
#undef load_audio_settings

#include <cstdio>

bool ui_fixture_first_profile = false;
unsigned ui_fixture_capture_channels = 2;
bool ui_fixture_reverse_catalog = false;
bool ui_fixture_omit_capture = false;

std::vector<AudioEndpoint> ui_fixture_endpoints() {
  std::vector<AudioEndpoint> endpoints;
  for (unsigned index = 0; index < 3; ++index) {
    if (index == 0 && ui_fixture_omit_capture) continue;
    AudioEndpoint endpoint;
    endpoint.id = L"fixture-pin-" + std::to_wstring(index);
    endpoint.device_instance_id = index < 2 ? L"fixture-usb" : L"fixture-realtek";
    endpoint.device_name = index < 2 ? L"USB Audio Interface" : L"Realtek 高保真音频";
    endpoint.pin_role = index == 0 ? L"Analog 1/2" : L"Playback 1/2";
    endpoint.direction = index == 0 ? Direction::input : Direction::output;
    endpoint.ks_pin_id = index;
    endpoint.channels = index == 0 ? ui_fixture_capture_channels : 2;
    endpoint.native_sample_rates = {44100, 48000, 96000};
    endpoint.native_formats = {"PCM 24", "PCM 32", "Float 32"};
    endpoints.push_back(std::move(endpoint));
  }
  if (ui_fixture_reverse_catalog) std::reverse(endpoints.begin(), endpoints.end());
  return endpoints;
}

AudioSettings ui_fixture_settings() {
  if (ui_fixture_first_profile) return {};
  AudioSettings settings;
  settings.initialized = true;
  settings.channel_selection_configured = true;
  settings.input_channel_keys = {L"fixture-pin-0:ch=0", L"fixture-pin-0:ch=1"};
  settings.output_channel_keys = {L"fixture-pin-1:ch=0", L"fixture-pin-1:ch=1"};
  settings.device_buffers = {{L"fixture-usb", 128}, {L"fixture-realtek", 256}};
  return settings;
}

namespace {

HWND create_fixture_window(AppState& app, HINSTANCE instance, const wchar_t* title) {
  // Start with the same DPI-scaled, screen-bounded size as the real application.
  // A fixed 980x660 physical window is below the minimum at 200 percent scaling.
  app.theme.set_dpi(GetDpiForSystem());
  const auto work = monitor_work_area(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
  const auto initial = fit_window_to_work_area(
      {work.left + app.theme.px(24), work.top + app.theme.px(24),
       work.left + app.theme.px(1004), work.top + app.theme.px(684)}, work, app.theme.px(8));
  return CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, title,
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, initial.left, initial.top,
      initial.right - initial.left, initial.bottom - initial.top, nullptr, nullptr, instance, &app);
}

RECT control_rect(HWND parent, HWND child) {
  RECT rect{};
  GetWindowRect(child, &rect);
  MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
  return rect;
}

bool layout_is_valid(const AppState& app) {
  const auto list = control_rect(app.window, app.device_list);
  const auto rate = control_rect(app.window, app.sample_rate);
  const auto asio = control_rect(app.window, app.asio_buffer);
  const auto device = control_rect(app.window, app.device_buffer);
  const auto refresh = control_rect(app.window, app.refresh);
  const auto recommendation = control_rect(app.window, app.recommendation);
  const auto close = control_rect(app.window, app.close);
  const auto active = control_rect(app.window, app.active_device);
  const auto settings = control_rect(app.window, app.settings_button);
  RECT client{};
  GetClientRect(app.window, &client);
  return list.bottom > list.top && list.top >= app.device_card.top &&
      list.bottom < app.device_card.bottom && app.device_card.bottom < app.settings_card.top &&
      rate.top >= app.settings_card.top && rate.bottom < app.settings_card.bottom &&
      rate.right < asio.left && asio.right < device.left &&
      active.right <= app.settings_card.right && active.bottom < app.settings_card.bottom &&
      app.settings_card.bottom < recommendation.top && recommendation.bottom < refresh.top &&
      refresh.right < close.left && refresh.top == close.top && close.bottom < client.bottom &&
      settings.bottom < app.device_card.top && settings.right <= client.right;
}

bool item_drawing_is_clipped(AppState& app) {
  const auto list = app.device_list;
  RECT client{}, header{};
  GetClientRect(list, &client);
  GetWindowRect(ListView_GetHeader(list), &header);
  MapWindowPoints(nullptr, list, reinterpret_cast<POINT*>(&header), 2);
  const auto screen = GetDC(nullptr);
  const auto dc = CreateCompatibleDC(screen);
  const auto bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
  const auto old = SelectObject(dc, bitmap);
  const auto sentinel = RGB(251, 0, 211);
  const auto brush = CreateSolidBrush(sentinel);
  FillRect(dc, &client, brush);
  DeleteObject(brush);
  bool passed = true;
  bool offscreen = false;
  for (int row = 0; row < ListView_GetItemCount(list); ++row) {
    RECT bounds{};
    if (ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS) && bounds.top < header.bottom)
      offscreen = true;
    NMLVCUSTOMDRAW draw{};
    draw.nmcd.hdr.hwndFrom = list;
    draw.nmcd.hdc = dc;
    draw.nmcd.dwDrawStage = CDDS_ITEMPREPAINT;
    draw.nmcd.dwItemSpec = row;
    draw.dwItemType = LVCDI_ITEM;
    RECT before{}, after{};
    GetClipBox(dc, &before);
    config_theme::draw_list(app.theme, draw);
    GetClipBox(dc, &after);
    passed = passed && EqualRect(&before, &after);
  }
  for (int y = 0; y < std::min(header.bottom, client.bottom); ++y) {
    for (int x = 0; x < client.right; ++x) {
      if (GetPixel(dc, x, y) != sentinel) { passed = false; break; }
    }
    if (!passed) break;
  }
  SelectObject(dc, old);
  DeleteObject(bitmap);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return passed && offscreen;
}

LRESULT CALLBACK instance_test_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  if (message == config_instance::kActivateMessage) {
    ++*reinterpret_cast<int*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

bool single_instance_is_valid() {
  const auto suffix = std::to_wstring(GetCurrentProcessId());
  const auto mutex_name = L"Local\\AggregateAudio.Config.Instance.Test." + suffix;
  const auto class_name = L"AggregateAudio.Config.Instance.Test." + suffix;
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW klass{};
  klass.lpfnWndProc = instance_test_proc;
  klass.hInstance = instance;
  klass.lpszClassName = class_name.c_str();
  if (!RegisterClassW(&klass)) return false;
  bool passed = true;
  int activations = 0;
  {
    config_instance::Instance owner;
    passed = owner.claim_or_activate(mutex_name.c_str(), class_name.c_str()) == 1;
    std::array<wchar_t, 32768> executable{};
    GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    auto command = L"\"" + std::wstring(executable.data()) + L"\" --instance-client " +
        mutex_name + L" " + class_name;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    // The client starts before the first HWND exists; it must await publication.
    Sleep(150);
    const auto window = CreateWindowW(class_name.c_str(), L"Hidden instance test", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, instance, &activations);
    const auto deadline = GetTickCount64() + 5000;
    DWORD exit_code = STILL_ACTIVE;
    while (GetTickCount64() < deadline && (exit_code == STILL_ACTIVE || activations == 0)) {
      MsgWaitForMultipleObjects(1, &process.hProcess, FALSE, 50, QS_ALLINPUT);
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message != WM_QUIT) DispatchMessageW(&message);
      }
      GetExitCodeProcess(process.hProcess, &exit_code);
    }
    passed = passed && window && exit_code == 0 && activations == 1 && !IsWindowVisible(window);
    DestroyWindow(window);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  config_instance::Instance replacement;
  passed = passed && replacement.claim_or_activate(mutex_name.c_str(), class_name.c_str()) == 1;
  UnregisterClassW(class_name.c_str(), instance);
  return passed;
}

bool render_preview(AppState& app, const wchar_t* path) {
  RECT client{};
  GetClientRect(app.window, &client);
  const auto screen = GetDC(nullptr);
  const auto dc = CreateCompatibleDC(screen);
  const auto bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
  const auto previous = SelectObject(dc, bitmap);
  SendMessageW(app.window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
  // Render each child in client coordinates. DefWindowProc's WM_PRINT traversal
  // includes nonclient offsets for a hidden fixture window on some Windows builds.
  struct RenderContext { HWND parent; HDC dc; } render{app.window, dc};
  EnumChildWindows(app.window, [](HWND child, LPARAM data) -> BOOL {
    const auto& context = *reinterpret_cast<RenderContext*>(data);
    if (GetParent(child) != context.parent ||
        !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) return TRUE;
    const auto rect = control_rect(context.parent, child);
    const int saved = SaveDC(context.dc);
    SetViewportOrgEx(context.dc, rect.left, rect.top, nullptr);
    IntersectClipRect(context.dc, 0, 0, rect.right - rect.left, rect.bottom - rect.top);
    SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(context.dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN);
    RestoreDC(context.dc, saved);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&render));
  SelectObject(dc, previous);
  UINT count{}, size{};
  Gdiplus::GetImageEncodersSize(&count, &size);
  std::vector<BYTE> encoders(size);
  auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(encoders.data());
  Gdiplus::GetImageEncoders(count, size, codecs);
  bool saved = false;
  {
    Gdiplus::Bitmap output(bitmap, nullptr);
    for (UINT index = 0; index < count; ++index) {
      if (std::wcscmp(codecs[index].MimeType, L"image/png") == 0) {
        saved = output.Save(path, &codecs[index].Clsid) == Gdiplus::Ok;
        break;
      }
    }
  }
  DeleteObject(bitmap);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return saved;
}

bool scrollbar_drag(HWND control, int bar, bool to_end, bool cancel = false) {
  SCROLLBARINFO info{sizeof(info)};
  SCROLLINFO before{sizeof(before), SIF_ALL};
  if (!GetScrollBarInfo(control, bar == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &info) ||
      !GetScrollInfo(control, bar, &before) || info.xyThumbBottom <= info.xyThumbTop) return false;
  const int axis = (info.xyThumbTop + info.xyThumbBottom) / 2;
  POINT thumb = bar == SB_VERT ? POINT{(info.rcScrollBar.left + info.rcScrollBar.right) / 2,
      info.rcScrollBar.top + axis} : POINT{info.rcScrollBar.left + axis,
      (info.rcScrollBar.top + info.rcScrollBar.bottom) / 2};
  SendMessageW(control, WM_NCLBUTTONDOWN, bar == SB_VERT ? HTVSCROLL : HTHSCROLL,
      MAKELPARAM(thumb.x, thumb.y));
  bool captured = GetCapture() == control;
  if (bar == SB_VERT) thumb.y = to_end ? info.rcScrollBar.bottom + 40 : info.rcScrollBar.top - 40;
  else thumb.x = to_end ? info.rcScrollBar.right + 40 : info.rcScrollBar.left - 40;
  ScreenToClient(control, &thumb);
  SendMessageW(control, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(thumb.x, thumb.y));
  SendMessageW(control, cancel ? WM_CANCELMODE : WM_LBUTTONUP, 0, MAKELPARAM(thumb.x, thumb.y));
  SCROLLINFO after{sizeof(after), SIF_ALL};
  GetScrollInfo(control, bar, &after);
  const int maximum = after.nMax - static_cast<int>(std::max(1u, after.nPage)) + 1;
  // ListView rounds vertical movement to rows. Both extremes must remain within
  // one row, and pointer capture must be released on up/cancel.
  const bool moved = to_end ? after.nPos > before.nPos && maximum - after.nPos < 64 :
      after.nPos < before.nPos && after.nPos == after.nMin;
  if (!moved) std::printf("Scroll drag %d %d: before=%d after=%d max=%d\n", bar, to_end,
      before.nPos, after.nPos, maximum);
  return captured && moved && GetCapture() != control;
}

bool scrollbar_page_and_arrow(HWND control, int bar) {
  SCROLLBARINFO info{sizeof(info)};
  SCROLLINFO before{sizeof(before), SIF_POS};
  GetScrollInfo(control, bar, &before);
  if (!GetScrollBarInfo(control, bar == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &info)) return false;
  POINT point = bar == SB_VERT ? POINT{(info.rcScrollBar.left + info.rcScrollBar.right) / 2,
      info.rcScrollBar.top + (info.xyThumbBottom + info.rcScrollBar.bottom - info.rcScrollBar.top - info.dxyLineButton) / 2} :
      POINT{info.rcScrollBar.left + (info.xyThumbBottom + info.rcScrollBar.right - info.rcScrollBar.left - info.dxyLineButton) / 2,
      (info.rcScrollBar.top + info.rcScrollBar.bottom) / 2};
  SendMessageW(control, WM_NCLBUTTONDOWN, bar == SB_VERT ? HTVSCROLL : HTHSCROLL,
      MAKELPARAM(point.x, point.y));
  SendMessageW(control, WM_LBUTTONUP, 0, 0);
  SCROLLINFO page{sizeof(page), SIF_POS};
  GetScrollInfo(control, bar, &page);
  GetScrollBarInfo(control, bar == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &info);
  if (bar == SB_VERT) point.y = info.rcScrollBar.top + info.dxyLineButton / 2;
  else point.x = info.rcScrollBar.left + info.dxyLineButton / 2;
  SendMessageW(control, WM_NCLBUTTONDOWN, bar == SB_VERT ? HTVSCROLL : HTHSCROLL,
      MAKELPARAM(point.x, point.y));
  SendMessageW(control, WM_LBUTTONUP, 0, 0);
  SCROLLINFO arrow{sizeof(arrow), SIF_POS};
  GetScrollInfo(control, bar, &arrow);
  return page.nPos > before.nPos && arrow.nPos < page.nPos && GetCapture() != control;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wcscmp(argv[1], L"--instance-client") == 0) {
    config_instance::Instance client;
    return client.claim_or_activate(argv[2], argv[3], config_instance::kActivateMessage, false) == 0 ? 0 : 1;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token{};
  if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 1;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_LINK_CLASS};
  InitCommonControlsEx(&controls);
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW klass{};
  klass.lpfnWndProc = window_proc;
  klass.hInstance = instance;
  klass.lpszClassName = kWindowClass;
  RegisterClassW(&klass);

  bool passed = true;
  {
    AppState app;
    app.tray_enabled = false; // Hidden layout fixtures never modify the real system tray.
    const auto window = create_fixture_window(app, instance, L"UI regression fixture");
    if (!window) return 1;
    const auto check = [&](bool condition, const char* label) {
      std::printf("%s: %s\n", label, condition ? "PASS" : "FAIL");
      passed = passed && condition;
    };
    check(!app.showing_settings &&
        !(GetWindowLongPtrW(app.status_log, GWL_STYLE) & WS_VISIBLE),
        "First open hides the full-page log before any page switch");
    check(layout_is_valid(app), "First open lays out controls before any resize or page switch");
    if (argc > 1) {
      const auto path = std::wstring(argv[1]) + L".first-open.png";
      check(render_preview(app, path.c_str()), "First-open native control preview");
    }
    check(app.devices.size() == 2 && app.channels.size() == 6 &&
        checked_channel_indices(app).size() == 4, "Grouped channels and saved checkboxes");
    check(app.app_icon != nullptr, "Window uses the embedded five-bar brand icon");
    app.tray_enabled = true;
    app.asio_active = true;
    close_or_hide(app);
    check(IsWindow(window) && app.tray_background && !IsWindowVisible(window), "Closing an active ASIO panel preserves its existing window");
    app.tray_enabled = false;
    app.tray_background = false;
    check(single_instance_is_valid(), "Concurrent launch activates one hidden window and releases ownership on exit");
    for (const UINT dpi : {96u, 144u, 192u}) {
      RECT suggested{0, 0, MulDiv(660, dpi, 96), MulDiv(400, dpi, 96)};
      SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
      check(layout_is_valid(app), "Minimum window layout at 100/150/200 percent");
      check(checked_channel_indices(app).size() == 4, "DPI change preserves checkboxes");
      int row_width{}, row_height{};
      check(ImageList_GetIconSize(app.theme.rows, &row_width, &row_height) &&
          row_height == app.theme.px(26), "DPI-scaled list rows");
      const RECT work{0, 0, 1920, 1040};
      const auto bounded = fit_window_to_work_area({50, 50, MulDiv(1030, dpi, 96), MulDiv(710, dpi, 96)},
          work, app.theme.px(8));
      check(bounded.right <= work.right && bounded.bottom <= work.bottom,
          "DPI-scaled window fits a 1080p monitor work area");
      SetWindowPos(window, nullptr, 0, 0, bounded.right - bounded.left, bounded.bottom - bounded.top,
          SWP_NOZORDER | SWP_NOACTIVATE);
      check(layout_is_valid(app), "Screen-bounded layout retains accessible controls");
      SendMessageW(window, WM_COMMAND, kSettings, 0);
      const auto log = control_rect(window, app.status_log);
      check(log.top >= app.log_card.top &&
          log.bottom < app.log_card.bottom && log.bottom > log.top && app.log_card.bottom < app.footer.top,
          "Settings controls and log fit at 100/150/200 percent");
      SendMessageW(window, WM_COMMAND, kSettings, 0);
      if (argc > 1 && dpi == 192) {
        const auto path = std::wstring(argv[1]) + L".200pct.png";
        check(render_preview(app, path.c_str()), "200 percent screen-bounded preview");
      }
    }
    RECT normal{0, 0, 980, 660};
    SendMessageW(window, WM_DPICHANGED, MAKELONG(96, 96), reinterpret_cast<LPARAM>(&normal));
    check(layout_is_valid(app), "Default window layout");
    if (argc > 1) check(render_preview(app, argv[1]), "Native controls render to PNG");
    const auto selection_before = selected_channel_keys(app, Direction::input);
    SendMessageW(window, WM_COMMAND, kSettings, 0);
    append_log(app, L"Settings 日志页回归测试");
    std::wstring log_text(static_cast<size_t>(GetWindowTextLengthW(app.status_log)) + 1, L'\0');
    GetWindowTextW(app.status_log, log_text.data(), static_cast<int>(log_text.size()));
    check(app.showing_settings && (GetWindowLongPtrW(app.status_log, GWL_STYLE) & WS_VISIBLE) &&
        !(GetWindowLongPtrW(app.device_list, GWL_STYLE) & WS_VISIBLE), "Settings contains the session log");
    if (argc > 1) {
      const auto path = std::wstring(argv[1]) + L".settings.png";
      check(render_preview(app, path.c_str()), "Settings preview");
    }
    SendMessageW(window, WM_COMMAND, kSettings, 0);
    SendMessageW(window, WM_COMMAND, kSettings, 0);
    std::wstring reopened(static_cast<size_t>(GetWindowTextLengthW(app.status_log)) + 1, L'\0');
    GetWindowTextW(app.status_log, reopened.data(), static_cast<int>(reopened.size()));
    check(log_text == reopened && selection_before == selected_channel_keys(app, Direction::input),
          "Page changes preserve log and unsaved selections");
    SendMessageW(window, WM_COMMAND, kSettings, 0);

    SendMessageW(window, WM_COMMAND, kSettings, 0);
    SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    check(!app.showing_settings && IsWindow(window), "Escape returns from Settings without closing the panel");
    ListView_SetItemState(app.device_list, 0, 0, LVIS_SELECTED);
    ListView_SetItemState(app.device_list, 4, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    check(app.active_device_index == 1 && combo_value(app.device_buffer, 0) == 256,
          "Selecting a channel updates its device buffer");
    select_combo_value(app.device_buffer, 512);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(kDeviceBuffer, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(app.device_buffer));
    check(app.devices[1].buffer_frames == 512 && app.devices[0].buffer_frames == 128,
          "Device buffer edits remain independent");
    ListView_SetCheckState(app.device_list, 4, TRUE);
    check(selected_channel_keys(app, Direction::output).size() == 3, "Checkbox selection still routes channels");
    set_probe_controls_enabled(app, false);
    check(!IsWindowEnabled(app.sample_rate) && !IsWindowEnabled(app.save) && IsWindowEnabled(app.close),
          "Probe disables settings while close stays available");
    set_probe_controls_enabled(app, true);
    check(IsWindowEnabled(app.sample_rate) && IsWindowEnabled(app.save), "Probe restores controls");
    SendMessageW(app.save, BM_SETSTYLE, BS_DEFPUSHBUTTON, FALSE);
    check((GetWindowLongPtrW(app.save, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW,
          "Default-button navigation preserves owner drawing");
    refresh_devices(app);
    check(checked_channel_indices(app).size() == 5 && app.devices[1].buffer_frames == 512,
          "Device refresh preserves unsaved channel and buffer edits");
    check(app.active_device_index == 1,
          "Device refresh preserves the current device instead of selecting the first row");
    app.endpoints[0].channels = 24;
    app.populating = true;
    populate_device_list(app, app.saved_settings);
    app.populating = false;
    for (const UINT dpi : {96u, 144u, 192u}) {
      RECT suggested{0, 0, MulDiv(980, dpi, 96), MulDiv(660, dpi, 96)};
      SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
      SendMessageW(app.device_list, WM_VSCROLL, SB_TOP, 0);
      const auto checked_before = checked_channel_indices(app);
      const auto selected_before = ListView_GetNextItem(app.device_list, -1, LVNI_SELECTED);
      check(scrollbar_drag(app.device_list, SB_VERT, true) &&
          scrollbar_drag(app.device_list, SB_VERT, false, true),
          "Themed vertical thumb reaches both ends and releases capture at all DPIs");
      check(scrollbar_page_and_arrow(app.device_list, SB_VERT), "Themed list track and arrow navigate natively");
      SendMessageW(app.device_list, WM_VSCROLL, SB_TOP, 0);
      SendMessageW(app.device_list, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
      check(GetScrollPos(app.device_list, SB_VERT) > 0, "List wheel navigation remains native");
      SendMessageW(app.device_list, WM_VSCROLL, SB_TOP, 0);
      ListView_SetColumnWidth(app.device_list, 0, app.theme.px(1800));
      check(scrollbar_drag(app.device_list, SB_HORZ, true) && scrollbar_drag(app.device_list, SB_HORZ, false),
          "Themed horizontal thumb reaches both ends at all DPIs");
      check(scrollbar_page_and_arrow(app.device_list, SB_HORZ), "Themed horizontal track and arrow navigate natively");
      SendMessageW(app.device_list, WM_HSCROLL, SB_LEFT, 0);
      if (argc > 1) {
        const auto path = std::wstring(argv[1]) + L".scrollbars." + std::to_wstring(dpi) + L"dpi.png";
        check(render_preview(app, path.c_str()), "Both themed list scrollbars preview");
      }
      resize_columns(app.device_list, app.theme);
      check(checked_before == checked_channel_indices(app) &&
          selected_before == ListView_GetNextItem(app.device_list, -1, LVNI_SELECTED),
          "Scrollbar navigation preserves channel selection and routing");
      SendMessageW(window, WM_COMMAND, kSettings, 0);
      std::wstring lines;
      const auto log_bounds = control_rect(window, app.status_log);
      const auto link_bounds = control_rect(window, app.repository_link);
      const auto credit_bounds = control_rect(window, app.attribution);
      LITEM repository{};
      repository.mask = LIF_ITEMINDEX | LIF_URL;
      repository.iLink = 0;
      check(app.repository_link && app.attribution &&
          (GetWindowLongPtrW(app.repository_link, GWL_STYLE) & WS_VISIBLE) &&
          (GetWindowLongPtrW(app.attribution, GWL_STYLE) & WS_VISIBLE) &&
          SendMessageW(app.repository_link, LM_GETITEM, 0, reinterpret_cast<LPARAM>(&repository)) &&
          std::wcscmp(repository.szUrl, L"https://github.com/wanan522z/AggregateAudio-asio") == 0 &&
          log_bounds.bottom <= link_bounds.top && link_bounds.bottom <= credit_bounds.top &&
          credit_bounds.bottom <= app.log_card.bottom,
          "Settings repository and attribution fit below the log at all DPIs");
      for (int line = 0; line < 300; ++line) lines += L"运行日志 scrollbar fixture " + std::to_wstring(line) + L"\r\n";
      SetWindowTextW(app.status_log, lines.c_str());
      SendMessageW(app.status_log, EM_SETSEL, 0, 0);
      SendMessageW(app.status_log, WM_VSCROLL, SB_TOP, 0);
      check(scrollbar_drag(app.status_log, SB_VERT, true) && scrollbar_drag(app.status_log, SB_VERT, false),
          "Themed log thumb scrolls the read-only text at all DPIs");
      check(scrollbar_page_and_arrow(app.status_log, SB_VERT), "Themed log track and arrow navigate natively");
      DWORD first{}, last{};
      SendMessageW(app.status_log, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
      check(GetWindowTextLengthW(app.status_log) == static_cast<int>(lines.size()) && first == 0 && last == 0,
          "Log scrollbar leaves text and caret selection intact");
      SendMessageW(app.status_log, WM_VSCROLL, SB_TOP, 0);
      SendMessageW(app.status_log, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
      check(GetScrollPos(app.status_log, SB_VERT) > 0, "Log wheel navigation remains native");
      if (argc > 1 && dpi == 144) {
        const auto path = std::wstring(argv[1]) + L".log-scrollbar.png";
        check(render_preview(app, path.c_str()), "Themed log scrollbar preview");
      }
      SendMessageW(window, WM_COMMAND, kSettings, 0);
      check(!(GetWindowLongPtrW(app.repository_link, GWL_STYLE) & WS_VISIBLE) &&
          !(GetWindowLongPtrW(app.attribution, GWL_STYLE) & WS_VISIBLE),
          "Device page hides Settings repository and attribution");
    }
    SendMessageW(window, WM_DPICHANGED, MAKELONG(96, 96), reinterpret_cast<LPARAM>(&normal));
    ListView_EnsureVisible(app.device_list, 27, FALSE);
    ListView_Scroll(app.device_list, 0, app.theme.px(13));
    check(item_drawing_is_clipped(app), "Scrolled channel rows cannot paint over column headers and preserve DC clip");
    if (argc > 1) {
      const auto path = std::wstring(argv[1]) + L".scrolled.png";
      check(render_preview(app, path.c_str()), "Scrolled list preview");
    }
    RECT scaled{0, 0, 1888, 1008};
    SendMessageW(window, WM_DPICHANGED, MAKELONG(192, 192), reinterpret_cast<LPARAM>(&scaled));
    ListView_EnsureVisible(app.device_list, 27, FALSE);
    ListView_Scroll(app.device_list, 0, app.theme.px(13));
    check(item_drawing_is_clipped(app), "200 percent scrolled channel clipping");
    if (argc > 1) {
      const auto path = std::wstring(argv[1]) + L".200pct-scrolled.png";
      check(render_preview(app, path.c_str()), "200 percent scrolled list preview");
    }
    ui_fixture_capture_channels = 48;
    refresh_devices(app);
    ListView_SetItemState(app.device_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(app.device_list, 31, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    LVGROUP collapsed{};
    collapsed.cbSize = sizeof(collapsed);
    collapsed.mask = LVGF_STATE;
    collapsed.stateMask = LVGS_COLLAPSED;
    collapsed.state = LVGS_COLLAPSED;
    ListView_SetGroupInfo(app.device_list, app.devices[1].list_group_id, &collapsed);
    ListView_EnsureVisible(app.device_list, 30, FALSE);
    ListView_Scroll(app.device_list, 0, app.theme.px(13));
    RECT before{}, after{};
    ListView_GetItemRect(app.device_list, 30, &before, LVIR_BOUNDS);
    bool preserved = true;
    for (int refresh = 0; refresh < 3; ++refresh) {
      SendMessageW(window, config_tray::kDeviceRefreshMessage, 0, 0);
      preserved = preserved && ListView_GetItemRect(app.device_list, 30, &after, LVIR_BOUNDS) &&
          before.top == after.top && before.bottom == after.bottom &&
          ListView_GetNextItem(app.device_list, -1, LVNI_SELECTED) == 31 &&
          (ListView_GetGroupState(app.device_list, app.devices[1].list_group_id,
              LVGS_COLLAPSED) & LVGS_COLLAPSED);
      if (before.top != after.top) std::printf("Refresh scroll mismatch: before=%ld after=%ld\n", before.top, after.top);
    }
    check(preserved, "Repeated background refresh preserves pixel scroll, selected channel and collapsed groups");
    ui_fixture_reverse_catalog = true;
    SendMessageW(window, config_tray::kDeviceRefreshMessage, 0, 0);
    check(ListView_GetNextItem(app.device_list, -1, LVNI_SELECTED) == 35 &&
        ListView_GetItemRect(app.device_list, 34, &after, LVIR_BOUNDS) &&
        std::abs(before.top - after.top) <= (before.bottom - before.top) / 2 &&
        app.devices[app.active_device_index].id == L"fixture-usb" &&
        (ListView_GetGroupState(app.device_list, app.devices[0].list_group_id,
            LVGS_COLLAPSED) & LVGS_COLLAPSED),
        "Reordered catalog retains view and current device using stable IDs");
    ui_fixture_omit_capture = true;
    SendMessageW(window, config_tray::kDeviceRefreshMessage, 0, 0);
    check(app.channels.size() == 4 && app.devices[app.active_device_index].id == L"fixture-usb",
        "Removed top channel falls back safely to its surviving device");
    SendMessageW(window, WM_COMMAND, kSettings, 0);
    SendMessageW(window, config_tray::kDeviceRefreshMessage, 0, 0);
    check(!(GetWindowLongPtrW(app.device_list, GWL_STYLE) & WS_VISIBLE) &&
        (GetWindowLongPtrW(app.status_log, GWL_STYLE) & WS_VISIBLE),
        "Background refresh on Settings does not expose the device list");
    SendMessageW(window, WM_COMMAND, kSettings, 0);
    ui_fixture_capture_channels = 2;
    ui_fixture_reverse_catalog = ui_fixture_omit_capture = false;
    app.endpoints.clear();
    app.populating = true;
    populate_device_list(app, {});
    app.populating = false;
    check(app.channels.empty() && !(GetWindowLongPtrW(app.device_list, GWL_STYLE) & WS_VISIBLE),
          "Empty catalog shows the empty state");
    DestroyWindow(window);
  }
  ui_fixture_first_profile = true;
  for (const UINT dpi : {96u, 144u, 192u}) {
    AppState app;
    app.tray_enabled = false;
    const auto window = create_fixture_window(app, instance, L"First-profile UI fixture");
    if (!window) return 1;
    const auto check = [&](bool condition, const char* label) {
      std::printf("%s (%u DPI): %s\n", label, dpi, condition ? "PASS" : "FAIL");
      passed = passed && condition;
    };
    check(!app.saved_settings.initialized && !app.showing_settings &&
        !(GetWindowLongPtrW(app.status_log, GWL_STYLE) & WS_VISIBLE) && layout_is_valid(app),
        "New profile starts with a complete device page and no log overlay");
    RECT suggested{0, 0, MulDiv(980, dpi, 96), MulDiv(660, dpi, 96)};
    SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
    check(layout_is_valid(app) && !(GetWindowLongPtrW(app.status_log, GWL_STYLE) & WS_VISIBLE),
        "First page remains correctly laid out after initial DPI adjustment");
    if (argc > 1) {
      const auto path = std::wstring(argv[1]) + L".new-profile." + std::to_wstring(dpi) + L"dpi.png";
      check(render_preview(app, path.c_str()), "First-profile native preview");
    }
    SendMessageW(window, WM_COMMAND, kSettings, 0);
    std::wstring log_text(static_cast<size_t>(GetWindowTextLengthW(app.status_log)) + 1, L'\0');
    GetWindowTextW(app.status_log, log_text.data(), static_cast<int>(log_text.size()));
    check((GetWindowLongPtrW(app.status_log, GWL_STYLE) & WS_VISIBLE) &&
        log_text.find(L"首次配置") != std::wstring::npos,
        "Initially hidden log retains startup messages on Settings");
    DestroyWindow(window);
  }
  Gdiplus::GdiplusShutdown(token);
  return passed ? 0 : 1;
}
