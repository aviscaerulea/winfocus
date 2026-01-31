/*
 * winfocus - 全ウィンドウをプライマリモニタに集約するツール
 *
 * Win11 上のすべてのアプリウィンドウ（最小化・トレイ常駐含む）を
 * 通常サイズに復元し、プライマリモニタの作業領域に移動する。
 * DisplayFusion によるウィンドウ再配置の前処理として使用する。
 *
 * ビルド:
 *   build.bat を Developer Command Prompt for VS で実行
 *   cl.exe /O2 /W4 /Fe:winfocus.exe winfocus.c user32.lib psapi.lib
 *
 * 設定:
 *   winfocus.cfg を exe と同じディレクトリに配置（オプション）
 *   トレイ常駐アプリの復元対象プロセスをホワイトリストで指定する
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <string.h>
#include <stdio.h>

/* ホワイトリストの最大エントリ数 */
#define MAX_WHITELIST 128

/* ホワイトリストのプロセス名の最大長 */
#define MAX_PROCNAME  260

/* ホワイトリストのタイトルパターンの最大長 */
#define MAX_TITLEPATTERN 260

/* ホワイトリストエントリ */
typedef struct {
    char procName[MAX_PROCNAME];           /* プロセス名 */
    char titlePattern[MAX_TITLEPATTERN];   /* タイトルパターン（空文字列の場合は全ウィンドウ対象） */
    char exePath[MAX_PATH];                /* 実行ファイルパス（ShellExecute 用、空文字列なら不使用） */
    BOOL found;                            /* EnumWindows で該当ウィンドウが見つかったか */
} WhitelistEntry;

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
    RECT             workArea;                  /* プライマリモニタの作業領域 */
    DWORD            myPid;                     /* 自プロセスの PID */
    WhitelistEntry   whitelist[MAX_WHITELIST];  /* ホワイトリスト */
    int              whitelistCount;            /* ホワイトリストのエントリ数 */
    BOOL             whitelistOnly;             /* TRUE: ホワイトリストのみ処理、FALSE: ホワイトリスト以外を処理 */
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
 * プロセス名とタイトルがホワイトリストに含まれるか判定する
 * タイトルパターンが空文字列の場合は、プロセス名のみで判定
 * 戻り値: 一致したエントリのインデックス（見つからなければ -1）
 */
static int find_whitelist_entry(const EnumContext *ctx, DWORD pid, const char *title)
{
    if (ctx->whitelistCount == 0) {
        return -1;
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProc == NULL) {
        return -1;
    }

    char procName[MAX_PROCNAME] = {0};
    int matchIndex = -1;

    if (GetModuleBaseNameA(hProc, NULL, procName, sizeof(procName)) > 0) {
        for (int i = 0; i < ctx->whitelistCount; i++) {
            /* プロセス名が一致するか判定 */
            if (_stricmp(procName, ctx->whitelist[i].procName) != 0) {
                continue;
            }

            /* タイトルパターンが空文字列の場合は、プロセス名のみで判定（後方互換性） */
            if (ctx->whitelist[i].titlePattern[0] == '\0') {
                matchIndex = i;
                break;
            }

            /* タイトルパターンが指定されている場合は、前方一致で判定 */
            size_t patternLen = strlen(ctx->whitelist[i].titlePattern);
            if (_strnicmp(title, ctx->whitelist[i].titlePattern, patternLen) == 0) {
                matchIndex = i;
                break;
            }
        }
    }

    CloseHandle(hProc);
    return matchIndex;
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
 * 指定プロセス名が実行中か EnumProcesses で確認する
 */
static BOOL is_process_running(const char *procName)
{
    DWORD pids[1024];
    DWORD bytesReturned;
    if (!EnumProcesses(pids, sizeof(pids), &bytesReturned)) {
        return FALSE;
    }

    DWORD count = bytesReturned / sizeof(DWORD);
    for (DWORD i = 0; i < count; i++) {
        if (pids[i] == 0) {
            continue;
        }
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pids[i]);
        if (hProc == NULL) {
            continue;
        }

        char name[MAX_PROCNAME] = {0};
        if (GetModuleBaseNameA(hProc, NULL, name, sizeof(name)) > 0) {
            if (_stricmp(name, procName) == 0) {
                CloseHandle(hProc);
                return TRUE;
            }
        }
        CloseHandle(hProc);
    }
    return FALSE;
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

    /* タイトルを取得 */
    char title[256] = {0};
    GetWindowTextA(hwnd, title, sizeof(title));
    BOOL hasTitle = (title[0] != '\0');

    /* ウィンドウスタイルを取得 */
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);

    /* ツールウィンドウは除外（ホワイトリストのプロセスは例外） */
    if (exStyle & WS_EX_TOOLWINDOW) {
        if (find_whitelist_entry(ctx, pid, title) < 0) {
            return TRUE;
        }
    }

    /* whitelistOnly モードでのフィルタリング */
    int earlyWhitelistIndex = find_whitelist_entry(ctx, pid, title);

    if (ctx->whitelistOnly) {
        /* ホワイトリストに一致しないウィンドウはスキップ */
        if (earlyWhitelistIndex < 0) {
            return TRUE;
        }
    } else {
        /* 非ホワイトリストモード: ホワイトリストのウィンドウはスキップ */
        if (earlyWhitelistIndex >= 0) {
            return TRUE;
        }
    }

    BOOL visible = IsWindowVisible(hwnd);
    int whitelistIndex = -1;

    if (visible) {
        /*
         * 可視ウィンドウの処理
         * ApplicationFrameHost の不可視子ウィンドウは EnumWindows では
         * トップレベルとして列挙されないため、ここでは可視のもののみ処理する
         */
        whitelistIndex = find_whitelist_entry(ctx, pid, title);
    } else {
        /*
         * 非表示ウィンドウの処理
         * ホワイトリストに該当し、タイトルありのもののみ対象とする
         */
        if (!hasTitle) {
            return TRUE;
        }

        whitelistIndex = find_whitelist_entry(ctx, pid, title);
        if (whitelistIndex < 0) {
            return TRUE;
        }
    }

    /* 最小化・最大化されていれば通常サイズに復元 */
    if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    /* 非表示のウィンドウを表示 */
    if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
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

    /* ホワイトリストウィンドウの移動完了を検証 */
    if (whitelistIndex >= 0 && is_on_primary(hwnd)) {
        ctx->whitelist[whitelistIndex].found = TRUE;
    }

    /* メッセージキュー安定化のためのウェイト */
    Sleep(5);

    return TRUE;
}

/*
 * ダブルクォート文字列をパースする（\\ エスケープ処理あり）
 */
static int parse_quoted_string(const char *src, char *dest, size_t destSize)
{
    const char *p = src;
    size_t len = 0;

    /* 先頭のダブルクォートをスキップ */
    if (*p != '"') {
        return -1;
    }
    p++;

    while (*p != '\0' && *p != '"' && len < destSize - 1) {
        if (*p == '\\' && *(p + 1) == '\\') {
            dest[len++] = '\\';
            p += 2;
        } else {
            dest[len++] = *p;
            p++;
        }
    }

    dest[len] = '\0';

    /* 末尾のダブルクォートを確認 */
    if (*p != '"') {
        return -1;
    }

    return 0;
}

/*
 * exe と同じディレクトリにある winfocus.toml を読み込む。
 * ファイルが存在しない場合は何もしない（ホワイトリスト 0 件）。
 */
static void load_config(EnumContext *ctx)
{
    ctx->whitelistCount = 0;

    /* exe のパスを取得 */
    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
        return;
    }

    /* ディレクトリ部分を取得して toml パスを構築 */
    char *lastSep = strrchr(exePath, '\\');
    if (lastSep == NULL) {
        return;
    }
    *(lastSep + 1) = '\0';

    char tomlPath[MAX_PATH] = {0};
    _snprintf_s(tomlPath, sizeof(tomlPath), _TRUNCATE, "%swinfocus.toml", exePath);

    FILE *fp = fopen(tomlPath, "r");
    if (fp == NULL) {
        return;
    }

    char line[1024];
    int inTraySection = 0;
    int currentEntry = -1;

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* 末尾の改行を除去 */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        /* 先頭の空白をスキップ */
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        /* 空行とコメント行をスキップ */
        if (*p == '\0' || *p == '#') {
            continue;
        }

        /* [[tray]] セクションの開始を検出 */
        if (strcmp(p, "[[tray]]") == 0) {
            /* 前のエントリが有効なら whitelistCount をインクリメント */
            if (currentEntry >= 0 && ctx->whitelist[currentEntry].procName[0] != '\0') {
                ctx->whitelistCount++;
            }

            if (ctx->whitelistCount >= MAX_WHITELIST) {
                break;
            }

            currentEntry = ctx->whitelistCount;
            ctx->whitelist[currentEntry].procName[0] = '\0';
            ctx->whitelist[currentEntry].titlePattern[0] = '\0';
            ctx->whitelist[currentEntry].exePath[0] = '\0';
            ctx->whitelist[currentEntry].found = FALSE;
            inTraySection = 1;
            continue;
        }

        /* [[tray]] セクション内でない場合はスキップ */
        if (!inTraySection || currentEntry < 0) {
            continue;
        }

        /* key = "value" のパース */
        char *eq = strchr(p, '=');
        if (eq == NULL) {
            continue;
        }

        /* key 部分を取得（末尾の空白を除去） */
        char *keyEnd = eq - 1;
        while (keyEnd >= p && (*keyEnd == ' ' || *keyEnd == '\t')) {
            keyEnd--;
        }
        *(keyEnd + 1) = '\0';

        /* value 部分を取得（先頭の空白をスキップ） */
        char *value = eq + 1;
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        /* ダブルクォート文字列をパース */
        char parsedValue[MAX_PATH] = {0};
        if (parse_quoted_string(value, parsedValue, sizeof(parsedValue)) != 0) {
            continue;
        }

        /* key に応じてフィールドに格納 */
        if (_stricmp(p, "process") == 0) {
            strncpy_s(ctx->whitelist[currentEntry].procName, MAX_PROCNAME, parsedValue, _TRUNCATE);
        } else if (_stricmp(p, "title") == 0) {
            strncpy_s(ctx->whitelist[currentEntry].titlePattern, MAX_TITLEPATTERN, parsedValue, _TRUNCATE);
        } else if (_stricmp(p, "exe") == 0) {
            strncpy_s(ctx->whitelist[currentEntry].exePath, MAX_PATH, parsedValue, _TRUNCATE);
        }
    }

    /* 最後のエントリが有効なら whitelistCount をインクリメント */
    if (inTraySection && currentEntry >= 0 && ctx->whitelist[currentEntry].procName[0] != '\0') {
        ctx->whitelistCount = currentEntry + 1;
    }

    fclose(fp);
}

int main(void)
{
    EnumContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* 自プロセスの PID を取得 */
    ctx.myPid = GetCurrentProcessId();

    /* ホワイトリストを読み込み */
    load_config(&ctx);

    /* プライマリモニタの作業領域を取得 */
    SystemParametersInfo(SPI_GETWORKAREA, 0, &ctx.workArea, 0);

    /* Phase 1: 非ホワイトリストウィンドウの処理（1 回のみ） */
    ctx.whitelistOnly = FALSE;
    EnumWindows(enum_callback, (LPARAM)&ctx);

    /* Phase 2: ホワイトリストウィンドウの処理 */
    ctx.whitelistOnly = TRUE;
    EnumWindows(enum_callback, (LPARAM)&ctx);

    /* ウィンドウ未検出かつプロセスが存在するエントリに対して ShellExecute で復元 */
    int shellExecuteCount = 0;
    for (int i = 0; i < ctx.whitelistCount; i++) {
        if (!ctx.whitelist[i].found &&
            ctx.whitelist[i].titlePattern[0] != '\0' &&
            ctx.whitelist[i].exePath[0] != '\0' &&
            is_process_running(ctx.whitelist[i].procName)) {

            ShellExecuteA(NULL, "open", ctx.whitelist[i].exePath, NULL, NULL, SW_SHOWNORMAL);
            shellExecuteCount++;
        }
    }

    /* ShellExecute でアプリを起動した場合、ウィンドウ出現をポーリング */
    if (shellExecuteCount > 0) {
        DWORD startTime = GetTickCount();
        DWORD timeout = 20000;  /* 20 秒タイムアウト */

        while (GetTickCount() - startTime < timeout) {
            Sleep(200);  /* 200ms 間隔でポーリング */

            /* enum_callback で found フラグ更新 + 復元 + 移動を同時実行 */
            EnumWindows(enum_callback, (LPARAM)&ctx);

            /* 全対象エントリの found が TRUE になったか確認 */
            BOOL allFound = TRUE;
            for (int i = 0; i < ctx.whitelistCount; i++) {
                if (!ctx.whitelist[i].found &&
                    ctx.whitelist[i].titlePattern[0] != '\0' &&
                    ctx.whitelist[i].exePath[0] != '\0') {
                    allFound = FALSE;
                    break;
                }
            }

            if (allFound) {
                break;
            }
        }
    }

    return 0;
}
