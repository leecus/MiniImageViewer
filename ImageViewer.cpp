// ImageViewer.cpp — 原生 Win32 看图器（零依赖，仅系统 DLL）
// 编译: g++ -O2 -std=c++17 -municode -mwindows -s -static -o ImageViewer.exe ImageViewer.cpp -lgdiplus -lgdi32 -luser32 -lshell32 -lcomdlg32 -lole32
#define UNICODE
#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <objbase.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cwchar>

using namespace Gdiplus;

static const double ZOOM_MIN = 0.01, ZOOM_MAX = 64.0;

static Bitmap*  g_img    = nullptr;
static IStream* g_stream = nullptr;   // 保持流存活（GIF 等格式需要）
static std::wstring g_file;
static std::vector<std::wstring> g_list; // 同目录图片（全路径）
static int  g_idx  = -1;
static double g_scale = 1.0, g_target = 1.0;
static double g_offx  = 0.0, g_offy   = 0.0;   // 图片左上角在客户区坐标
static double g_ax = 0, g_ay = 0;              // 缩放锚点（客户区）
static double g_aix = 0, g_aiy = 0;            // 锚点对应的图片坐标
static bool   g_fit  = true;
static bool   g_drag = false;
static POINT  g_lastPt = {0, 0};
static int    g_srcW = 0, g_srcH = 0;
static std::wstring g_err;                    // 加载失败提示
static std::wstring g_notice;                 // 状态栏临时通知（如"已关联"）
static ULONG_PTR g_token = 0;

static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }
static double ClampScale(double s) { return dmax(ZOOM_MIN, dmin(ZOOM_MAX, s)); }

static void ClientSize(HWND hwnd, int& cw, int& ch) {
    RECT rc; GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left; ch = rc.bottom - rc.top;
}

static void ClampOffsets(HWND hwnd) {
    int cw, ch; ClientSize(hwnd, cw, ch);
    double dw = g_srcW * g_scale, dh = g_srcH * g_scale;
    // 任意缩放下都可自由拖动：只要保证图片至少 48px（或其自身尺寸，取小者）留在窗口内
    const double vis = 48.0;
    double vx = dmin(dw, vis), vy = dmin(dh, vis);
    if (g_offx < vx - dw)  g_offx = vx - dw;
    if (g_offx > cw - vx)  g_offx = cw - vx;
    if (g_offy < vy - dh)  g_offy = vy - dh;
    if (g_offy > ch - vy)  g_offy = ch - vy;
}

static double FitScale(HWND hwnd) {
    int cw, ch; ClientSize(hwnd, cw, ch);
    if (g_srcW < 1 || cw < 2) return 1.0;
    return dmin((cw - 16.0) / g_srcW, (ch - 16.0) / g_srcH);
}

static void FitToWindow(HWND hwnd) {
    if (!g_img) return;
    double s = FitScale(hwnd);
    g_scale = g_target = s;
    g_fit = true;
    int cw, ch; ClientSize(hwnd, cw, ch);
    g_offx = (cw - g_srcW * s) / 2.0;
    g_offy = (ch - g_srcH * s) / 2.0;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void StartZoom(HWND hwnd, double target, double ax, double ay) {
    target = ClampScale(target);
    g_ax = ax; g_ay = ay;
    g_aix = (ax - g_offx) / g_scale;
    g_aiy = (ay - g_offy) / g_scale;
    g_target = target;
    g_fit = false;
    SetTimer(hwnd, 1, 16, NULL);
}

static void StopZoom(HWND hwnd) { g_target = g_scale; KillTimer(hwnd, 1); }

static void ZoomTo(HWND hwnd, double s) {
    int cw, ch; ClientSize(hwnd, cw, ch);
    StartZoom(hwnd, s, cw / 2.0, ch / 2.0);
}

static std::wstring BaseName(const std::wstring& p) {
    size_t b = p.find_last_of(L"\\/");
    return b == std::wstring::npos ? p : p.substr(b + 1);
}

static const wchar_t* const kExts[] = {
    L".jpg", L".jpeg", L".png", L".bmp", L".gif",
    L".tif", L".tiff", L".ico", L".wmf", L".emf"
};
static bool IsImgExt(const std::wstring& n) {
    size_t d = n.find_last_of(L'.');
    if (d == std::wstring::npos) return false;
    std::wstring e = n.substr(d);
    for (size_t i = 0; i < sizeof(kExts)/sizeof(kExts[0]); ++i)
        if (_wcsicmp(e.c_str(), kExts[i]) == 0) return true;
    return false;
}

static void BuildList(const std::wstring& file) {
    g_list.clear(); g_idx = -1;
    size_t b = file.find_last_of(L'\\');
    if (b == std::wstring::npos) return;
    std::wstring dir = file.substr(0, b);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        if (IsImgExt(n)) g_list.push_back(dir + L"\\" + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // 简单插入排序（文件名不区分大小写）
    for (size_t i = 1; i < g_list.size(); ++i)
        for (size_t j = i; j > 0 && _wcsicmp(g_list[j-1].c_str(), g_list[j].c_str()) > 0; --j)
            std::swap(g_list[j-1], g_list[j]);
    for (size_t i = 0; i < g_list.size(); ++i)
        if (_wcsicmp(g_list[i].c_str(), file.c_str()) == 0) { g_idx = (int)i; break; }
}

static bool LoadFile(HWND hwnd, const std::wstring& pathRaw) {
    std::wstring path = pathRaw;
    std::replace(path.begin(), path.end(), L'/', L'\\');  // 统一分隔符
    g_err.clear();
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { g_err = L"无法打开文件"; return false; }
    DWORD hi = 0, lo = GetFileSize(hf, &hi);
    if (lo == INVALID_FILE_SIZE || hi != 0 || lo == 0) { CloseHandle(hf); g_err = L"文件无效"; return false; }
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, lo);
    if (!hg) { CloseHandle(hf); g_err = L"内存不足"; return false; }
    void* p = GlobalLock(hg);
    DWORD rd = 0;
    ReadFile(hf, p, lo, &rd, NULL);
    GlobalUnlock(hg);
    CloseHandle(hf);
    if (rd != lo) { GlobalFree(hg); g_err = L"读取失败"; return false; }

    IStream* st = nullptr;
    if (CreateStreamOnHGlobal(hg, TRUE, &st) != S_OK) { GlobalFree(hg); g_err = L"初始化失败"; return false; }
    Bitmap* nb = new Bitmap(st, FALSE);
    if (nb->GetLastStatus() != Ok) {
        delete nb; st->Release();
        g_err = L"无法解码此图片";
        return false;
    }
    if (g_img) delete g_img;
    if (g_stream) g_stream->Release();
    g_img = nb; g_stream = st;
    g_srcW = (int)g_img->GetWidth();
    g_srcH = (int)g_img->GetHeight();
    g_file = path;
    BuildList(path);
    std::wstring title = BaseName(path) + L" - 看图";
    SetWindowTextW(hwnd, title.c_str());
    FitToWindow(hwnd);
    return true;
}

static void Navigate(HWND hwnd, int step) {
    if (g_list.empty()) return;
    if (g_idx < 0) g_idx = 0;
    g_idx = (g_idx + step + (int)g_list.size()) % (int)g_list.size();
    LoadFile(hwnd, g_list[g_idx]);
}

static void PromptOpen(HWND hwnd) {
    wchar_t buf[MAX_PATH]; buf[0] = 0;
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"图片\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.wmf;*.emf\0"
                      L"所有文件\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) LoadFile(hwnd, buf);
}

// ---------- 文件关联（用户级注册表，无需管理员权限） ----------
static const wchar_t* kProgId = L"MiniViewer.Image";

static std::wstring ExePathQuoted() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return L"\"" + std::wstring(buf) + L"\"";
}

static void RegSetStr(const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value,
                       (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static bool RegHasDefault(const wchar_t* subkey) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    wchar_t v[512]; DWORD sz = sizeof(v), t = 0;
    LONG r = RegQueryValueExW(k, NULL, NULL, &t, (BYTE*)v, &sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && (t != REG_SZ || v[0]);
}

static bool IsRegistered() {
    HKEY k;
    std::wstring key = std::wstring(L"Software\\Classes\\") + kProgId + L"\\shell\\open\\command";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

static bool RegisterAssoc() {
    std::wstring exe = ExePathQuoted();
    std::wstring cmd = exe + L" \"%1\"";
    std::wstring base = std::wstring(L"Software\\Classes\\") + kProgId;
    RegSetStr(base.c_str(), NULL, L"看图 图片文件");
    RegSetStr((base + L"\\shell\\open\\command").c_str(), NULL, cmd.c_str());
    RegSetStr((base + L"\\DefaultIcon").c_str(), NULL, (exe + L",0").c_str());

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring appBase = std::wstring(L"Software\\Classes\\Applications\\") + BaseName(exePath);
    RegSetStr(appBase.c_str(), L"FriendlyAppName", L"看图");
    RegSetStr((appBase + L"\\shell\\open\\command").c_str(), NULL, cmd.c_str());
    RegSetStr((appBase + L"\\DefaultIcon").c_str(), NULL, (exe + L",0").c_str());

    for (size_t i = 0; i < sizeof(kExts) / sizeof(kExts[0]); ++i) {
        std::wstring ext = kExts[i];
        // 加入"打开方式"列表
        std::wstring ow = L"Software\\Classes\\" + ext + L"\\OpenWithProgids";
        RegSetStr(ow.c_str(), kProgId, L"");
        // 仅当该扩展名还没有默认打开方式时，顺带设为默认
        std::wstring dk = L"Software\\Classes\\" + ext;
        if (!RegHasDefault(dk.c_str()))
            RegSetStr(dk.c_str(), NULL, kProgId);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return IsRegistered();
}

static void UnregisterAssoc() {
    std::wstring base = std::wstring(L"Software\\Classes\\") + kProgId;
    SHDeleteKeyW(HKEY_CURRENT_USER, base.c_str());
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring appBase = std::wstring(L"Software\\Classes\\Applications\\") + BaseName(exePath);
    SHDeleteKeyW(HKEY_CURRENT_USER, appBase.c_str());
    for (size_t i = 0; i < sizeof(kExts) / sizeof(kExts[0]); ++i) {
        std::wstring ow = std::wstring(L"Software\\Classes\\") + kExts[i] + L"\\OpenWithProgids";
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, ow.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, kProgId);
            RegCloseKey(k);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

static void ShowNotice(HWND hwnd, const std::wstring& text) {
    g_notice = text;
    SetTimer(hwnd, 2, 6000, NULL);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void DrawImageArea(Graphics* g, int cw, int ch) {
    if (!g_img) return;
    double dw = g_srcW * g_scale, dh = g_srcH * g_scale;
    REAL ix = (REAL)g_offx, iy = (REAL)g_offy;
    REAL dx0 = dmax(ix, 0.0),        dy0 = dmax(iy, 0.0);
    REAL dx1 = dmin(ix + dw, (double)cw), dy1 = dmin(iy + dh, (double)ch);
    if (dx1 <= dx0 || dy1 <= dy0) return;
    REAL sx0 = (dx0 - ix) / (REAL)g_scale, sy0 = (dy0 - iy) / (REAL)g_scale;
    REAL sx1 = (dx1 - ix) / (REAL)g_scale, sy1 = (dy1 - iy) / (REAL)g_scale;

    ImageAttributes attrs;
    attrs.SetWrapMode(WrapModeTileFlipXY);   // 消除边缘插值白边
    g->SetPixelOffsetMode(PixelOffsetModeHalf);
    g->SetInterpolationMode(g_scale < 1.0 ? InterpolationModeHighQualityBicubic
                                          : InterpolationModeNearestNeighbor);
    g->DrawImage(g_img,
                 RectF(dx0, dy0, dx1 - dx0, dy1 - dy0),
                 sx0, sy0, sx1 - sx0, sy1 - sy0,
                 UnitPixel, &attrs);
}

static void DrawStatusBar(Graphics* g, int cw, int ch) {
    std::wstring text;
    BYTE ta = 255, tr = 208, tg = 208, tb = 214;
    if (!g_notice.empty()) {
        text = g_notice;
        tr = 150; tg = 230; tb = 160;   // 通知用绿色
    } else if (g_img) {
        int percent = (int)(g_scale * 1000.0);  // 精确到 0.1%
        text = BaseName(g_file)
             + L"    " + std::to_wstring(g_srcW) + L" × " + std::to_wstring(g_srcH)
             + L"    " + std::to_wstring(percent / 10) + L"." + std::to_wstring(percent % 10) + L"%";
    } else if (!g_err.empty()) {
        text = g_err + L"（按 Ctrl+O 重新选择）";
    } else {
        text = L"把图片拖进窗口，或按 Ctrl+O 打开";
    }
    static Font* font = nullptr;
    if (!font) font = new Font(L"Microsoft YaHei UI", 13, FontStyleRegular, UnitPixel);
    SolidBrush tbrush(Color(ta, tr, tg, tb));
    RectF box(0, 0, 0, 0);
    g->MeasureString(text.c_str(), (INT)-1, font, PointF(0, 0), &box);
    const REAL bh = 26;
    SolidBrush bb(Color(200, 12, 12, 16));
    g->FillRectangle(&bb, RectF(0, ch - bh, box.Width + 26, bh));
    g->DrawString(text.c_str(), (INT)-1, font,
                  PointF(12, ch - bh + (bh - box.Height) / 2), &tbrush);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_SIZE:
        if (g_img) {
            if (g_fit) FitToWindow(hwnd);
            else { ClampOffsets(hwnd); InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        int cw, ch; ClientSize(hwnd, cw, ch);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP mb = CreateCompatibleBitmap(hdc, dmax(cw,1), dmax(ch,1));
        HBITMAP old = (HBITMAP)SelectObject(mem, mb);
        {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeNone);
            SolidBrush bg(Color(255, 24, 24, 28));
            g.FillRectangle(&bg, 0, 0, cw, ch);
            DrawImageArea(&g, cw, ch);
            DrawStatusBar(&g, cw, ch);
        }
        BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(mb);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == 2) {  // 通知到期
            g_notice.clear();
            KillTimer(hwnd, 2);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_img && g_scale != g_target) {
            g_scale += (g_target - g_scale) * 0.35;
            if (fabs(g_target - g_scale) < g_target * 0.0004) g_scale = g_target;
            g_offx = g_ax - g_aix * g_scale;
            g_offy = g_ay - g_aiy * g_scale;
            ClampOffsets(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            if (g_scale == g_target) KillTimer(hwnd, 1);
        } else {
            KillTimer(hwnd, 1);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        if (!g_img) return 0;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        double d = (double)GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA;
        StartZoom(hwnd, g_target * pow(1.25, d), pt.x, pt.y);
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (g_img) {
            SetCapture(hwnd);
            g_drag = true;
            g_fit = false;   // 手动拖动后不再自动适应窗口
            g_lastPt.x = GET_X_LPARAM(lp);
            g_lastPt.y = GET_Y_LPARAM(lp);
            StopZoom(hwnd);
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g_drag && g_img) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            g_offx += x - g_lastPt.x;
            g_offy += y - g_lastPt.y;
            g_lastPt.x = x; g_lastPt.y = y;
            ClampOffsets(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_drag) {
            g_drag = false;
            ReleaseCapture();
            SetCursor(LoadCursor(NULL, IDC_ARROW));
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        if (g_img) {
            if (g_fit) { int cw, ch; ClientSize(hwnd, cw, ch); StartZoom(hwnd, 1.0, cw/2.0, ch/2.0); }
            else FitToWindow(hwnd);
        }
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:  Navigate(hwnd, -1); return 0;
        case VK_RIGHT: Navigate(hwnd,  1); return 0;
        case 'F':      FitToWindow(hwnd);  return 0;
        case '0':      ZoomTo(hwnd, 1.0);  return 0;
        case VK_ESCAPE: DestroyWindow(hwnd); return 0;
        case 'O':      if (GetKeyState(VK_CONTROL) & 0x8000) PromptOpen(hwnd); return 0;
        case 'A':      // 一键关联 / 取消关联
            if (IsRegistered()) {
                UnregisterAssoc();
                ShowNotice(hwnd, L"已取消文件关联");
            } else {
                RegisterAssoc();
                ShowNotice(hwnd, L"已注册到系统：右键图片 → 打开方式 → 看图（可设为始终使用）");
            }
            return 0;
        }
        break;

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hd, 0, path, MAX_PATH)) LoadFile(hwnd, path);
        DragFinish(hd);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmd, int show) {
    SetProcessDPIAware();
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_token, &gsi, NULL);

    // 命令行开关：/register 或 /unregister（静默执行后退出）
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmd, &argc);
    if (argv && argc >= 1) {
        std::wstring a0 = argv[0];
        if (_wcsicmp(a0.c_str(), L"/register") == 0 || _wcsicmp(a0.c_str(), L"-register") == 0) {
            RegisterAssoc();
            if (argv) LocalFree(argv);
            GdiplusShutdown(g_token);
            return 0;
        }
        if (_wcsicmp(a0.c_str(), L"/unregister") == 0 || _wcsicmp(a0.c_str(), L"-unregister") == 0) {
            UnregisterAssoc();
            if (argv) LocalFree(argv);
            GdiplusShutdown(g_token);
            return 0;
        }
    }

    WNDCLASSW wc = {0};
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"MiniImageViewer";
    RegisterClassW(&wc);

    const int ww = 1200, wh = 800;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"看图", WS_OVERLAPPEDWINDOW,
                                sx, sy, ww, wh, NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, show);

    bool loaded = false;
    if (argv && argc >= 1 && argv[0][0] && argv[0][0] != L'/') loaded = LoadFile(hwnd, argv[0]);
    if (argv) LocalFree(argv);
    if (!loaded && argc == 0) PromptOpen(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }

    if (g_img) delete g_img;
    if (g_stream) g_stream->Release();
    GdiplusShutdown(g_token);
    return 0;
}
