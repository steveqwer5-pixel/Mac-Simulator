#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#pragma warning(disable: 28251)

#include <windows.h>
#include <gdiplus.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <string>
#include <ctime>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ==================== 常量与全局变量 ====================
constexpr UINT_PTR TIMER_ANIM_ID = 501;
constexpr UINT_PTR TIMER_CLOCK_ID = 502;
constexpr UINT_PTR TIMER_TOPMOST_ID = 503;

constexpr float COLLAPSED_W = 200.0f;
constexpr float COLLAPSED_H = 36.0f;

constexpr float EXPANDED_W = 420.0f;
constexpr float EXPANDED_H = 180.0f;

enum TabType { TAB_CALENDAR = 0, TAB_CLOCK, TAB_MUSIC };

static ULONG_PTR gdiplusToken = 0;
static HWND hIslandWnd = NULL;

static bool g_isExpanded = false;
static float g_currentW = COLLAPSED_W;
static float g_currentH = COLLAPSED_H;

static TabType g_activeTab = TAB_MUSIC;

static float g_tabAnimProgress = 1.0f;
static bool  g_isTabAnimating = false;
static bool  g_isAudioPlaying = false;
static Gdiplus::Bitmap* g_pMusicCover = nullptr;

// ==================== 缓动算法 ====================
static float EaseOutElastic(float t) {
    constexpr float c4 = (2.0f * 3.14159265f) / 3.0f;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}

// ==================== 媒体控制与音频检测 ====================
static void SendMediaKey(BYTE vkCode) {
    keybd_event(vkCode, 0, 0, 0);
    keybd_event(vkCode, 0, KEYEVENTF_KEYUP, 0);
}

static bool CheckSystemAudioPlaying() {
    bool isPlaying = false;
    HRESULT hr = CoInitialize(NULL);
    IMMDeviceEnumerator* pEnumerator = NULL;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (SUCCEEDED(hr)) {
        IMMDevice* pDevice = NULL;
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (SUCCEEDED(hr)) {
            IAudioSessionManager2* pSessionManager = NULL;
            hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, NULL, (void**)&pSessionManager);
            if (SUCCEEDED(hr)) {
                IAudioSessionEnumerator* pSessionEnum = NULL;
                hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
                if (SUCCEEDED(hr)) {
                    int count = 0;
                    pSessionEnum->GetCount(&count);
                    for (int i = 0; i < count; ++i) {
                        IAudioSessionControl* pSessionCtrl = NULL;
                        if (SUCCEEDED(pSessionEnum->GetSession(i, &pSessionCtrl))) {
                            AudioSessionState state;
                            pSessionCtrl->GetState(&state);
                            if (state == AudioSessionStateActive) isPlaying = true;
                            pSessionCtrl->Release();
                        }
                    }
                    pSessionEnum->Release();
                }
                pSessionManager->Release();
            }
            pDevice->Release();
        }
        pEnumerator->Release();
    }
    CoUninitialize();
    return isPlaying;
}

// ==================== 正确的 Spotlight/WinIsland 纯粹液态路径 ====================
static void AddCleanIslandPath(GraphicsPath& path, float x, float y, float width, float height, float radius) {
    float diameter = radius * 2.0f;
    path.StartFigure();

    // 顶部贴紧屏幕，两侧完美优雅圆角，绝不向四周蔓延出多余填充
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);

    path.CloseFigure();
}

static void AddRoundedRect(GraphicsPath& path, float x, float y, float width, float height, float radius) {
    float diameter = radius * 2.0f;
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

// ==================== 图标绘制 ====================
static void DrawSidebarIcon(Graphics& g, int tabType, float x, float y, float size, Brush* brush) {
    if (tabType == TAB_CALENDAR) {
        Pen pen(brush, 1.5f);
        g.DrawRectangle(&pen, x, y + size * 0.2f, size, size * 0.8f);
        g.DrawLine(&pen, x, y + size * 0.45f, x + size, y + size * 0.45f);
        g.DrawLine(&pen, x + size * 0.3f, y, x + size * 0.3f, y + size * 0.25f);
        g.DrawLine(&pen, x + size * 0.7f, y, x + size * 0.7f, y + size * 0.25f);
    }
    else if (tabType == TAB_CLOCK) {
        Pen pen(brush, 1.5f);
        g.DrawEllipse(&pen, x, y, size, size);
        g.DrawLine(&pen, x + size * 0.5f, y + size * 0.5f, x + size * 0.5f, y + size * 0.25f);
        g.DrawLine(&pen, x + size * 0.5f, y + size * 0.5f, x + size * 0.72f, y + size * 0.5f);
    }
    else if (tabType == TAB_MUSIC) {
        g.FillEllipse(brush, x, y + size * 0.6f, size * 0.4f, size * 0.35f);
        g.FillEllipse(brush, x + size * 0.55f, y + size * 0.45f, size * 0.4f, size * 0.35f);
        Pen pen(brush, 2.0f);
        g.DrawLine(&pen, x + size * 0.32f, y + size * 0.7f, x + size * 0.32f, y + size * 0.15f);
        g.DrawLine(&pen, x + size * 0.87f, y + size * 0.55f, x + size * 0.87f, y + size * 0.05f);
        g.DrawLine(&pen, x + size * 0.32f, y + size * 0.15f, x + size * 0.87f, y + size * 0.05f);
    }
}

static void DrawMediaIcon(Graphics& g, int type, float x, float y, float size, Brush* brush) {
    if (type == 0) {
        PointF p1[] = { PointF(x + size * 0.5f, y), PointF(x, y + size * 0.5f), PointF(x + size * 0.5f, y + size) };
        PointF p2[] = { PointF(x + size, y), PointF(x + size * 0.5f, y + size * 0.5f), PointF(x + size, y + size) };
        g.FillPolygon(brush, p1, 3);
        g.FillPolygon(brush, p2, 3);
    }
    else if (type == 1) {
        if (g_isAudioPlaying) {
            g.FillRectangle(brush, x + size * 0.1f, y, size * 0.3f, size);
            g.FillRectangle(brush, x + size * 0.6f, y, size * 0.3f, size);
        }
        else {
            PointF pts[] = { PointF(x, y), PointF(x + size, y + size * 0.5f), PointF(x, y + size) };
            g.FillPolygon(brush, pts, 3);
        }
    }
    else if (type == 2) {
        PointF p1[] = { PointF(x, y), PointF(x + size * 0.5f, y + size * 0.5f), PointF(x, y + size) };
        PointF p2[] = { PointF(x + size * 0.5f, y), PointF(x + size, y + size * 0.5f), PointF(x, y + size) };
        g.FillPolygon(brush, p1, 3);
        g.FillPolygon(brush, p2, 3);
    }
}

// ==================== 核心渲染函数 ====================
static void RenderIsland(HWND hwnd) {
    int renderW = static_cast<int>(g_currentW + 0.5f);
    int renderH = static_cast<int>(g_currentH + 0.5f);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int x = (screenW - renderW) / 2;
    int y = 0; // 贴紧顶部

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, renderW, renderH, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = renderW;
    bmi.bmiHeader.biHeight = -renderH;
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

        // 1. 强制清空画布 (全透明 Alpha = 0)
        graphics.Clear(Color(0, 0, 0, 0));

        float w = static_cast<float>(renderW);
        float h = static_cast<float>(renderH);
        float animProgress = (g_currentH - COLLAPSED_H) / (EXPANDED_H - COLLAPSED_H);

        // 2. 绘制纯净灵动岛黑色胶囊/Notch
        GraphicsPath islandPath;
        float cornerRadius = 18.0f + animProgress * 6.0f;
        AddCleanIslandPath(islandPath, 0.0f, 0.0f, w, h, cornerRadius);

        // 深黑主体背景
        SolidBrush mainBgBrush(Color(248, 12, 12, 15));
        graphics.FillPath(&mainBgBrush, &islandPath);

        // 细腻外边缘高光
        Pen glowPen(Color(35, 255, 255, 255), 1.0f);
        graphics.DrawPath(&glowPen, &islandPath);

        Gdiplus::FontFamily mainFontFamily(L"Microsoft YaHei");
        Gdiplus::Font titleFont(&mainFontFamily, 11, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::Font subFont(&mainFontFamily, 8.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::Font clockFont(&mainFontFamily, 28, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

        SolidBrush whiteBrush(Color(255, 255, 255));
        SolidBrush grayBrush(Color(145, 150, 160));
        SolidBrush activeTabBrush(Color(50, 255, 255, 255));
        SolidBrush greenBrush(Color(255, 48, 209, 88));

        // 3. UI 内容绘制
        if (animProgress < 0.2f) {
            DrawSidebarIcon(graphics, TAB_MUSIC, 28.0f, 10.0f, 14.0f, &whiteBrush);

            if (g_isAudioPlaying) {
                graphics.FillRectangle(&greenBrush, w - 40.0f, 13.0f, 3.0f, 10.0f);
                graphics.FillRectangle(&greenBrush, w - 34.0f, 9.0f, 3.0f, 18.0f);
                graphics.FillRectangle(&greenBrush, w - 28.0f, 14.0f, 3.0f, 8.0f);
            }
            else {
                SolidBrush lensBrush(Color(255, 60, 60, 70));
                graphics.FillEllipse(&lensBrush, w - 36.0f, 14.0f, 8.0f, 8.0f);
            }
        }
        else {
            // Sidebar
            GraphicsPath sidebarPath;
            AddRoundedRect(sidebarPath, 20.0f, 16.0f, 44.0f, h - 32.0f, 12.0f);
            SolidBrush sidebarBg(Color(30, 255, 255, 255));
            graphics.FillPath(&sidebarBg, &sidebarPath);

            for (int i = 0; i < 3; ++i) {
                float tabY = 24.0f + i * 46.0f;
                if (g_activeTab == i) {
                    GraphicsPath activePill;
                    AddRoundedRect(activePill, 26.0f, tabY - 2.0f, 32.0f, 32.0f, 8.0f);
                    graphics.FillPath(&activeTabBrush, &activePill);
                }
                DrawSidebarIcon(graphics, i, 34.0f, tabY + 6.0f, 16.0f, &whiteBrush);
            }

            // 选项卡切换物理弹跳 (EaseOutElastic)
            float bounceScale = EaseOutElastic(g_tabAnimProgress);
            float bounceOffsetY = (1.0f - bounceScale) * 18.0f;
            BYTE cardAlpha = static_cast<BYTE>(g_tabAnimProgress * 255.0f);

            float contentX = 82.0f;
            float contentY = 20.0f + bounceOffsetY;

            if (g_activeTab == TAB_CALENDAR) {
                time_t now = time(nullptr);
                tm ltm;
                localtime_s(&ltm, &now);

                const wchar_t* weeks[] = { L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六" };
                wchar_t dateStr[64];
                swprintf_s(dateStr, L"%d年%d月%d日 %s", ltm.tm_year + 1900, ltm.tm_mon + 1, ltm.tm_mday, weeks[ltm.tm_wday]);

                graphics.DrawString(L"系统日历", -1, &titleFont, PointF(contentX, contentY), &whiteBrush);
                graphics.DrawString(dateStr, -1, &subFont, PointF(contentX, contentY + 24.0f), &grayBrush);

                GraphicsPath dayBox;
                AddRoundedRect(dayBox, contentX, contentY + 48.0f, 60.0f, 60.0f, 10.0f);
                SolidBrush boxBrush(Color(cardAlpha, 0, 122, 255));
                graphics.FillPath(&boxBrush, &dayBox);

                wchar_t dayNumStr[8];
                swprintf_s(dayNumStr, L"%02d", ltm.tm_mday);
                Gdiplus::Font bigDayFont(&mainFontFamily, 20, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                graphics.DrawString(dayNumStr, -1, &bigDayFont, PointF(contentX + 11.0f, contentY + 62.0f), &whiteBrush);

                graphics.DrawString(L"今日日程安排正常", -1, &subFont, PointF(contentX + 72.0f, contentY + 60.0f), &whiteBrush);
                graphics.DrawString(L"无紧急事项提醒", -1, &subFont, PointF(contentX + 72.0f, contentY + 82.0f), &grayBrush);
            }
            else if (g_activeTab == TAB_CLOCK) {
                time_t now = time(nullptr);
                tm ltm;
                localtime_s(&ltm, &now);

                wchar_t timeBuf[32];
                swprintf_s(timeBuf, L"%02d:%02d:%02d", ltm.tm_hour, ltm.tm_min, ltm.tm_sec);

                graphics.DrawString(timeBuf, -1, &clockFont, PointF(contentX + 10.0f, contentY + 16.0f), &whiteBrush);
                graphics.DrawString(L"实时系统时钟 (北京时间)", -1, &subFont, PointF(contentX + 14.0f, contentY + 76.0f), &grayBrush);
            }
            else if (g_activeTab == TAB_MUSIC) {
                GraphicsPath albumCover;
                AddRoundedRect(albumCover, contentX, contentY + 2.0f, 64.0f, 64.0f, 12.0f);

                if (g_pMusicCover && g_pMusicCover->GetLastStatus() == Ok) {
                    graphics.SetClip(&albumCover);
                    graphics.DrawImage(g_pMusicCover, contentX, contentY + 2.0f, 64.0f, 64.0f);
                    graphics.ResetClip();
                }
                else {
                    LinearGradientBrush coverGrad(PointF(contentX, contentY), PointF(contentX + 64, contentY + 64),
                        g_isAudioPlaying ? Color(cardAlpha, 0, 122, 255) : Color(cardAlpha, 45, 45, 55),
                        g_isAudioPlaying ? Color(cardAlpha, 255, 45, 85) : Color(cardAlpha, 20, 20, 25));
                    graphics.FillPath(&coverGrad, &albumCover);
                    DrawSidebarIcon(graphics, TAB_MUSIC, contentX + 22.0f, contentY + 22.0f, 20.0f, &whiteBrush);
                }

                if (g_isAudioPlaying) {
                    graphics.DrawString(L"系统媒体播放中", -1, &titleFont, PointF(contentX + 80.0f, contentY + 10.0f), &whiteBrush);
                    graphics.DrawString(L"已识别音频通道...", -1, &subFont, PointF(contentX + 80.0f, contentY + 36.0f), &greenBrush);
                }
                else {
                    graphics.DrawString(L"未检测到音乐", -1, &titleFont, PointF(contentX + 80.0f, contentY + 10.0f), &whiteBrush);
                    graphics.DrawString(L"播放器静音中", -1, &subFont, PointF(contentX + 80.0f, contentY + 36.0f), &grayBrush);
                }

                float ctrlY = contentY + 95.0f;
                DrawMediaIcon(graphics, 0, contentX + 80.0f, ctrlY, 16.0f, &whiteBrush);
                DrawMediaIcon(graphics, 1, contentX + 140.0f, ctrlY, 16.0f, &whiteBrush);
                DrawMediaIcon(graphics, 2, contentX + 200.0f, ctrlY, 16.0f, &whiteBrush);
            }
        }

        // 4. Alpha 通道预乘 (必不可少)
        BYTE* pixels = static_cast<BYTE*>(pBits);
        int totalPixels = renderW * renderH;
        for (int i = 0; i < totalPixels; ++i) {
            BYTE a = pixels[i * 4 + 3];
            if (a == 0) {
                pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 0;
            }
            else {
                pixels[i * 4 + 0] = static_cast<BYTE>((pixels[i * 4 + 0] * a) / 255);
                pixels[i * 4 + 1] = static_cast<BYTE>((pixels[i * 4 + 1] * a) / 255);
                pixels[i * 4 + 2] = static_cast<BYTE>((pixels[i * 4 + 2] * a) / 255);
            }
        }

        POINT ptDst = { x, y };
        SIZE sizeDst = { renderW, renderH };
        POINT ptSrc = { 0, 0 };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

        UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &sizeDst, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

        SelectObject(hdcMem, hbmpOld);
        DeleteObject(hbmpMem);
    }

    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// ==================== 消息循环 ====================
static LRESULT CALLBACK IslandProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        g_pMusicCover = Gdiplus::Bitmap::FromFile(L"cover.jpg");
        SetTimer(hwnd, TIMER_CLOCK_ID, 1000, NULL);
        SetTimer(hwnd, TIMER_TOPMOST_ID, 100, NULL);

        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        if (!g_isExpanded) {
            g_isExpanded = true;
            SetTimer(hwnd, TIMER_ANIM_ID, 16, NULL);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!g_isExpanded) return 0;
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        if (x >= 20 && x <= 64) {
            TabType targetTab = g_activeTab;
            if (y >= 15 && y <= 55) targetTab = TAB_CALENDAR;
            else if (y >= 60 && y <= 100) targetTab = TAB_CLOCK;
            else if (y >= 105 && y <= 145) targetTab = TAB_MUSIC;

            if (targetTab != g_activeTab) {
                g_activeTab = targetTab;
                g_tabAnimProgress = 0.0f;
                g_isTabAnimating = true;
                SetTimer(hwnd, TIMER_ANIM_ID, 16, NULL);
            }
            return 0;
        }

        if (g_activeTab == TAB_MUSIC && y >= 105 && y <= 145) {
            if (x >= 150 && x <= 185) SendMediaKey(VK_MEDIA_PREV_TRACK);
            else if (x >= 210 && x <= 245) SendMediaKey(VK_MEDIA_PLAY_PAUSE);
            else if (x >= 270 && x <= 305) SendMediaKey(VK_MEDIA_NEXT_TRACK);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (g_isExpanded) {
            g_isExpanded = false;
            SetTimer(hwnd, TIMER_ANIM_ID, 16, NULL);
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == TIMER_ANIM_ID) {
            bool reRender = false;
            float targetW = g_isExpanded ? EXPANDED_W : COLLAPSED_W;
            float targetH = g_isExpanded ? EXPANDED_H : COLLAPSED_H;

            float diffW = targetW - g_currentW;
            float diffH = targetH - g_currentH;

            if (std::abs(diffW) > 0.5f || std::abs(diffH) > 0.5f) {
                g_currentW += diffW * 0.22f;
                g_currentH += diffH * 0.22f;
                reRender = true;
            }
            else {
                g_currentW = targetW;
                g_currentH = targetH;
            }

            if (g_isTabAnimating) {
                g_tabAnimProgress += 0.08f;
                if (g_tabAnimProgress >= 1.0f) {
                    g_tabAnimProgress = 1.0f;
                    g_isTabAnimating = false;
                }
                reRender = true;
            }

            if (reRender) {
                RenderIsland(hwnd);
            }
            else if (!g_isTabAnimating) {
                KillTimer(hwnd, TIMER_ANIM_ID);
            }
        }
        else if (wParam == TIMER_CLOCK_ID) {
            g_isAudioPlaying = CheckSystemAudioPlaying();
            if (g_isExpanded || g_isAudioPlaying) {
                RenderIsland(hwnd);
            }
        }
        else if (wParam == TIMER_TOPMOST_ID) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_DESTROY:
        if (g_pMusicCover) delete g_pMusicCover;
        KillTimer(hwnd, TIMER_ANIM_ID);
        KillTimer(hwnd, TIMER_CLOCK_ID);
        KillTimer(hwnd, TIMER_TOPMOST_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ==================== 主入口 ====================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    GdiplusStartupInput gdiplusInput = { 0 };
    GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL);

    const wchar_t CLASS_NAME[] = L"WinIslandCleanClass";

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = IslandProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClass(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int initX = (screenW - static_cast<int>(COLLAPSED_W)) / 2;

    hIslandWnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        CLASS_NAME, L"WinIslandClean",
        WS_POPUP,
        initX, 0,
        static_cast<int>(COLLAPSED_W), static_cast<int>(COLLAPSED_H),
        NULL, NULL, hInstance, NULL
    );

    if (hIslandWnd) {
        RenderIsland(hIslandWnd);
        ShowWindow(hIslandWnd, SW_SHOWNA);

        MSG msg = { 0 };
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    GdiplusShutdown(gdiplusToken);
    return 0;
}