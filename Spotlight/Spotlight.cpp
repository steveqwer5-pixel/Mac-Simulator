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

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// ==================== 1. 常量定义 (现代 C++ constexpr) ====================
constexpr UINT_PTR HOTKEY_SPOTLIGHT = 101;
constexpr UINT_PTR TIMER_CURSOR_ID = 202;
constexpr UINT_PTR TIMER_ANIM_ID = 203;
constexpr UINT_PTR SUBCLASS_ID_EDIT = 301;

constexpr int SPOTLIGHT_WIDTH = 740;
constexpr int HEADER_HEIGHT = 72;
constexpr int ITEM_HEIGHT = 52;
constexpr int CORNER_RADIUS = 20;
constexpr int MARGIN_TOP = 20;

// ==================== 2. 全局状态变量 ====================
static BYTE g_currentAlpha = 0;
static BYTE g_targetAlpha = 0;

static float g_currentHeight = static_cast<float>(HEADER_HEIGHT);
static float g_targetHeight = static_cast<float>(HEADER_HEIGHT);

struct SearchEngine {
    std::wstring name;
    std::wstring baseUrl;
};

enum class ResultType { WebSearch, Calculator, App };

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

// ==================== 3. 前向声明与逻辑函数 ====================
static void RenderLayeredWindow(HWND hwnd);
static void HideSpotlight(HWND hwnd);
static void SwitchToNextEngine();
static void ExecuteSelectedResult(HWND hwnd);

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
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, path))) ScanDirectoryApps(path);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, path))) ScanDirectoryApps(path);
}

// 修复：加入非 ASCII (中文等) 安全校验，彻底解决中文输入闪退
static bool TryEvaluateMath(const std::wstring& expr, double& outResult) {
    if (expr.empty()) return false;

    // 1. 如果包含非 ASCII 字符（如中文），跳过计算器逻辑
    for (wchar_t ch : expr) {
        if (ch > 127) return false;
    }

    // 2. 过滤空白符
    std::string s;
    s.reserve(expr.size());
    for (wchar_t ch : expr) {
        if (!iswspace(ch)) {
            s.push_back(static_cast<char>(ch));
        }
    }

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

static void EnableWindowRoundingOnly(HWND hwnd) {
    DWORD cornerPreference = 2;
    DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(DWMWA_WINDOW_CORNER_PREFERENCE), &cornerPreference, sizeof(cornerPreference));
}

static void AddRoundedRectToPath(GraphicsPath& path, float x, float y, float width, float height, float radius) {
    float diameter = radius * 2.0f;
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

static void UpdateSearchResults() {
    g_results.clear();
    if (g_inputText.empty()) {
        g_selectedIndex = 0;
        g_targetHeight = static_cast<float>(HEADER_HEIGHT);
        if (hMainWnd) SetTimer(hMainWnd, TIMER_ANIM_ID, 16, NULL);
        return;
    }

    double mathRes = 0;
    if (TryEvaluateMath(g_inputText, mathRes)) {
        wchar_t buf[128] = { 0 };
        swprintf_s(buf, L"= %g", mathRes);
        g_results.push_back({ ResultType::Calculator, buf, L"计算器结果 • 按 Enter 复制结果", std::to_wstring(mathRes) });
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

    std::wstring webTitle = L"在 " + g_engines[g_currentEngineIndex].name + L" 搜 \"" + g_inputText + L"\"";
    g_results.push_back({ ResultType::WebSearch, webTitle, L"快捷网页搜索", g_inputText });

    g_selectedIndex = 0;
    int resultCount = static_cast<int>(g_results.size());
    g_targetHeight = static_cast<float>(HEADER_HEIGHT + (resultCount * ITEM_HEIGHT) + (resultCount > 0 ? 14 : 0));
    if (hMainWnd) SetTimer(hMainWnd, TIMER_ANIM_ID, 16, NULL);
}

// ==================== 4. 渲染界面 ====================
static void RenderLayeredWindow(HWND hwnd) {
    int renderHeight = static_cast<int>(g_currentHeight);
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
        HBITMAP hbmpOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbmpMem));

        Graphics graphics(hdcMem);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        graphics.Clear(Color(0, 0, 0, 0));

        float w = static_cast<float>(SPOTLIGHT_WIDTH);
        float h = static_cast<float>(renderHeight);
        float r = static_cast<float>(CORNER_RADIUS);

        // 主窗口圆角路径
        GraphicsPath bgPath;
        AddRoundedRectToPath(bgPath, 1.0f, 1.0f, w - 2.0f, h - 2.0f, r);

        // macOS 渐变暗黑磨砂背景
        LinearGradientBrush bgGradient(PointF(0, 0), PointF(0, h),
            Color(235, 34, 35, 42),
            Color(235, 22, 23, 28));
        graphics.FillPath(&bgGradient, &bgPath);

        // 边框
        Pen outerBorderPen(Color(45, 255, 255, 255), 1.0f);
        graphics.DrawPath(&outerBorderPen, &bgPath);

        GraphicsPath innerBorderPath;
        AddRoundedRectToPath(innerBorderPath, 2.0f, 2.0f, w - 4.0f, h - 4.0f, r - 1.0f);
        Pen innerBorderPen(Color(15, 255, 255, 255), 1.0f);
        graphics.DrawPath(&innerBorderPen, &innerBorderPath);

        // 搜索图标
        FontFamily emojiFont(L"Segoe UI Emoji");
        Font iconFont(&emojiFont, 18, FontStyleRegular, UnitPoint);
        SolidBrush iconBrush(Color(200, 160, 165, 180));
        graphics.DrawString(L"🔍", -1, &iconFont, PointF(22, 21), &iconBrush);

        // 搜索引擎 Badge
        std::wstring engineText = g_engines[g_currentEngineIndex].name + L"  ▾";
        FontFamily textFont(L"Segoe UI");
        Font labelFont(&textFont, 9.5f, FontStyleBold, UnitPoint);

        RectF badgeTextBounds;
        graphics.MeasureString(engineText.c_str(), -1, &labelFont, PointF(0, 0), &badgeTextBounds);

        int badgeWidth = static_cast<int>(badgeTextBounds.Width) + 24;
        int badgeHeight = 28;
        int badgeX = static_cast<int>(w) - badgeWidth - 20;
        int badgeY = (HEADER_HEIGHT - badgeHeight) / 2;

        g_badgeRect = { badgeX, badgeY, badgeX + badgeWidth, badgeY + badgeHeight };

        GraphicsPath badgePath;
        AddRoundedRectToPath(badgePath, static_cast<float>(badgeX), static_cast<float>(badgeY), static_cast<float>(badgeWidth), static_cast<float>(badgeHeight), 12.0f);

        SolidBrush badgeBrush(Color(40, 255, 255, 255));
        graphics.FillPath(&badgeBrush, &badgePath);
        Pen badgeBorderPen(Color(30, 255, 255, 255), 1.0f);
        graphics.DrawPath(&badgeBorderPen, &badgePath);

        SolidBrush badgeTextBrush(Color(220, 225, 235));
        graphics.DrawString(engineText.c_str(), -1, &labelFont, PointF(badgeX + 12.0f, badgeY + 6.0f), &badgeTextBrush);

        // 搜索输入框
        float maxInputWidth = static_cast<float>(badgeX - 85);
        RectF clipArea(64.0f, 20.0f, maxInputWidth, 36.0f);
        Font inputFont(&textFont, 17, FontStyleRegular, UnitPoint);

        RectF textRealBounds;
        graphics.MeasureString(g_inputText.c_str(), -1, &inputFont, PointF(0, 0), &textRealBounds);

        float drawX = 64.0f;
        float cursorX = 64.0f;

        if (textRealBounds.Width > maxInputWidth) {
            drawX = 64.0f - (textRealBounds.Width - maxInputWidth);
            cursorX = 64.0f + maxInputWidth - 2.0f;
        }
        else {
            drawX = 64.0f;
            cursorX = 64.0f + (g_inputText.empty() ? 0 : textRealBounds.Width) - 2.0f;
        }

        GraphicsState state = graphics.Save();
        graphics.SetClip(clipArea);

        SolidBrush textBrush(Color(250, 250, 255));
        if (!g_inputText.empty()) {
            graphics.DrawString(g_inputText.c_str(), -1, &inputFont, PointF(drawX, 20.0f), &textBrush);
        }
        else {
            SolidBrush placeholderBrush(Color(90, 150, 155, 170));
            graphics.DrawString(L"Spotlight 搜索...", -1, &inputFont, PointF(64.0f, 20.0f), &placeholderBrush);
        }

        // 光标
        if (g_showCursor) {
            Pen cursorPen(Color(255, 10, 132, 255), 2.0f);
            graphics.DrawLine(&cursorPen, cursorX, 23.0f, cursorX, 48.0f);
        }
        graphics.Restore(state);

        // 结果列表
        int resultCount = static_cast<int>(g_results.size());
        if (resultCount > 0 && renderHeight > HEADER_HEIGHT) {
            Pen splitPen(Color(20, 255, 255, 255), 1.0f);
            graphics.DrawLine(&splitPen, 18.0f, static_cast<float>(HEADER_HEIGHT), w - 18.0f, static_cast<float>(HEADER_HEIGHT));

            Font titleFont(&textFont, 12, FontStyleBold, UnitPoint);
            Font subFont(&textFont, 8.5f, FontStyleRegular, UnitPoint);

            RectF listClipArea(0.0f, static_cast<float>(HEADER_HEIGHT), w, h - HEADER_HEIGHT);
            GraphicsState listState = graphics.Save();
            graphics.SetClip(listClipArea);

            for (int i = 0; i < resultCount; ++i) {
                int itemY = HEADER_HEIGHT + 7 + (i * ITEM_HEIGHT);
                if (itemY + ITEM_HEIGHT > renderHeight + 10) break;

                // 选中高亮
                if (i == g_selectedIndex) {
                    GraphicsPath itemPath;
                    AddRoundedRectToPath(itemPath, 14.0f, static_cast<float>(itemY), w - 28.0f, static_cast<float>(ITEM_HEIGHT - 4), 10.0f);

                    LinearGradientBrush selectBrush(
                        PointF(0, static_cast<float>(itemY)),
                        PointF(0, static_cast<float>(itemY + ITEM_HEIGHT)),
                        Color(255, 0, 112, 245),
                        Color(255, 0, 88, 208)
                    );
                    graphics.FillPath(&selectBrush, &itemPath);
                }

                const wchar_t* itemIcon = L"🌐";
                if (g_results[i].type == ResultType::App) itemIcon = L"🚀";
                else if (g_results[i].type == ResultType::Calculator) itemIcon = L"🧮";

                SolidBrush itemIconBrush(i == g_selectedIndex ? Color(255, 255, 255) : Color(200, 205, 215));
                graphics.DrawString(itemIcon, -1, &iconFont, PointF(26.0f, static_cast<float>(itemY) + 8.0f), &itemIconBrush);

                SolidBrush itemTitleBrush(i == g_selectedIndex ? Color(255, 255, 255) : Color(240, 242, 248));
                SolidBrush itemSubBrush(i == g_selectedIndex ? Color(210, 230, 255) : Color(140, 150, 165));

                graphics.DrawString(g_results[i].title.c_str(), -1, &titleFont, PointF(66.0f, static_cast<float>(itemY) + 6.0f), &itemTitleBrush);
                graphics.DrawString(g_results[i].subtitle.c_str(), -1, &subFont, PointF(66.0f, static_cast<float>(itemY) + 27.0f), &itemSubBrush);
            }
            graphics.Restore(listState);
        }

        // Alpha 预乘
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

// ==================== 5. 事件回调与入口 ====================
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
    if (g_results.empty() || g_selectedIndex >= static_cast<int>(g_results.size())) return;

    const auto& res = g_results[g_selectedIndex];
    if (res.type == ResultType::Calculator) {
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            size_t sizeInBytes = (res.target.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeInBytes);
            if (hMem) {
                wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
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

    SetWindowPos(hwnd, HWND_TOPMOST, x, MARGIN_TOP, SPOTLIGHT_WIDTH, HEADER_HEIGHT, SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);

    if (hHiddenEdit) SetFocus(hHiddenEdit);

    g_showCursor = true;
    SetTimer(hwnd, TIMER_CURSOR_ID, 500, NULL);

    UpdateSearchResults();

    g_currentAlpha = 0;
    g_targetAlpha = 255;
    g_currentHeight = static_cast<float>(HEADER_HEIGHT);

    RenderLayeredWindow(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, TIMER_ANIM_ID, 16, NULL);

    isSpotlightVisible = true;
}

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
                g_selectedIndex = (g_selectedIndex - 1 + static_cast<int>(g_results.size())) % static_cast<int>(g_results.size());
                RenderLayeredWindow(GetParent(hWnd));
            }
            return 0;
        }
        else if (wParam == VK_DOWN) {
            if (!g_results.empty()) {
                g_selectedIndex = (g_selectedIndex + 1) % static_cast<int>(g_results.size());
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

        EnableWindowRoundingOnly(hwnd);

        hHiddenEdit = CreateWindowEx(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            -1000, -1000, 500, 20,
            hwnd, reinterpret_cast<HMENU>(1001), GetModuleHandle(NULL), NULL
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
                g_currentAlpha = static_cast<BYTE>(min(static_cast<int>(g_targetAlpha), static_cast<int>(g_currentAlpha) + alphaStep));
                needRender = true;
            }
            else if (g_currentAlpha > g_targetAlpha) {
                g_currentAlpha = static_cast<BYTE>(max(static_cast<int>(g_targetAlpha), static_cast<int>(g_currentAlpha) - alphaStep));
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
        if (HIWORD(wParam) == EN_CHANGE && reinterpret_cast<HWND>(lParam) == hHiddenEdit) {
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
            int clickedIdx = (y - HEADER_HEIGHT - 7) / ITEM_HEIGHT;
            if (clickedIdx >= 0 && clickedIdx < static_cast<int>(g_results.size())) {
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