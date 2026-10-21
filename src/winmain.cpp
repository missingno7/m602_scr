// Windows screen saver shell: command line (/s /p /c), one saver thread and
// window per monitor, input, and the settings dialog.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <thread>
#include <vector>

#include "renderer.h"
#include "resource.h"
#include "saver.h"
#include "winmachine.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_APP_DONE = WM_APP + 2;   // saver thread left its loop
const wchar_t kClassName[] = L"M602SaverWindow";
const wchar_t kRegKey[] = L"Software\\M602Saver";

// ---------------------------------------------------------------- settings

struct Settings {
    RenderSettings render;
    bool fixed80 = false;  // 80x25 with side bars instead of a wider text buffer
    int speed = 100;       // percent of the original timer rate
};

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value, size = sizeof(value), type;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        return value;
    return fallback;
}

Settings LoadSettings() {
    Settings s;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        s.render.scanlines = int(ReadDword(key, L"Scanlines", 0)) % 3;
        s.render.color = ReadDword(key, L"Color", kOriginalTextColor) & 0xFFFFFF;
        s.speed = std::clamp(int(ReadDword(key, L"Speed", 100)), 10, 400);
        s.fixed80 = ReadDword(key, L"Fixed80x25", 0) != 0;
        RegCloseKey(key);
    }
    return s;
}

void SaveSettings(const Settings& s) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    auto put = [&](const wchar_t* name, DWORD v) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    };
    put(L"Scanlines", DWORD(s.render.scanlines));
    put(L"Color", s.render.color);
    put(L"Speed", DWORD(s.speed));
    put(L"Fixed80x25", s.fixed80);
    RegCloseKey(key);
}

// ------------------------------------------------------------ windows

struct Instance {
    HWND hwnd = nullptr;
    std::unique_ptr<WinMachine> machine;
    Renderer renderer;
    std::thread thread;
    std::vector<uint16_t> cells;
};

Settings g_settings;
bool g_preview = false;
POINT g_startCursor;
std::vector<std::unique_ptr<Instance>> g_instances;
bool g_shuttingDown = false;

void ConfigureRenderer(Instance& in) {
    RECT rc;
    GetClientRect(in.hwnd, &rc);
    in.renderer.Configure(rc.right, rc.bottom, in.machine->screen.ScreenWidth, in.machine->screen.ScreenHeight,
                          g_settings.render);
    InvalidateRect(in.hwnd, nullptr, FALSE);
}

void StartInstance(Instance& in) {
    RECT rc;
    GetClientRect(in.hwnd, &rc);
    in.machine = std::make_unique<WinMachine>(in.hwnd, g_settings.speed);
    TextScreen& s = in.machine->screen;
    // The wider text buffer is the display adaptation: the saver's own
    // boundary logic (MaxX = ScreenWidth - 11) uses whatever width it gets.
    s.ScreenWidth = uint8_t(g_settings.fixed80 ? 80 : TextColumnsForDisplay(rc.right, rc.bottom));
    s.ScreenHeight = 25;
    in.cells.resize(size_t(s.ScreenWidth) * s.ScreenHeight);
    ConfigureRenderer(in);
    Instance* p = &in;
    in.thread = std::thread([p] {
        ScreenSaver(*p->machine);
        PostMessageW(p->hwnd, WM_APP_DONE, 0, 0);
    });
}

void StopInstance(Instance& in) {
    if (in.machine) in.machine->quit = true;
    if (in.thread.joinable()) in.thread.join();
}

// Input on any screen ends every saver at its next GetEvent.
void Wake() {
    if (g_preview) return;
    for (auto& in : g_instances) ++in->machine->pendingEvents;
}

void ShutdownAll() {
    if (g_shuttingDown) return;
    g_shuttingDown = true;
    for (auto& in : g_instances) StopInstance(*in);
    for (auto& in : g_instances) DestroyWindow(in->hwnd);
    PostQuitMessage(0);
}

void OnFrame(Instance& in) {
    in.machine->framePosted = false;
    {
        std::lock_guard<std::mutex> g(in.machine->screen.lock);
        std::copy_n(in.machine->screen.mem, in.cells.size(), in.cells.begin());
    }
    for (const PixelRect& r : in.renderer.Update(in.cells.data())) {
        RECT rc = {r.left, r.top, r.right, r.bottom};
        InvalidateRect(in.hwnd, &rc, FALSE);
    }
}

void Present(Instance& in, HDC dc) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = in.renderer.Width();
    bmi.bmiHeader.biHeight = -in.renderer.Height();  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, in.renderer.Width(), in.renderer.Height(), 0, 0, 0, in.renderer.Height(),
                      in.renderer.Pixels(), &bmi, DIB_RGB_COLORS);
}

void OnPaint(Instance& in) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(in.hwnd, &ps);
    Present(in, dc);
    EndPaint(in.hwnd, &ps);
}

LRESULT CALLBACK SaverWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* in = reinterpret_cast<Instance*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_APP_FRAME:
        if (in && in->machine) OnFrame(*in);
        return 0;
    case WM_APP_DONE:
        if (!g_preview) ShutdownAll();
        return 0;
    case WM_PAINT:
        if (in && in->machine) {
            OnPaint(*in);
            return 0;
        }
        break;
    case WM_PRINTCLIENT:  // screenshots / PrintWindow
        if (in && in->machine) {
            Present(*in, reinterpret_cast<HDC>(wp));
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (in && in->machine) ConfigureRenderer(*in);
        return 0;
    case WM_SETCURSOR:
        if (!g_preview) {
            SetCursor(nullptr);  // HideMouse
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (!g_preview) {
            POINT pt;
            GetCursorPos(&pt);
            if (std::abs(pt.x - g_startCursor.x) > 4 || std::abs(pt.y - g_startCursor.y) > 4) Wake();
        }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        Wake();
        return 0;
    case WM_ACTIVATEAPP:
        if (!wp) Wake();
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_SCREENSAVE) return 0;
        break;
    case WM_CLOSE:
        if (!g_preview) {
            ShutdownAll();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (g_preview && in) {
            StopInstance(*in);
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterSaverClass(HINSTANCE hinst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = SaverWndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

int MessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}

BOOL CALLBACK AddMonitor(HMONITOR, HDC, LPRECT rc, LPARAM lp) {
    reinterpret_cast<std::vector<RECT>*>(lp)->push_back(*rc);
    return TRUE;
}

int RunFullScreen(HINSTANCE hinst) {
    RegisterSaverClass(hinst);
    GetCursorPos(&g_startCursor);
    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, LPARAM(&monitors));
    if (monitors.empty())
        monitors.push_back({0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)});
    for (const RECT& rc : monitors) {
        auto in = std::make_unique<Instance>();
        in->hwnd = CreateWindowExW(WS_EX_TOPMOST, kClassName, L"M602 Screensaver", WS_POPUP, rc.left, rc.top,
                                   rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, in.get());
        if (!in->hwnd) continue;
        g_instances.push_back(std::move(in));
    }
    for (auto& in : g_instances) {
        StartInstance(*in);
        ShowWindow(in->hwnd, SW_SHOW);
    }
    if (g_instances.empty()) return 1;
    SetForegroundWindow(g_instances.front()->hwnd);
    return MessageLoop();
}

int RunPreview(HINSTANCE hinst, HWND parent) {
    g_preview = true;
    RegisterSaverClass(hinst);
    RECT rc;
    GetClientRect(parent, &rc);
    auto in = std::make_unique<Instance>();
    in->hwnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, rc.right, rc.bottom, parent,
                               nullptr, hinst, in.get());
    if (!in->hwnd) return 1;
    StartInstance(*in);
    g_instances.push_back(std::move(in));
    return MessageLoop();
}

// ----------------------------------------------------------- settings dialog

uint32_t g_dlgColor;  // colour chosen in the open dialog, 0xRRGGBB

void ShowSpeed(HWND dlg, int speed) {
    wchar_t text[32];
    wsprintfW(text, L"Speed: %d %%", speed);
    SetDlgItemTextW(dlg, IDC_SPEED_LABEL, text);
}

// Double-clicking the speed slider's thumb resets it to 100 %.
LRESULT CALLBACK SpeedSliderProc(HWND slider, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    static DWORD lastTime;
    static POINT lastPt;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT thumb;
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, LPARAM(&thumb));
        if (PtInRect(&thumb, pt)) {
            DWORD now = DWORD(GetMessageTime());
            bool second = msg == WM_LBUTTONDBLCLK ||
                          (lastTime && now - lastTime <= GetDoubleClickTime() &&
                           std::abs(pt.x - lastPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                           std::abs(pt.y - lastPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2);
            if (second) {
                lastTime = 0;
                SendMessageW(slider, TBM_SETPOS, TRUE, 100);
                ShowSpeed(GetParent(slider), 100);
                return 0;
            }
            lastTime = now;
            lastPt = pt;
        } else {
            lastTime = 0;
        }
    }
    return DefSubclassProc(slider, msg, wp, lp);
}

void ShowDialogSettings(HWND dlg, const Settings& s) {
    SendDlgItemMessageW(dlg, IDC_SPEED, TBM_SETPOS, TRUE, s.speed);
    ShowSpeed(dlg, s.speed);
    g_dlgColor = s.render.color;
    InvalidateRect(GetDlgItem(dlg, IDC_COLOR), nullptr, TRUE);
    SendDlgItemMessageW(dlg, IDC_SCANLINES, CB_SETCURSEL, s.render.scanlines, 0);
    SendDlgItemMessageW(dlg, IDC_TEXTAREA, CB_SETCURSEL, s.fixed80 ? 1 : 0, 0);
}

COLORREF ToColorRef(uint32_t rgb) { return RGB(rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF); }

void PickColor(HWND dlg) {
    static COLORREF custom[16] = {ToColorRef(kOriginalTextColor)};
    CHOOSECOLORW cc = {sizeof(cc)};
    cc.hwndOwner = dlg;
    cc.rgbResult = ToColorRef(g_dlgColor);
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&cc)) {
        g_dlgColor = uint32_t(GetRValue(cc.rgbResult)) << 16 | GetGValue(cc.rgbResult) << 8 | GetBValue(cc.rgbResult);
        InvalidateRect(GetDlgItem(dlg, IDC_COLOR), nullptr, TRUE);
    }
}

INT_PTR CALLBACK ConfigDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        auto add = [&](int id, const wchar_t* item) { SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, LPARAM(item)); };
        add(IDC_SCANLINES, L"Off");
        add(IDC_SCANLINES, L"Subtle");
        add(IDC_SCANLINES, L"Strong");
        add(IDC_TEXTAREA, L"Widescreen");
        add(IDC_TEXTAREA, L"80\x00D7" L"25");
        SendDlgItemMessageW(dlg, IDC_SPEED, TBM_SETRANGE, TRUE, MAKELPARAM(10, 400));
        SendDlgItemMessageW(dlg, IDC_SPEED, TBM_SETTICFREQ, 50, 0);
        SendDlgItemMessageW(dlg, IDC_SPEED, TBM_SETPAGESIZE, 0, 10);
        SetWindowSubclass(GetDlgItem(dlg, IDC_SPEED), SpeedSliderProc, 0, 0);
        ShowDialogSettings(dlg, LoadSettings());
        return TRUE;
    }
    case WM_HSCROLL:
        ShowSpeed(dlg, int(SendDlgItemMessageW(dlg, IDC_SPEED, TBM_GETPOS, 0, 0)));
        return TRUE;
    case WM_DRAWITEM: {  // colour swatch button
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (di->CtlID != IDC_COLOR) break;
        RECT rc = di->rcItem;
        DrawEdge(di->hDC, &rc, (di->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT | BF_ADJUST);
        HBRUSH brush = CreateSolidBrush(ToColorRef(g_dlgColor));
        FillRect(di->hDC, &rc, brush);
        DeleteObject(brush);
        if (di->itemState & ODS_FOCUS) {
            InflateRect(&rc, -2, -2);
            DrawFocusRect(di->hDC, &rc);
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_COLOR:
            PickColor(dlg);
            return TRUE;
        case IDC_COLOR_ORIGINAL:
            g_dlgColor = kOriginalTextColor;
            InvalidateRect(GetDlgItem(dlg, IDC_COLOR), nullptr, TRUE);
            return TRUE;
        case IDC_DEFAULTS:
            ShowDialogSettings(dlg, Settings{});
            return TRUE;
        case IDOK: {
            Settings s;
            s.speed = int(SendDlgItemMessageW(dlg, IDC_SPEED, TBM_GETPOS, 0, 0));
            s.render.color = g_dlgColor;
            s.render.scanlines = int(SendDlgItemMessageW(dlg, IDC_SCANLINES, CB_GETCURSEL, 0, 0));
            s.fixed80 = SendDlgItemMessageW(dlg, IDC_TEXTAREA, CB_GETCURSEL, 0, 0) == 1;
            SaveSettings(s);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ------------------------------------------------------------ command line

HWND ParseHwnd(const wchar_t* s) {
    while (*s == L' ' || *s == L':') ++s;
    return reinterpret_cast<HWND>(uintptr_t(wcstoull(s, nullptr, 10)));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_settings = LoadSettings();

    const wchar_t* p = cmdLine;
    while (*p == L' ' || *p == L'"') ++p;
    wchar_t mode = L'c';
    if (*p == L'/' || *p == L'-') {
        mode = wchar_t(towlower(p[1]));
        p += 2;
    }
    switch (mode) {
    case L's':
        return RunFullScreen(hinst);
    case L'p': {
        HWND parent = ParseHwnd(p);
        return IsWindow(parent) ? RunPreview(hinst, parent) : 1;
    }
    case L'c': {
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);
        HWND owner = *p ? ParseHwnd(p) : nullptr;
        DialogBoxParamW(hinst, MAKEINTRESOURCEW(IDD_CONFIG), IsWindow(owner) ? owner : GetForegroundWindow(),
                        ConfigDlgProc, 0);
        return 0;
    }
    default:  // /a (password change) and anything else: nothing to do
        return 0;
    }
}
