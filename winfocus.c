/*
 * winfocus - 全ウィンドウをプライマリモニタに集約するツール
 *
 * Win11 上のすべての可視ウィンドウを通常サイズに復元し、
 * プライマリモニタの作業領域に移動する。
 * DisplayFusion によるウィンドウ再配置の前処理として使用する。
 *
 * ビルド:
 *   build.bat を Developer Command Prompt for VS で実行
 *   cl.exe /O2 /W4 /Fe:winfocus.exe winfocus.c user32.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* 除外するシステムウィンドウのクラス名 */
static const char *EXCLUDED_CLASSES[] = {
    "Shell_TrayWnd",           /* タスクバー */
    "Progman",                 /* デスクトップ */
    "WorkerW",                 /* デスクトップ背景 */
    "Button",                  /* デスクトップの「表示」ボタン */
    "Shell_SecondaryTrayWnd",  /* セカンダリモニタのタスクバー */
    NULL
};

/* EnumWindows コールバックに渡すコンテキスト */
typedef struct {
    RECT  workArea;  /* プライマリモニタの作業領域 */
    DWORD myPid;     /* 自プロセスの PID */
} EnumContext;

/*
 * 除外対象のシステムクラス名か判定する
 */
static BOOL is_excluded_class(const char *className)
{
    for (int i = 0; EXCLUDED_CLASSES[i] != NULL; i++) {
        if (_stricmp(className, EXCLUDED_CLASSES[i]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * ウィンドウがプライマリモニタ上にあるか判定する
 */
static BOOL is_on_primary(HWND hwnd)
{
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);

    if (!GetMonitorInfo(hMon, &mi)) {
        return TRUE;
    }

    return (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

/*
 * ウィンドウが F11 全画面かどうかを判定する
 * モニタの全領域（タスクバー含む）とウィンドウ矩形が一致するか
 */
static BOOL is_fullscreen(HWND hwnd)
{
    /* 通常の最大化（WS_MAXIMIZE）は対象外 */
    if (IsZoomed(hwnd)) {
        return FALSE;
    }

    RECT windowRect;
    if (!GetWindowRect(hwnd, &windowRect)) {
        return FALSE;
    }

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hMon, &mi)) {
        return FALSE;
    }

    /* モニタの全領域（rcMonitor、タスクバー含む）と比較 */
    return (windowRect.left   == mi.rcMonitor.left   &&
            windowRect.top    == mi.rcMonitor.top     &&
            windowRect.right  == mi.rcMonitor.right   &&
            windowRect.bottom == mi.rcMonitor.bottom);
}

/*
 * EnumWindows コールバック。各ウィンドウに対してフィルタリングと処理を行う。
 */
static BOOL CALLBACK enum_callback(HWND hwnd, LPARAM lParam)
{
    EnumContext *ctx = (EnumContext *)lParam;

    /* 自プロセスのウィンドウは除外 */
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->myPid) {
        return TRUE;
    }

    /* クラス名を取得 */
    char className[256] = {0};
    GetClassNameA(hwnd, className, sizeof(className));

    /* システムウィンドウクラスは除外 */
    if (is_excluded_class(className)) {
        return TRUE;
    }

    /* ウィンドウスタイルを取得 */
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);

    /* ツールウィンドウは除外 */
    if (exStyle & WS_EX_TOOLWINDOW) {
        return TRUE;
    }

    /* 非表示ウィンドウは除外 */
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    /* 最小化・最大化されていれば通常サイズに復元 */
    if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    /* F11 全画面の場合、VK_F11 をシミュレートして解除 */
    if (is_fullscreen(hwnd)) {
        SetForegroundWindow(hwnd);
        INPUT inputs[2] = {0};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_F11;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_F11;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
        Sleep(100);  /* 全画面解除待ち */
    }

    /* プライマリモニタ外にあれば移動 */
    if (!is_on_primary(hwnd)) {
        SetWindowPos(hwnd, NULL,
                     ctx->workArea.left, ctx->workArea.top,
                     0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* メッセージキュー安定化のためのウェイト */
    Sleep(5);

    return TRUE;
}

int main(void)
{
    EnumContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* 自プロセスの PID を取得 */
    ctx.myPid = GetCurrentProcessId();

    /* プライマリモニタの作業領域を取得 */
    SystemParametersInfo(SPI_GETWORKAREA, 0, &ctx.workArea, 0);

    /* 全ウィンドウを処理 */
    EnumWindows(enum_callback, (LPARAM)&ctx);

    /* 処理完了を通知 */
    MessageBeep(MB_OK);

    return 0;
}
