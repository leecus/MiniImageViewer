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
#include <cstring>
#include <cstdlib>

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

// GIF 动画：预合成的整帧缓存 + 每帧延时
static std::vector<Bitmap*> g_frames;
static std::vector<UINT>    g_delays;   // 毫秒
static int  g_frame = 0;
static bool g_animPaused = false;

// 图片信息面板（按 I 切换）
static bool g_showInfo = false;
static std::vector<std::wstring> g_info;
static ULONGLONG g_fileSize = 0;

static bool g_fullscreen = false;
static bool g_topmost = false;
static WINDOWPLACEMENT g_prevPlace = { sizeof(WINDOWPLACEMENT) };

static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }
static double ClampScale(double s) { return dmax(ZOOM_MIN, dmin(ZOOM_MAX, s)); }

static void ClientSize(HWND hwnd, int& cw, int& ch) {
    RECT rc; GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left; ch = rc.bottom - rc.top;
}

static RECT ClientRC(HWND hwnd) {   // 客户区矩形（屏幕坐标）
    RECT rc; GetClientRect(hwnd, &rc);
    MapWindowPoints(hwnd, NULL, (POINT*)&rc, 2);
    return rc;
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

// ---------- GIF 动画 ----------
static void FreeAnim(HWND hwnd) {
    KillTimer(hwnd, 3);
    for (Bitmap* b : g_frames) delete b;
    g_frames.clear(); g_delays.clear();
    g_frame = 0; g_animPaused = false;
}

static void SetupAnim(HWND hwnd) {
    if (!g_img) return;
    UINT nd = g_img->GetFrameDimensionsCount();
    if (nd < 1) return;
    std::vector<GUID> dims(nd);
    if (g_img->GetFrameDimensionsList(dims.data(), nd) != Ok) return;
    bool hasTime = false;
    for (UINT i = 0; i < nd; ++i)
        if (dims[i] == FrameDimensionTime) { hasTime = true; break; }
    if (!hasTime) return;
    UINT count = g_img->GetFrameCount(&FrameDimensionTime);
    if (count < 2) return;

    // 每帧延时（PropertyTagFrameDelay，单位 10ms）
    std::vector<UINT> raw(count, 10);
    UINT sz = g_img->GetPropertyItemSize(PropertyTagFrameDelay);
    if (sz >= sizeof(PropertyItem)) {
        PropertyItem* pi = (PropertyItem*)malloc(sz);
        if (pi) {
            if (g_img->GetPropertyItem(PropertyTagFrameDelay, sz, pi) == Ok &&
                pi->type == PropertyTagTypeLong && pi->length >= count * 4)
                memcpy(raw.data(), pi->value, count * 4);
            free(pi);
        }
    }

    // 预合成整帧：GDI+ SelectActiveFrame 已按 GIF  disposal 规则合成好画面
    for (UINT i = 0; i < count; ++i) {
        if (g_img->SelectActiveFrame(&FrameDimensionTime, i) != Ok) break;
        Bitmap* fb = new Bitmap(g_srcW, g_srcH, PixelFormat32bppARGB);
        {
            Graphics fg(fb);
            fg.SetCompositingMode(CompositingModeSourceCopy);  // 保留透明像素
            fg.SetInterpolationMode(InterpolationModeNearestNeighbor);
            fg.DrawImage(g_img, 0, 0, g_srcW, g_srcH);
        }
        if (fb->GetLastStatus() != Ok) { delete fb; break; }
        g_frames.push_back(fb);
        UINT d = raw[i] * 10;
        if (d < 20) d = 100;   // 与浏览器一致：过快帧按 100ms 处理
        g_delays.push_back(d);
    }
    if (g_frames.size() < 2) {  // 缓存失败则退回静态显示
        for (Bitmap* b : g_frames) delete b;
        g_frames.clear(); g_delays.clear();
        g_img->SelectActiveFrame(&FrameDimensionTime, 0);
        return;
    }
    g_frame = 0;
    SetTimer(hwnd, 3, g_delays[0], NULL);
}

// ---------- 图片信息 ----------
static std::wstring FmtSize(ULONGLONG b) {
    wchar_t buf[64];
    if (b < 1024) swprintf(buf, 64, L"%llu B", b);
    else if (b < 1024 * 1024) swprintf(buf, 64, L"%.1f KB", b / 1024.0);
    else swprintf(buf, 64, L"%.2f MB", b / (1024.0 * 1024.0));
    return buf;
}

static std::wstring FormatName(Bitmap* b) {
    // MinGW 头文件缺少 ImageFormatICO
    static const GUID kFmtICO = {0xb96b3cb5,0x0728,0x11d3,{0x9d,0x7b,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    GUID g;
    if (b->GetRawFormat(&g) != Ok) return L"未知";
    if (g == ImageFormatJPEG) return L"JPEG";
    if (g == ImageFormatPNG)  return L"PNG";
    if (g == ImageFormatGIF)  return L"GIF";
    if (g == ImageFormatBMP)  return L"BMP";
    if (g == ImageFormatTIFF) return L"TIFF";
    if (g == kFmtICO)         return L"ICO";
    if (g == ImageFormatWMF)  return L"WMF";
    if (g == ImageFormatEMF)  return L"EMF";
    return L"其他";
}

static std::wstring PixelFormatName(PixelFormat f) {
    wchar_t buf[64];
    UINT bits = (f >> 8) & 0xFF;
    if (f & PixelFormatIndexed) { swprintf(buf, 64, L"%u bpp 索引色", bits); return buf; }
    swprintf(buf, 64, L"%u bpp", bits);
    std::wstring s = buf;
    if (f & PixelFormatAlpha) s += L"（含透明通道）";
    return s;
}

// 读取 EXIF 属性到调用方缓冲（512 字节内）
static bool Prop(Bitmap* b, PROPID id, PropertyItem* out) {
    UINT sz = b->GetPropertyItemSize(id);
    if (!sz || sz > 512) return false;
    return b->GetPropertyItem(id, sz, out) == Ok;
}

static std::wstring PropAscii(PropertyItem* pi) {
    if (pi->type != PropertyTagTypeASCII || !pi->length) return L"";
    const char* s = (const char*)pi->value;
    std::wstring w;
    for (UINT i = 0; i < pi->length && s[i]; ++i) w += (wchar_t)(unsigned char)s[i];
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\r' || w.back() == L'\n')) w.pop_back();
    return w;
}

// 标准 EXIF 为 RATIONAL；部分软件写成两个 SHORT（分子/分母），一并兼容
static bool PropRational(PropertyItem* pi, ULONG& num, ULONG& den) {
    if (pi->type == PropertyTagTypeRational && pi->length >= 8) {
        ULONG* r = (ULONG*)pi->value; num = r[0]; den = r[1]; return true;
    }
    if (pi->type == PropertyTagTypeShort && pi->length >= 4) {
        USHORT* r = (USHORT*)pi->value; num = r[0]; den = r[1]; return true;
    }
    return false;
}

static void BuildInfo() {
    g_info.clear();
    if (!g_img) return;
    Bitmap* b = g_img;
    wchar_t buf[128];

    g_info.push_back(L"文件：" + BaseName(g_file));
    g_info.push_back(L"位置：" + g_file);
    g_info.push_back(L"大小：" + FmtSize(g_fileSize));
    swprintf(buf, 128, L"尺寸：%d × %d（%.2f MP）", g_srcW, g_srcH, g_srcW * (double)g_srcH / 1e6);
    g_info.push_back(buf);
    g_info.push_back(L"格式：" + FormatName(b) + L" · " + PixelFormatName(b->GetPixelFormat()));
    REAL dx = b->GetHorizontalResolution(), dy = b->GetVerticalResolution();
    if (dx > 0 && dy > 0) {
        swprintf(buf, 128, L"DPI：%.0f × %.0f", dx, dy);
        g_info.push_back(buf);
    }
    if (!g_frames.empty()) {
        ULONGLONG ms = 0;
        for (UINT d : g_delays) ms += d;
        swprintf(buf, 128, L"动画：%zu 帧 · 每轮 %.1f 秒 · 无限循环", g_frames.size(), ms / 1000.0);
        g_info.push_back(buf);
    }

    // EXIF（常见拍摄参数）
    ULONG storage[128];
    PropertyItem* pi = (PropertyItem*)storage;
    std::wstring make, model;
    if (Prop(b, 0x010F, pi)) make  = PropAscii(pi);   // Make
    if (Prop(b, 0x0110, pi)) model = PropAscii(pi);   // Model
    if (!make.empty() || !model.empty()) {
        std::wstring cam = make;
        if (!model.empty()) { if (!cam.empty()) cam += L" "; cam += model; }
        g_info.push_back(L"相机：" + cam);
    }
    if (Prop(b, 0x9003, pi)) {                        // DateTimeOriginal
        std::wstring dt = PropAscii(pi);
        if (dt.size() >= 10) { dt[4] = L'-'; dt[7] = L'-'; }
        if (!dt.empty()) g_info.push_back(L"拍摄时间：" + dt);
    }
    ULONG num = 0, den = 0;
    if (Prop(b, 0x829A, pi) && PropRational(pi, num, den) && num && den) {
        double v = num / (double)den;               // ExposureTime
        if (v < 1.0) swprintf(buf, 128, L"快门：1/%.0f 秒", den / (double)num);
        else swprintf(buf, 128, L"快门：%.1f 秒", v);
        g_info.push_back(buf);
    }
    if (Prop(b, 0x829D, pi) && PropRational(pi, num, den) && den) {
        swprintf(buf, 128, L"光圈：f/%.1f", num / (double)den);   // FNumber
        g_info.push_back(buf);
    }
    if (Prop(b, 0x8827, pi) && pi->type == PropertyTagTypeShort && pi->length >= 2) {
        swprintf(buf, 128, L"ISO：%u", *(USHORT*)pi->value);
        g_info.push_back(buf);
    }
    if (Prop(b, 0x920A, pi) && PropRational(pi, num, den) && den) {
        swprintf(buf, 128, L"焦距：%.1f mm", num / (double)den);  // FocalLength
        g_info.push_back(buf);
    }
}

static bool LoadFile(HWND hwnd, const std::wstring& pathRaw) {
    std::wstring path = pathRaw;
    std::replace(path.begin(), path.end(), L'/', L'\\');  // 统一分隔符
    wchar_t full[MAX_PATH * 2];
    if (GetFullPathNameW(path.c_str(), MAX_PATH * 2, full, NULL)) path = full;
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
    FreeAnim(hwnd);
    if (g_img) delete g_img;
    if (g_stream) g_stream->Release();
    g_img = nb; g_stream = st;
    g_fileSize = lo;
    g_srcW = (int)g_img->GetWidth();
    g_srcH = (int)g_img->GetHeight();
    g_file = path;
    BuildList(path);
    std::wstring title = BaseName(path) + L" - 看图";
    SetWindowTextW(hwnd, title.c_str());
    FitToWindow(hwnd);
    SetupAnim(hwnd);
    BuildInfo();
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

// ---------- 剪贴板 ----------
static bool CopyImageToClipboard(HWND hwnd) {
    Bitmap* src = g_frames.empty() ? g_img : g_frames[g_frame];
    if (!src) return false;
    int w = (int)src->GetWidth(), h = (int)src->GetHeight();
    // 流加载位图的 Clone 瞬态状态不可靠（GetLastStatus 读后即重置），以 LockBits 结果为准
    Bitmap* tmp = src->Clone(0, 0, w, h, PixelFormat32bppARGB);
    if (!tmp) return false;
    Rect r(0, 0, w, h);
    BitmapData bd;
    if (tmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) { delete tmp; return false; }
    SIZE_T bytes = (SIZE_T)w * h * 4;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (!hg) { tmp->UnlockBits(&bd); delete tmp; return false; }
    BITMAPINFOHEADER* bi = (BITMAPINFOHEADER*)GlobalLock(hg);
    memset(bi, 0, sizeof(*bi));
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = w; bi->biHeight = h;   // 正高度 = 自底向上
    bi->biPlanes = 1; bi->biBitCount = 32; bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)bytes;
    BYTE* dst = (BYTE*)(bi + 1);
    const BYTE* rows = (const BYTE*)bd.Scan0;
    for (int y = 0; y < h; ++y)
        memcpy(dst + (SIZE_T)(h - 1 - y) * w * 4, rows + (SIZE_T)y * bd.Stride, (SIZE_T)w * 4);
    GlobalUnlock(hg);
    tmp->UnlockBits(&bd);
    delete tmp;
    bool ok = false;
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_DIB, hg) != NULL;
        HBITMAP hb = nullptr;
        if (src->GetHBITMAP(Color(255, 255, 255), &hb) == Ok && hb)
            SetClipboardData(CF_BITMAP, hb);   // 白底 DDB，兼容只认 CF_BITMAP 的程序
        CloseClipboard();
    }
    if (!ok) GlobalFree(hg);
    return ok;
}

static bool CopyPathToClipboard(HWND hwnd) {
    if (g_file.empty()) return false;
    SIZE_T bytes = (g_file.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hg) return false;
    memcpy(GlobalLock(hg), g_file.c_str(), bytes);
    GlobalUnlock(hg);
    bool ok = false;
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_UNICODETEXT, hg) != NULL;
        CloseClipboard();
    }
    if (!ok) GlobalFree(hg);
    return ok;
}

// ---------- 旋转 / 翻转 ----------
static void RotateCurrent(HWND hwnd, RotateFlipType rf, const wchar_t* msg) {
    if (!g_img) return;
    g_img->RotateFlip(rf);
    for (Bitmap* b : g_frames) b->RotateFlip(rf);   // GIF 缓存帧同步旋转
    g_srcW = (int)g_img->GetWidth();
    g_srcH = (int)g_img->GetHeight();
    BuildInfo();
    FitToWindow(hwnd);
    ShowNotice(hwnd, msg);
}

// ---------- 全屏 / 置顶 ----------
static void ToggleFullscreen(HWND hwnd) {
    if (!g_fullscreen) {
        GetWindowPlacement(hwnd, &g_prevPlace);
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        SetWindowLongW(hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hm, &mi);
        SetWindowPos(hwnd, NULL, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = true;
    } else {
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        SetWindowLongW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_prevPlace);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

static void ToggleTopmost(HWND hwnd) {
    g_topmost = !g_topmost;
    SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ShowNotice(hwnd, g_topmost ? L"窗口已置顶" : L"已取消置顶");
}

// ---------- 删除到回收站 ----------
static void ClearImage(HWND hwnd) {
    FreeAnim(hwnd);
    if (g_img) { delete g_img; g_img = nullptr; }
    if (g_stream) { g_stream->Release(); g_stream = nullptr; }
    g_file.clear(); g_info.clear(); g_fileSize = 0;
    g_srcW = g_srcH = 0;
    g_list.clear(); g_idx = -1;
    SetWindowTextW(hwnd, L"看图");
    InvalidateRect(hwnd, NULL, FALSE);
}

static void DeleteCurrent(HWND hwnd) {
    if (!g_img || g_file.empty()) return;
    std::wstring msg = L"将「" + BaseName(g_file) + L"」移到回收站？";
    if (MessageBoxW(hwnd, msg.c_str(), L"删除确认", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    std::wstring from = g_file;
    from.push_back(L'\0');   // SHFileOperation 要求双 null 结尾
    SHFILEOPSTRUCTW op = {0};
    op.hwnd = hwnd; op.wFunc = FO_DELETE; op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) { ShowNotice(hwnd, L"删除失败"); return; }
    if (g_idx >= 0 && g_idx < (int)g_list.size()) g_list.erase(g_list.begin() + g_idx);
    if (g_list.empty()) ClearImage(hwnd);
    else {
        if (g_idx >= (int)g_list.size()) g_idx = 0;
        LoadFile(hwnd, g_list[g_idx]);
    }
    ShowNotice(hwnd, L"已移到回收站");
}

// ---------- 右键菜单 ----------
enum {
    IDM_FIT = 1001, IDM_ACTUAL, IDM_PREV, IDM_NEXT,
    IDM_ROTATE, IDM_MIRROR,
    IDM_INFO, IDM_COPY, IDM_COPYPATH, IDM_FOLDER,
    IDM_WALL_FILL, IDM_WALL_FIT, IDM_WALL_STRETCH, IDM_WALL_CENTER, IDM_WALL_TILE,
    IDM_FULL, IDM_TOP, IDM_DELETE,
    IDM_OPEN, IDM_ASSOC, IDM_EXIT
};

static void ToggleAssoc(HWND hwnd) {
    if (IsRegistered()) {
        UnregisterAssoc();
        ShowNotice(hwnd, L"已取消文件关联");
    } else {
        RegisterAssoc();
        ShowNotice(hwnd, L"已注册到系统：右键图片 → 打开方式 → 看图（可设为始终使用）");
    }
}

// 设置壁纸：导出无损 BMP 到 %APPDATA% 后经 SystemParametersInfo 应用。
// WallpaperStyle：10=填充 6=适应 2=拉伸 0=居中；平铺另置 TileWallpaper=1
static void SetWallpaper(HWND hwnd, const wchar_t* style, bool tile, const wchar_t* label) {
    if (!g_img) return;
    if (!g_frames.empty()) g_img->SelectActiveFrame(&FrameDimensionTime, 0);   // GIF 取第 0 帧
    wchar_t appdata[MAX_PATH] = L"";
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) || !appdata[0]) {
        ShowNotice(hwnd, L"设置壁纸失败");
        return;
    }
    std::wstring dir = std::wstring(appdata) + L"\\MiniImageViewer";
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring path = dir + L"\\wallpaper.bmp";
    // MinGW 头文件无内置 BMP 编码器 CLSID
    static const GUID kEncBMP = {0x557cf400,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    if (g_img->Save(path.c_str(), &kEncBMP, NULL) != Ok) {
        ShowNotice(hwnd, L"设置壁纸失败：无法导出图片");
        return;
    }
    RegSetStr(L"Control Panel\\Desktop", L"WallpaperStyle", style);
    RegSetStr(L"Control Panel\\Desktop", L"TileWallpaper", tile ? L"1" : L"0");
    wchar_t buf[MAX_PATH];
    wcscpy(buf, path.c_str());
    if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, buf, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        ShowNotice(hwnd, std::wstring(L"已设为桌面壁纸（") + label + L"）");
    else
        ShowNotice(hwnd, L"设置壁纸失败");
}

static int ShowContextMenu(HWND hwnd, POINT pt) {
    HMENU m = CreatePopupMenu();
    if (!m) return 0;
    bool has = g_img != nullptr;
    UINT en = has ? 0 : MF_GRAYED, nav = g_list.empty() ? MF_GRAYED : 0;
    AppendMenuW(m, MF_STRING | en, IDM_FIT,    L"适应窗口\tF");
    AppendMenuW(m, MF_STRING | en, IDM_ACTUAL, L"实际大小\t0");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | nav, IDM_PREV, L"上一张\t←");
    AppendMenuW(m, MF_STRING | nav, IDM_NEXT, L"下一张\t→");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | en, IDM_ROTATE, L"顺时针旋转 90°\tR");
    AppendMenuW(m, MF_STRING | en, IDM_MIRROR, L"水平镜像\tM");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    HMENU wall = CreatePopupMenu();
    AppendMenuW(wall, MF_STRING, IDM_WALL_FILL,    L"填充（推荐）");
    AppendMenuW(wall, MF_STRING, IDM_WALL_FIT,     L"适应");
    AppendMenuW(wall, MF_STRING, IDM_WALL_STRETCH, L"拉伸");
    AppendMenuW(wall, MF_STRING, IDM_WALL_CENTER,  L"居中");
    AppendMenuW(wall, MF_STRING, IDM_WALL_TILE,    L"平铺");
    AppendMenuW(m, MF_POPUP | en, (UINT_PTR)wall, L"设为壁纸");

    AppendMenuW(m, MF_STRING | en, IDM_INFO,
                g_showInfo ? L"隐藏图片信息\tI" : L"显示图片信息\tI");
    AppendMenuW(m, MF_STRING | en, IDM_COPY,   L"复制图片\tCtrl+C");
    AppendMenuW(m, MF_STRING | en, IDM_COPYPATH, L"复制文件路径\tCtrl+Shift+C");
    AppendMenuW(m, MF_STRING | en, IDM_FOLDER, L"打开所在文件夹");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_FULL, g_fullscreen ? L"退出全屏\tF11" : L"全屏\tF11");
    AppendMenuW(m, MF_STRING, IDM_TOP,  g_topmost ? L"取消置顶\tT" : L"窗口置顶\tT");
    AppendMenuW(m, MF_STRING | en, IDM_DELETE, L"移到回收站\tDel");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_OPEN,  L"打开图片…\tCtrl+O");
    AppendMenuW(m, MF_STRING, IDM_ASSOC, IsRegistered() ? L"取消文件关联\tA" : L"注册文件关联\tA");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出\tEsc");

    int id = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    return id;
}

static void HandleMenu(HWND hwnd, int id) {
    switch (id) {
    case IDM_FIT:    FitToWindow(hwnd); break;
    case IDM_ACTUAL: ZoomTo(hwnd, 1.0); break;
    case IDM_PREV:   Navigate(hwnd, -1); break;
    case IDM_NEXT:   Navigate(hwnd, 1);  break;
    case IDM_INFO:
        if (g_img) { g_showInfo = !g_showInfo; InvalidateRect(hwnd, NULL, FALSE); }
        break;
    case IDM_COPY:
        ShowNotice(hwnd, CopyImageToClipboard(hwnd) ? L"已复制图像到剪贴板" : L"复制失败");
        break;
    case IDM_COPYPATH:
        ShowNotice(hwnd, CopyPathToClipboard(hwnd) ? L"已复制文件路径" : L"复制失败");
        break;
    case IDM_ROTATE: RotateCurrent(hwnd, Rotate90FlipNone, L"顺时针旋转 90°"); break;
    case IDM_MIRROR: RotateCurrent(hwnd, RotateNoneFlipX, L"水平镜像"); break;
    case IDM_FULL:   ToggleFullscreen(hwnd); break;
    case IDM_TOP:    ToggleTopmost(hwnd); break;
    case IDM_DELETE: DeleteCurrent(hwnd); break;
    case IDM_WALL_FILL:    SetWallpaper(hwnd, L"10", false, L"填充"); break;
    case IDM_WALL_FIT:     SetWallpaper(hwnd, L"6",  false, L"适应"); break;
    case IDM_WALL_STRETCH: SetWallpaper(hwnd, L"2",  false, L"拉伸"); break;
    case IDM_WALL_CENTER:  SetWallpaper(hwnd, L"0",  false, L"居中"); break;
    case IDM_WALL_TILE:    SetWallpaper(hwnd, L"1",  true,  L"平铺"); break;
    case IDM_FOLDER:   // 资源管理器中定位并选中当前文件
        if (!g_file.empty()) {
            std::wstring params = L"/select,\"" + g_file + L"\"";
            ShellExecuteW(hwnd, L"open", L"explorer.exe", params.c_str(), NULL, SW_SHOWNORMAL);
        }
        break;
    case IDM_OPEN:  PromptOpen(hwnd); break;
    case IDM_ASSOC: ToggleAssoc(hwnd); break;
    case IDM_EXIT:  DestroyWindow(hwnd); break;
    }
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
    Bitmap* src = g_frames.empty() ? g_img : g_frames[g_frame];
    g->DrawImage(src,
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
        if (!g_frames.empty()) {
            text += L"    GIF " + std::to_wstring(g_frame + 1) + L"/" + std::to_wstring(g_frames.size());
            if (g_animPaused) text += L"（已暂停，空格继续）";
        }
    } else if (!g_err.empty()) {
        text = g_err + L"（按 Ctrl+O 重新选择）";
    } else {
        text = L"把图片拖进窗口，或按 Ctrl+O 打开（右键查看更多操作）";
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

static void DrawInfoPanel(Graphics* g, int cw, int ch) {
    if (!g_showInfo || g_info.empty()) return;
    static Font* font = nullptr;
    if (!font) font = new Font(L"Microsoft YaHei UI", 13, FontStyleRegular, UnitPixel);
    const REAL pad = 12;
    const REAL maxw = dmax(120.0, cw - 24.0 - pad * 2);
    std::vector<RectF> boxes(g_info.size());
    REAL totalH = 0, wmax = 0;
    for (size_t i = 0; i < g_info.size(); ++i) {
        g->MeasureString(g_info[i].c_str(), (INT)-1, font,
                         RectF(0, 0, maxw, 10000), &boxes[i]);
        totalH += boxes[i].Height + 4;
        wmax = dmax(wmax, (double)boxes[i].Width);
    }
    REAL pw = dmin(wmax, maxw) + pad * 2, ph = totalH - 4 + pad * 2;
    SolidBrush bg(Color(225, 16, 16, 22));
    g->FillRectangle(&bg, RectF(12, 12, pw, ph));
    Pen border(Color(70, 255, 255, 255), 1);
    g->DrawRectangle(&border, RectF(12, 12, pw, ph));
    SolidBrush tb(Color(255, 215, 215, 222));
    REAL y = 12 + pad;
    for (size_t i = 0; i < g_info.size(); ++i) {
        g->DrawString(g_info[i].c_str(), (INT)-1, font,
                      RectF(12 + pad, y, maxw, boxes[i].Height + 4), NULL, &tb);
        y += boxes[i].Height + 4;
    }
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
            DrawInfoPanel(&g, cw, ch);
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
        if (wp == 3) {  // GIF 下一帧
            if (!g_frames.empty() && !g_animPaused) {
                g_frame = (g_frame + 1) % (int)g_frames.size();
                InvalidateRect(hwnd, NULL, FALSE);
                SetTimer(hwnd, 3, g_delays[g_frame], NULL);
            } else {
                KillTimer(hwnd, 3);
            }
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
        case 'I':      // 图片信息面板
            if (g_img) { g_showInfo = !g_showInfo; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        case '0':      ZoomTo(hwnd, 1.0);  return 0;
        case 'R':      // 旋转（Shift 反向）
            if (GetKeyState(VK_SHIFT) & 0x8000) RotateCurrent(hwnd, Rotate270FlipNone, L"逆时针旋转 90°");
            else RotateCurrent(hwnd, Rotate90FlipNone, L"顺时针旋转 90°");
            return 0;
        case 'M':      RotateCurrent(hwnd, RotateNoneFlipX, L"水平镜像"); return 0;
        case 'C':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                if (GetKeyState(VK_SHIFT) & 0x8000)
                    ShowNotice(hwnd, CopyPathToClipboard(hwnd) ? L"已复制文件路径" : L"复制失败");
                else
                    ShowNotice(hwnd, CopyImageToClipboard(hwnd) ? L"已复制图像到剪贴板" : L"复制失败");
            }
            return 0;
        case 'T':      ToggleTopmost(hwnd);  return 0;
        case 'W':      SetWallpaper(hwnd, L"10", false, L"填充"); return 0;
        case VK_F11:   ToggleFullscreen(hwnd); return 0;
        case VK_DELETE: DeleteCurrent(hwnd); return 0;
        case VK_ESCAPE:
            if (g_fullscreen) { ToggleFullscreen(hwnd); return 0; }
            DestroyWindow(hwnd);
            return 0;
        case VK_SPACE:  // GIF 暂停 / 继续
            if (!g_frames.empty()) {
                g_animPaused = !g_animPaused;
                if (g_animPaused) KillTimer(hwnd, 3);
                else SetTimer(hwnd, 3, g_delays[g_frame], NULL);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case 'O':      if (GetKeyState(VK_CONTROL) & 0x8000) PromptOpen(hwnd); return 0;
        case 'A':      ToggleAssoc(hwnd); return 0;
        }
        break;

    case WM_CONTEXTMENU: {
        if ((HWND)wp != hwnd) return 0;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x == -1 && pt.y == -1) {   // 键盘触发（Shift+F10 / 菜单键）：弹在窗口中央
            RECT rc;
            GetWindowRect(hwnd, &rc);
            pt.x = (rc.left + rc.right) / 2;
            pt.y = (rc.top + rc.bottom) / 2;
        } else {
            RECT crc = ClientRC(hwnd);
            if (!PtInRect(&crc, pt)) break;   // 标题栏等非客户区交给系统菜单
        }
        int id = ShowContextMenu(hwnd, pt);
        if (id) HandleMenu(hwnd, id);
        return 0;
    }

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

    for (Bitmap* b : g_frames) delete b;
    if (g_img) delete g_img;
    if (g_stream) g_stream->Release();
    GdiplusShutdown(g_token);
    return 0;
}
