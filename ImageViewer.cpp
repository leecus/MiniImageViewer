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
        text = L"把图片拖进窗口，或按 Ctrl+O 打开（I 查看图片信息）";
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
        case VK_ESCAPE: DestroyWindow(hwnd); return 0;
        case VK_SPACE:  // GIF 暂停 / 继续
            if (!g_frames.empty()) {
                g_animPaused = !g_animPaused;
                if (g_animPaused) KillTimer(hwnd, 3);
                else SetTimer(hwnd, 3, g_delays[g_frame], NULL);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
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

    for (Bitmap* b : g_frames) delete b;
    if (g_img) delete g_img;
    if (g_stream) g_stream->Release();
    GdiplusShutdown(g_token);
    return 0;
}
