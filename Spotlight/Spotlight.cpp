#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#pragma warning(disable: 28251)

#include <windows.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cctype>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

// ==================== 1. DWM 与亚克力模糊/圆角 API 兼容定义 ====================
typedef enum _WINDOWCOMPOSITIONATTRIB {
    WCA_ACCENT_POLICY = 19
} WINDOWCOMPOSITIONATTRIB;

typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_INVALID_STATE = 5
} ACCENT_STATE;

struct ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attribute;
    PVOID pvData;
    SIZE_T cbData;
};

typedef BOOL(WINAPI* pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

// 避免 Windows SDK 重定义冲突：直接使用 DWM 属性 ID 值 33
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

static void EnableBlurBehindAndRounding(HWND hwnd) {
    // 1. 设置系统亚克力/毛玻璃
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        pfnSetWindowCompositionAttribute SetWindowCompositionAttribute =
            (pfnSetWindowCompositionAttribute)GetProcAddress(hUser32, "SetWindowCompositionAttribute");
        if (SetWindowCompositionAttribute) {
            ACCENT_POLICY accent = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, 0x601A1A1A, 0 };
            WINDOWCOMPOSITIONATTRIBDATA data;
            data.Attribute = WCA_ACCENT_POLICY;
            data.pvData = &accent;
            data.cbData = sizeof(accent);
            SetWindowCompositionAttribute(hwnd, &data);
        }
    }

    // 2. 使用数值 2 (DWMWCP_ROUND) 强制 DWM 启用系统原生圆角裁切
    DWORD cornerPreference = 2;
    DwmSetWindowAttribute(hwnd, (DWMWINDOWATTRIBUTE)DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
}

// ==================== 2. 常量定义 ====================
constexpr UINT_PTR HOTKEY_SPOTLIGHT = 101;
constexpr UINT_PTR TIMER_CURSOR_ID = 202;
constexpr UINT_PTR TIMER_ANIM_ID = 203;
constexpr UINT_PTR SUBCLASS_ID_EDIT = 301;

constexpr int SPOTLIGHT_WIDTH = 720;
constexpr int HEADER_HEIGHT = 68;
constexpr int ITEM_HEIGHT = 48;
constexpr int CORNER_RADIUS = 20;
constexpr int MARGIN_TOP = 16;

// ==================== 3. 动态状态变量 ====================
static BYTE g_currentAlpha = 0;
static BYTE g_targetAlpha = 0;

static float g_currentHeight = (float)HEADER_HEIGHT;
static float g_targetHeight = (float)HEADER_HEIGHT;

struct SearchEngine {
    std::wstring name;
    std::wstring baseUrl;
};

enum class ResultType {
    WebSearch,
    Calculator,
    App
};

struct SearchResult {
    ResultType type;
    std::wstring title;
    std::wstring subtitle;
    std::wstring target;
};

static std::vector<SearchEngine> g_engines = {
    { L"Google", L"https://www.google.com/search?q=" },
    { L"Bing",   L"https://www.bing.com/search?q=" },
    { L"Baidu",  L"https://www.baidu.com/s?wd=" }
};

static size_t g_currentEngineIndex = 0;
static RECT g_badgeRect = { 0, 0, 0, 0 };
static ULONG_PTR gdiplusToken = 0;
static HWND hMainWnd = NULL;
static HWND hHiddenEdit = NULL;
static bool isSpotlightVisible = false;

static std::wstring g_inputText = L"";
static bool g_showCursor = true;

struct AppInfo { std::wstring name; std::wstring path; };
static std::vector<AppInfo> g_installedApps;
static std::vector<SearchResult> g_results;
static int g_selectedIndex = 0;

// ==================== 4. 业务逻辑 ====================
static void ScanDirectoryApps(const std::wstring& dirPath) {
    std::wstring searchPath = dirPath + L"\\*";
    WIN32_FIND_DATAW fd = { 0 };
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                    ScanDirectoryApps(dirPath + L"\\" + fd.cFileName);
                }
            }
            else {
                std::wstring fileName = fd.cFileName;
                if (fileName.length() > 4 && fileName.substr(fileName.length() - 4) == L".lnk") {
                    std::wstring appName = fileName.substr(0, fileName.length() - 4);
                    std::wstring fullPath = dirPath + L"\\" + fileName;
                    g_installedApps.push_back({ appName, fullPath });
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

static void LoadLocalApps() {
    g_installedApps.clear();
    wchar_t path[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, path))) {
        ScanDirectoryApps(path);
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, path))) {
        ScanDirectoryApps(path);
    }
}

static bool TryEvaluateMath(const std::wstring& expr, double& outResult) {
    std::string s(expr.begin(), expr.end());
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
    if (s.empty()) return false;
    if (s.find_first_not_of("0123456789.+-*/()") != std::string::npos) return false;

    std::stringstream ss(s);
    double val = 0;
    char op = '+';
    double current = 0;

    if (ss >> current) {
        val = current;
        while (ss >> op >> current) {
            if (op == '+') val += current;
            else if (op == '-') val -= current;
            else if (op == '*') val *= current;
            else if (op == '/') { if (current != 0) val /= current; else return false; }
        }
        outResult = val;
        return true;
    }
    return false;
}

static void UpdateSearchResults() {
    g_results.clear();
    if (g_inputText.empty()) {
        g_selectedIndex = 0;
        g_targetHeight = (float)HEADER_HEIGHT;
        if (hMainWnd) SetTimer(hMainWnd, TIMER_ANIM_ID, 16, NULL);
        return;
    }

    double mathRes = 0;
    if (TryEvaluateMath(g_inputText, mathRes)) {
        wchar_t buf[128] = { 0 };
        swprintf_s(buf, L"= %g", mathRes);
        g_results.push_back({ ResultType::Calculator, buf, L"计算器结果 (按 Enter 复制)", std::to_wstring(mathRes) });
    }

    std::wstring lowerInput = g_inputText;
    std::transform(lowerInput.begin(), lowerInput.end(), lowerInput.begin(), ::tolower);

    for (const auto& app : g_installedApps) {
        std::wstring lowerName = app.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName.find(lowerInput) != std::wstring::npos) {
            g_results.push_back({ ResultType::App, app.name, L"应用程序", app.path });
            if (g_results.size() >= 5) break;
        }
    }

    std::wstring webTitle = L"在 " + g_engines[g_currentEngineIndex].name + L" 中搜索 \"" + g_inputText + L"\"";
    g_results.push_back({ ResultType::WebSearch, webTitle, L"网页搜索", g_inputText });

    g_selectedIndex = 0;

    int resultCount = (int)g_results.size();
    g_targetHeight = (float)(HEADER_HEIGHT + (resultCount * ITEM_HEIGHT) + (resultCount > 0 ? 12 : 0));
    if (hMainWnd) SetTimer(hMainWnd, TIMER_ANIM_ID, 16, NULL);
}

static void RenderLayeredWindow(HWND hwnd);

static void SwitchToNextEngine() {
    g_currentEngineIndex = (g_currentEngineIndex + 1) % g_engines.size();
    UpdateSearchResults();
    if (hMainWnd) RenderLayeredWindow(hMainWnd);
}

static void HideSpotlight(HWND hwnd) {
    g_targetAlpha = 0;
    SetTimer(hwnd, TIMER_ANIM_ID, 16, NULL);
}

static void ExecuteSelectedResult(HWND hwnd) {
    if (g_results.empty() || g_selectedIndex >= (int)g_results.size()) return;

    const auto& res = g_results[g_selectedIndex];
    if (res.type == ResultType::Calculator) {
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            size_t sizeInBytes = (res.target.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeInBytes);
            if (hMem) {
                wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
                // 修复：严谨检查 GlobalLock 返回值，避免 0/NULL 解引用
                if (pMem) {
                    memcpy(pMem, res.target.c_str(), sizeInBytes);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                else {
                    GlobalFree(hMem);
                }
            }
            CloseClipboard();
        }
    }
    else if (res.type == ResultType::App) {
        ShellExecute(NULL, L"open", res.target.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    else if (res.type == ResultType::WebSearch) {
        std::wstring url = g_engines[g_currentEngineIndex].baseUrl + res.target;
        ShellExecute(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    HideSpotlight(hwnd);
}

static void ShowSpotlight(HWND hwnd) {
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int x = (screenW - SPOTLIGHT_WIDTH) / 2;

    DWORD dwCurrentThread = GetCurrentThreadId();
    HWND hForegroundWnd = GetForegroundWindow();
    DWORD dwFGThread = 0;

    if (hForegroundWnd != NULL) {
        GetWindowThreadProcessId(hForegroundWnd, &dwFGThread);
    }

    if (dwFGThread != 0 && dwFGThread != dwCurrentThread) {
        AttachThreadInput(dwCurrentThread, dwFGThread, TRUE);
    }

    SetWindowPos(hwnd, HWND_TOPMOST, x, MARGIN_TOP, SPOTLIGHT_WIDTH, HEADER_HEIGHT, SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);

    if (dwFGThread != 0 && dwFGThread != dwCurrentThread) {
        AttachThreadInput(dwCurrentThread, dwFGThread, FALSE);
    }

    if (hHiddenEdit) SetFocus(hHiddenEdit);

    g_showCursor = true;
    SetTimer(hwnd, TIMER_CURSOR_ID, 500, NULL);

    UpdateSearchResults();

    g_currentAlpha = 0;
    g_targetAlpha = 255;
    g_currentHeight = (float)HEADER_HEIGHT;

    RenderLayeredWindow(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, TIMER_ANIM_ID, 16, NULL);

    isSpotlightVisible = true;
}

static void AddRoundedRectToPath(GraphicsPath& path, float x, float y, float width, float height, float radius) {
    float diameter = radius * 2.0f;
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

// ==================== 5. 核心渲染 ENGINE ====================
static void RenderLayeredWindow(HWND hwnd) {
    int renderHeight = (int)g_currentHeight;
    if (renderHeight < HEADER_HEIGHT) renderHeight = HEADER_HEIGHT;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int x = (screenW - SPOTLIGHT_WIDTH) / 2;

    SetWindowPos(hwnd, HWND_TOPMOST, x, MARGIN_TOP, SPOTLIGHT_WIDTH, renderHeight, SWP_NOZORDER | SWP_NOACTIVATE);

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) { ReleaseDC(NULL, hdcScreen); return; }

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = SPOTLIGHT_WIDTH;
    bmi.bmiHeader.biHeight = -renderHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HBITMAP hbmpMem = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);

    if (hbmpMem != NULL) {
        HBITMAP hbmpOld = (HBITMAP)SelectObject(hdcMem, hbmpMem);

        Graphics graphics(hdcMem);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        graphics.Clear(Color(0, 0, 0, 0));

        float w = static_cast<float>(SPOTLIGHT_WIDTH);
        float h = static_cast<float>(renderHeight);
        float r = static_cast<float>(CORNER_RADIUS);

        GraphicsPath bgPath;
        AddRoundedRectToPath(bgPath, 1.0f, 1.0f, w - 2.0f, h - 2.0f, r);

        SolidBrush bgBrush(Color(45, 20, 20, 25));
        graphics.FillPath(&bgBrush, &bgPath);

        Pen borderPen(Color(50, 255, 255, 255), 1.0f);
        graphics.DrawPath(&borderPen, &bgPath);

        FontFamily emojiFont(L"Segoe UI Emoji");
        Font iconFont(&emojiFont, 16, FontStyleRegular, UnitPoint);
        SolidBrush iconBrush(Color(220, 200, 205, 215));
        graphics.DrawString(L"🔍", -1, &iconFont, PointF(20, 20), &iconBrush);

        std::wstring engineText = g_engines[g_currentEngineIndex].name + L" ▾";
        FontFamily textFont(L"Segoe UI");
        Font labelFont(&textFont, 10, FontStyleBold, UnitPoint);

        RectF badgeTextBounds;
        graphics.MeasureString(engineText.c_str(), -1, &labelFont, PointF(0, 0), &badgeTextBounds);

        int badgeWidth = static_cast<int>(badgeTextBounds.Width) + 22;
        int badgeHeight = 30;
        int badgeX = static_cast<int>(w) - badgeWidth - 20;
        int badgeY = (HEADER_HEIGHT - badgeHeight) / 2;

        g_badgeRect = { badgeX, badgeY, badgeX + badgeWidth, badgeY + badgeHeight };

        SolidBrush badgeBrush(Color(60, 255, 255, 255));
        GraphicsPath badgePath;
        AddRoundedRectToPath(badgePath, (float)badgeX, (float)badgeY, (float)badgeWidth, (float)badgeHeight, 14.0f);
        graphics.FillPath(&badgeBrush, &badgePath);

        SolidBrush textBrush(Color(245, 245, 250));
        graphics.DrawString(engineText.c_str(), -1, &labelFont, PointF(badgeX + 11.0f, badgeY + 6.0f), &textBrush);

        float maxInputWidth = static_cast<float>(badgeX - 80);
        RectF clipArea(60.0f, 19.0f, maxInputWidth, 34.0f);
        Font inputFont(&textFont, 16, FontStyleRegular, UnitPoint);

        RectF textRealBounds;
        graphics.MeasureString(g_inputText.c_str(), -1, &inputFont, PointF(0, 0), &textRealBounds);

        float drawX = 60.0f;
        float cursorX = 60.0f;

        if (textRealBounds.Width > maxInputWidth) {
            drawX = 60.0f - (textRealBounds.Width - maxInputWidth);
            cursorX = 60.0f + maxInputWidth - 2.0f;
        }
        else {
            drawX = 60.0f;
            cursorX = 60.0f + (g_inputText.empty() ? 0 : textRealBounds.Width) - 2.0f;
        }

        GraphicsState state = graphics.Save();
        graphics.SetClip(clipArea);

        if (!g_inputText.empty()) {
            graphics.DrawString(g_inputText.c_str(), -1, &inputFont, PointF(drawX, 19.0f), &textBrush);
        }

        if (g_showCursor) {
            Pen cursorPen(Color(255, 245, 245, 250), 2.0f);
            graphics.DrawLine(&cursorPen, cursorX, 21.0f, cursorX, 47.0f);
        }
        graphics.Restore(state);

        int resultCount = (int)g_results.size();
        if (resultCount > 0 && renderHeight > HEADER_HEIGHT) {
            Pen splitPen(Color(30, 255, 255, 255), 1.0f);
            graphics.DrawLine(&splitPen, 16.0f, (float)HEADER_HEIGHT, w - 16.0f, (float)HEADER_HEIGHT);

            Font titleFont(&textFont, 12, FontStyleRegular, UnitPoint);
            Font subFont(&textFont, 9, FontStyleRegular, UnitPoint);
            SolidBrush subTextBrush(Color(160, 170, 185));

            RectF listClipArea(0.0f, (float)HEADER_HEIGHT, w, h - HEADER_HEIGHT);
            GraphicsState listState = graphics.Save();
            graphics.SetClip(listClipArea);

            for (int i = 0; i < resultCount; ++i) {
                int itemY = HEADER_HEIGHT + 6 + (i * ITEM_HEIGHT);
                if (itemY + ITEM_HEIGHT > renderHeight + 10) break;

                if (i == g_selectedIndex) {
                    SolidBrush selectBrush(Color(70, 255, 255, 255));
                    GraphicsPath itemPath;
                    AddRoundedRectToPath(itemPath, 12.0f, (float)itemY, w - 24.0f, (float)ITEM_HEIGHT, 8.0f);
                    graphics.FillPath(&selectBrush, &itemPath);
                }

                const wchar_t* itemIcon = L"🌐";
                if (g_results[i].type == ResultType::App) itemIcon = L"🚀";
                else if (g_results[i].type == ResultType::Calculator) itemIcon = L"🧮";

                graphics.DrawString(itemIcon, -1, &iconFont, PointF(22.0f, (float)itemY + 8.0f), &textBrush);
                graphics.DrawString(g_results[i].title.c_str(), -1, &titleFont, PointF(60.0f, (float)itemY + 6.0f), &textBrush);
                graphics.DrawString(g_results[i].subtitle.c_str(), -1, &subFont, PointF(60.0f, (float)itemY + 26.0f), &subTextBrush);
            }
            graphics.Restore(listState);
        }

        BYTE* pixels = static_cast<BYTE*>(pBits);
        int totalPixels = SPOTLIGHT_WIDTH * renderHeight;
        for (int i = 0; i < totalPixels; ++i) {
            BYTE a = pixels[i * 4 + 3];
            if (a == 0) {
                pixels[i * 4 + 0] = 0;
                pixels[i * 4 + 1] = 0;
                pixels[i * 4 + 2] = 0;
            }
            else {
                pixels[i * 4 + 0] = static_cast<BYTE>((pixels[i * 4 + 0] * a) / 255);
                pixels[i * 4 + 1] = static_cast<BYTE>((pixels[i * 4 + 1] * a) / 255);
                pixels[i * 4 + 2] = static_cast<BYTE>((pixels[i * 4 + 2] * a) / 255);
            }
        }

        POINT ptDst = { x, MARGIN_TOP };
        SIZE sizeDst = { SPOTLIGHT_WIDTH, renderHeight };
        POINT ptSrc = { 0, 0 };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, g_currentAlpha, AC_SRC_ALPHA };

        UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &sizeDst, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

        SelectObject(hdcMem, hbmpOld);
        DeleteObject(hbmpMem);
    }

    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// ==================== 6. 消息回调函数 ====================
static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_TAB) {
            SwitchToNextEngine();
            return 0;
        }
        else if (wParam == VK_ESCAPE) {
            HideSpotlight(GetParent(hWnd));
            return 0;
        }
        else if (wParam == VK_UP) {
            if (!g_results.empty()) {
                g_selectedIndex = (g_selectedIndex - 1 + (int)g_results.size()) % (int)g_results.size();
                RenderLayeredWindow(GetParent(hWnd));
            }
            return 0;
        }
        else if (wParam == VK_DOWN) {
            if (!g_results.empty()) {
                g_selectedIndex = (g_selectedIndex + 1) % (int)g_results.size();
                RenderLayeredWindow(GetParent(hWnd));
            }
            return 0;
        }
        else if (wParam == VK_RETURN) {
            ExecuteSelectedResult(GetParent(hWnd));
            return 0;
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK SpotlightProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        hMainWnd = hwnd;

        EnableBlurBehindAndRounding(hwnd);

        hHiddenEdit = CreateWindowEx(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            -1000, -1000, 500, 20,
            hwnd, (HMENU)1001, GetModuleHandle(NULL), NULL
        );

        SendMessage(hHiddenEdit, EM_SETLIMITTEXT, 0, 0);
        SetWindowSubclass(hHiddenEdit, EditSubclassProc, SUBCLASS_ID_EDIT, 0);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == TIMER_CURSOR_ID && isSpotlightVisible) {
            g_showCursor = !g_showCursor;
            RenderLayeredWindow(hwnd);
        }
        else if (wParam == TIMER_ANIM_ID) {
            bool needRender = false;

            int alphaStep = 35;
            if (g_currentAlpha < g_targetAlpha) {
                g_currentAlpha = (BYTE)min((int)g_targetAlpha, (int)g_currentAlpha + alphaStep);
                needRender = true;
            }
            else if (g_currentAlpha > g_targetAlpha) {
                g_currentAlpha = (BYTE)max((int)g_targetAlpha, (int)g_currentAlpha - alphaStep);
                needRender = true;

                if (g_currentAlpha == 0) {
                    KillTimer(hwnd, TIMER_ANIM_ID);
                    KillTimer(hwnd, TIMER_CURSOR_ID);
                    ShowWindow(hwnd, SW_HIDE);
                    isSpotlightVisible = false;
                    g_inputText.clear();
                    g_results.clear();
                    if (hHiddenEdit) SetWindowText(hHiddenEdit, L"");
                    return 0;
                }
            }

            float heightDiff = g_targetHeight - g_currentHeight;
            if (abs(heightDiff) > 0.5f) {
                g_currentHeight += heightDiff * 0.25f;
                needRender = true;
            }
            else {
                g_currentHeight = g_targetHeight;
            }

            if (needRender) {
                RenderLayeredWindow(hwnd);
            }
            else {
                KillTimer(hwnd, TIMER_ANIM_ID);
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == hHiddenEdit) {
            int length = GetWindowTextLength(hHiddenEdit);
            std::vector<wchar_t> buffer(length + 1);
            GetWindowText(hHiddenEdit, buffer.data(), length + 1);
            g_inputText = buffer.data();

            g_showCursor = true;
            UpdateSearchResults();
            RenderLayeredWindow(hwnd);
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        if (x >= g_badgeRect.left && x <= g_badgeRect.right &&
            y >= g_badgeRect.top && y <= g_badgeRect.bottom) {
            SwitchToNextEngine();
            return 0;
        }

        if (y > HEADER_HEIGHT && !g_results.empty()) {
            int clickedIdx = (y - HEADER_HEIGHT - 6) / ITEM_HEIGHT;
            if (clickedIdx >= 0 && clickedIdx < (int)g_results.size()) {
                g_selectedIndex = clickedIdx;
                ExecuteSelectedResult(hwnd);
                return 0;
            }
        }
        return 0;
    }

    case WM_ACTIVATE: {
        if (LOWORD(wParam) == WA_INACTIVE) {
            HideSpotlight(hwnd);
        }
        return 0;
    }

    case WM_DESTROY: {
        KillTimer(hwnd, TIMER_CURSOR_ID);
        KillTimer(hwnd, TIMER_ANIM_ID);
        RemoveWindowSubclass(hHiddenEdit, EditSubclassProc, SUBCLASS_ID_EDIT);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ==================== 7. 程序入口 ====================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    INITCOMMONCONTROLSEX icce = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icce);

    GdiplusStartupInput gdiplusInput = { 0 };
    GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL);

    LoadLocalApps();

    const wchar_t CLASS_NAME[] = L"MacSpotlightClass";

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = SpotlightProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);

    RegisterClass(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int x = (screenW - SPOTLIGHT_WIDTH) / 2;

    HWND hwndSpotlight = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        CLASS_NAME,
        L"Spotlight",
        WS_POPUP,
        x, MARGIN_TOP, SPOTLIGHT_WIDTH, HEADER_HEIGHT,
        NULL, NULL, hInstance, NULL
    );

    if (!hwndSpotlight) {
        GdiplusShutdown(gdiplusToken);
        return 0;
    }

    RegisterHotKey(NULL, HOTKEY_SPOTLIGHT, MOD_CONTROL | MOD_SHIFT, VK_SPACE);
    ShowWindow(hwndSpotlight, SW_HIDE);

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_SPOTLIGHT) {
            if (isSpotlightVisible) {
                HideSpotlight(hwndSpotlight);
            }
            else {
                ShowSpotlight(hwndSpotlight);
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(NULL, HOTKEY_SPOTLIGHT);
    GdiplusShutdown(gdiplusToken);

    return 0;
}