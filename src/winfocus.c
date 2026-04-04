/*
 * winfocus - 全ウィンドウをプライマリモニタに集約するツール
 *
 * Win11 上のすべての可視ウィンドウを通常サイズに復元し、
 * プライマリモニタの作業領域に移動する。
 * DisplayFusion によるウィンドウ再配置の前処理として使用する。
 *
 *   winfocus           全ウィンドウをプライマリモニタに集約（位置を自動保存）
 *   winfocus --save    現在のウィンドウ位置を保存する（移動なし）
 *   winfocus --restore 保存した位置に全ウィンドウを復元
 *
 * ビルド:
 *   task build
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* 除外するシステムウィンドウのクラス名 */
static const char *EXCLUDED_CLASSES[] = {
    "Shell_TrayWnd",           /* タスクバー */
    "Progman",                 /* デスクトップ */
    "WorkerW",                 /* デスクトップ背景 */
    "Button",                  /* デスクトップの「表示」ボタン */
    "Shell_SecondaryTrayWnd",  /* セカンダリモニタのタスクバー */
    NULL
};

/* 保存ファイル名（TEMP ディレクトリに配置） */
static const char SAVE_FILE_NAME[] = "winfocus_positions.dat";

/*
 * WS_EX_TOOLWINDOW を持つウィンドウのうち処理対象にするクラス名（設定ファイルから読み込む）
 *
 * winfocus.toml の [toolwindow_whitelist] セクションの classes 配列で設定する。
 * 最大 32 エントリ。
 */
#define MAX_WHITELIST_ENTRIES 32
static char g_whitelist[MAX_WHITELIST_ENTRIES][256];  /* ホワイトリストのクラス名 */
static int  g_whitelist_count = 0;                    /* ホワイトリストの登録数 */

/* 保存するウィンドウ情報 */
typedef struct {
    HWND            hwnd;
    DWORD           pid;
    char            className[256];
    WINDOWPLACEMENT placement;  /* 表示状態（最大化・最小化含む）と通常時の矩形 */
} WindowEntry;

/* 保存フェーズの EnumWindows コールバックコンテキスト */
typedef struct {
    WindowEntry *entries;
    int          count;
    int          capacity;
    DWORD        myPid;
} SaveContext;

/* 移動フェーズの EnumWindows コールバックコンテキスト */
typedef struct {
    RECT  workArea;  /* プライマリモニタの作業領域 */
    DWORD myPid;     /* 自プロセスの PID */
} MoveContext;

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
 * WS_EX_TOOLWINDOW の除外を免除するホワイトリスト対象か判定する
 */
static BOOL is_whitelisted_toolwindow(const char *className)
{
    for (int i = 0; i < g_whitelist_count; i++) {
        if (_stricmp(className, g_whitelist[i]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * 対象ウィンドウかどうかを判定する
 *
 * 可視・他プロセス・非除外クラスのウィンドウを対象とする。
 * WS_EX_TOOLWINDOW はホワイトリストにあれば除外しない。
 * className は GetClassNameA の出力先（256 文字以上を想定）。
 */
static BOOL is_target_window(HWND hwnd, DWORD myPid, char *className, int classNameSize)
{
    if (!IsWindowVisible(hwnd)) {
        return FALSE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == myPid) {
        return FALSE;
    }

    if (GetClassNameA(hwnd, className, classNameSize) == 0) {
        return FALSE;
    }

    if (is_excluded_class(className)) {
        return FALSE;
    }

    if ((GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) &&
        !is_whitelisted_toolwindow(className)) {
        return FALSE;
    }

    return TRUE;
}

/*
 * プライマリモニタ判定
 *
 * GetMonitorInfo 失敗時は TRUE を返し、不要なウィンドウ移動を防ぐ。
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
 *
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
 * 保存ファイルのパスを組み立てる
 */
static BOOL get_save_path(char *buf, DWORD bufSize)
{
    char tempDir[MAX_PATH];
    if (GetTempPathA(sizeof(tempDir), tempDir) == 0) {
        return FALSE;
    }
    int written = snprintf(buf, bufSize, "%s%s", tempDir, SAVE_FILE_NAME);
    return written > 0 && (DWORD)written < bufSize;
}

/*
 * 保存フェーズの EnumWindows コールバック
 * 対象ウィンドウの HWND・PID・クラス名・配置情報を収集する。
 */
static BOOL CALLBACK save_callback(HWND hwnd, LPARAM lParam)
{
    SaveContext *ctx = (SaveContext *)lParam;

    char className[256] = {0};
    if (!is_target_window(hwnd, ctx->myPid, className, sizeof(className))) {
        return TRUE;
    }

    /* 容量不足なら 2 倍に拡張 */
    if (ctx->count >= ctx->capacity) {
        int newCap = ctx->capacity == 0 ? 64 : ctx->capacity * 2;
        WindowEntry *newBuf = (WindowEntry *)realloc(ctx->entries, newCap * sizeof(WindowEntry));
        if (newBuf == NULL) {
            return TRUE;
        }
        ctx->entries  = newBuf;
        ctx->capacity = newCap;
    }

    WindowEntry *e = &ctx->entries[ctx->count];
    e->hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &e->pid);
    strncpy_s(e->className, sizeof(e->className), className, _TRUNCATE);
    e->placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(hwnd, &e->placement);

    ctx->count++;
    return TRUE;
}

/*
 * 移動フェーズの EnumWindows コールバック
 * 各ウィンドウをフィルタリングし、復元・全画面解除・モニタ移動・最小化を行う。
 */
static BOOL CALLBACK move_callback(HWND hwnd, LPARAM lParam)
{
    MoveContext *ctx = (MoveContext *)lParam;

    char className[256] = {0};
    if (!is_target_window(hwnd, ctx->myPid, className, sizeof(className))) {
        return TRUE;
    }

    BOOL iconic = IsIconic(hwnd);

    /* 最小化かつプライマリモニタ上なら移動不要（最小化状態を維持して終了）
     * rcNormalPosition はワークエリア相対座標のため MonitorFromRect では正しく判定できず、
     * MonitorFromWindow を内部で使用する is_on_primary で判定する。 */
    if (iconic && is_on_primary(hwnd)) {
        return TRUE;
    }

    if (iconic || IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
        Sleep(10);  /* 復元完了待ち */
    }

    /* F11 全画面の場合、VK_F11 をシミュレートして解除 */
    if (is_fullscreen(hwnd)) {
        SetForegroundWindow(hwnd);
        Sleep(10);  /* フォアグラウンド確定待ち */
        /* SendInput はフォアグラウンドウィンドウに届くため、
         * 他のウィンドウがフォーカスを奪った場合はスキップして誤入力を防ぐ */
        if (GetForegroundWindow() == hwnd) {
            INPUT inputs[2] = {0};
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki.wVk = VK_F11;
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki.wVk = VK_F11;
            inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, inputs, sizeof(INPUT));
            Sleep(150);  /* 全画面解除・ウィンドウ位置確定待ち */
        }
    }

    if (!is_on_primary(hwnd)) {
        SetWindowPos(hwnd, NULL,
                     ctx->workArea.left, ctx->workArea.top,
                     0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* 移動後に最小化して積み重なりを解消する
     * （--restore の SetWindowPlacement が安定して動作するよう状態をクリーンにする） */
    ShowWindow(hwnd, SW_MINIMIZE);

    /* メッセージキュー安定化のためのウェイト */
    Sleep(5);

    return TRUE;
}

/*
 * ウィンドウ配置を保存する
 *
 * 保存先：%TEMP%\winfocus_positions.dat
 * フォーマット：エントリ数（int）+ WindowEntry 配列
 */
static void save_positions(DWORD myPid)
{
    SaveContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.myPid = myPid;

    EnumWindows(save_callback, (LPARAM)&ctx);

    char savePath[MAX_PATH];
    BOOL hasSavePath = get_save_path(savePath, sizeof(savePath));

    if (ctx.count > 0) {
        if (hasSavePath) {
            FILE *fp = fopen(savePath, "wb");
            if (fp != NULL) {
                /* 書き込み失敗（ディスクフル等）時は不完全ファイルを削除 */
                BOOL ok = (fwrite(&ctx.count, sizeof(int), 1, fp) == 1) &&
                          ((size_t)fwrite(ctx.entries, sizeof(WindowEntry), ctx.count, fp) ==
                           (size_t)ctx.count);
                fclose(fp);
                if (!ok) {
                    DeleteFileA(savePath);
                }
            }
        }
    }
    else if (hasSavePath) {
        /* 対象ウィンドウなし：古い保存ファイルが残っていれば削除 */
        DeleteFileA(savePath);
    }

    free(ctx.entries);
}

/*
 * 保存したウィンドウ配置を復元する
 *
 * HWND・PID・クラス名が一致するウィンドウに SetWindowPlacement で配置を戻す。
 * 復元完了後に保存ファイルを削除する。
 */
static void restore_positions(void)
{
    char savePath[MAX_PATH];
    if (!get_save_path(savePath, sizeof(savePath))) {
        return;
    }

    FILE *fp = fopen(savePath, "rb");
    if (fp == NULL) {
        return;
    }

    int count = 0;
    if (fread(&count, sizeof(int), 1, fp) != 1 || count <= 0 || count > 10000) {
        fclose(fp);
        return;
    }

    WindowEntry *entries = (WindowEntry *)malloc(count * sizeof(WindowEntry));
    if (entries == NULL) {
        fclose(fp);
        return;
    }

    if ((int)fread(entries, sizeof(WindowEntry), count, fp) != count) {
        free(entries);
        fclose(fp);
        return;
    }
    fclose(fp);

    for (int i = 0; i < count; i++) {
        WindowEntry *e = &entries[i];

        /* HWND の有効性と同一ウィンドウ検証（PID + クラス名） */
        if (!IsWindow(e->hwnd)) {
            continue;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(e->hwnd, &pid);
        if (pid != e->pid) {
            continue;
        }

        char className[256] = {0};
        if (GetClassNameA(e->hwnd, className, sizeof(className)) == 0) {
            continue;
        }
        if (_stricmp(className, e->className) != 0) {
            continue;
        }

        /* 位置・表示状態（最大化・最小化含む）を一括復元
         *
         * SW_SHOWMAXIMIZED はウィンドウが現在いるモニタで最大化するため、
         * 先に通常表示で保存座標のモニタに移動してから最大化する 2 ステップで行う。
         */
        if (e->placement.showCmd == SW_SHOWMAXIMIZED) {
            WINDOWPLACEMENT wp = e->placement;
            wp.showCmd = SW_SHOWNORMAL;
            SetWindowPlacement(e->hwnd, &wp);
            Sleep(10);
            ShowWindow(e->hwnd, SW_MAXIMIZE);
        }
        else {
            SetWindowPlacement(e->hwnd, &e->placement);
        }
        Sleep(5);
    }

    free(entries);
    DeleteFileA(savePath);
}

/*
 * 設定ファイル（winfocus.toml）を読み込む
 *
 * exe と同じディレクトリの winfocus.toml から [toolwindow_whitelist] セクションの
 * classes 配列を読み込んで g_whitelist に格納する。
 * ファイルが存在しない場合や読み込み失敗時はホワイトリストなしで動作する。
 */
static void load_config(void)
{
    char exePath[MAX_PATH] = {0};
    DWORD fileNameLen = GetModuleFileNameA(NULL, exePath, sizeof(exePath));
    if (fileNameLen == 0 || fileNameLen >= sizeof(exePath)) {
        return;
    }

    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash == NULL) {
        return;
    }
    *(lastSlash + 1) = '\0';

    char configPath[MAX_PATH];
    int written = snprintf(configPath, sizeof(configPath), "%swinfocus.toml", exePath);
    if (written <= 0 || (DWORD)written >= sizeof(configPath)) {
        return;
    }

    FILE *fp = fopen(configPath, "r");
    if (fp == NULL) {
        return;
    }

    int in_section = 0;
    char line[1024];

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* 末尾の空白・改行を除去 */
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                            line[len - 1] == ' '  || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }

        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        /* セクションヘッダ */
        if (line[0] == '[') {
            in_section = (_stricmp(line, "[toolwindow_whitelist]") == 0);
            continue;
        }

        if (!in_section) {
            continue;
        }

        /* classes = ["...", "..."] の行を解析 */
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }

        /* キー部分を末尾空白トリムして完全一致確認 */
        char key[64] = {0};
        int keyLen = (int)(eq - line);
        while (keyLen > 0 && (line[keyLen - 1] == ' ' || line[keyLen - 1] == '\t')) {
            keyLen--;
        }
        if (keyLen <= 0 || keyLen >= (int)sizeof(key)) {
            continue;
        }
        strncpy_s(key, sizeof(key), line, keyLen);
        if (_stricmp(key, "classes") != 0) {
            continue;
        }

        char *p = eq + 1;
        while (*p && g_whitelist_count < MAX_WHITELIST_ENTRIES) {
            while (*p && *p != '"') {
                p++;
            }
            if (*p == '\0') {
                break;
            }
            p++;  /* 開きクォートをスキップ */

            char *start = p;
            while (*p && *p != '"') {
                p++;
            }
            if (*p == '\0') {
                break;
            }

            int entryLen = (int)(p - start);
            if (entryLen > 0 && entryLen < 256) {
                strncpy_s(g_whitelist[g_whitelist_count],
                          sizeof(g_whitelist[g_whitelist_count]),
                          start, entryLen);
                g_whitelist_count++;
            }
            p++;  /* 閉じクォートをスキップ */
        }
    }

    fclose(fp);
}

int main(int argc, char *argv[])
{
    load_config();

    const char *arg1 = (argc >= 2) ? argv[1] : NULL;

    if (arg1 && _stricmp(arg1, "--restore") == 0) {
        restore_positions();
        return 0;
    }

    DWORD myPid = GetCurrentProcessId();

    if (arg1 && _stricmp(arg1, "--save") == 0) {
        save_positions(myPid);
        return 0;
    }

    /* 移動前にウィンドウ配置を保存 */
    save_positions(myPid);

    MoveContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.myPid = myPid;

    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &ctx.workArea, 0)) {
        /* 取得失敗時はスクリーン全体を作業領域とする */
        ctx.workArea.left   = 0;
        ctx.workArea.top    = 0;
        ctx.workArea.right  = GetSystemMetrics(SM_CXSCREEN);
        ctx.workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    EnumWindows(move_callback, (LPARAM)&ctx);

    return 0;
}
