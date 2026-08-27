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
 *   winfocus --raise   設定した exe のウィンドウを前面化
 *
 * ビルド:
 *   task build
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

/* 除外するシステムウィンドウのクラス名 */
static const char *EXCLUDED_CLASSES[] = {
    "Shell_TrayWnd",           /* タスクバー */
    "Progman",                 /* デスクトップ */
    "WorkerW",                 /* デスクトップ背景 */
    "Button",                  /* デスクトップの「表示」ボタン */
    "Shell_SecondaryTrayWnd",  /* セカンダリモニタのタスクバー */
    NULL
};

/* 保存ファイル名（exe と同じディレクトリに配置） */
static const char SAVE_FILE_NAME[] = "winfocus.dat";

/* 保存ファイルのフォーマット識別子 */
#define SAVE_FILE_MAGIC   0x57464353UL  /* "WFCS" = WinFocusSave */
#define SAVE_FILE_VERSION 2             /* WindowEntry 構造体変更時にインクリメント */

/*
 * WS_EX_TOOLWINDOW を持つウィンドウのうち処理対象にするクラス名（設定ファイルから読み込む）
 *
 * winfocus.toml の [toolwindow_whitelist] セクションの classes 配列で設定する。
 * 最大 32 エントリ。
 */
#define MAX_WHITELIST_ENTRIES 32
static char g_whitelist[MAX_WHITELIST_ENTRIES][256];  /* ホワイトリストのクラス名 */
static int  g_whitelist_count = 0;                    /* ホワイトリストの登録数 */

/* load_config で使用するセクション識別子 */
#define SECTION_NONE          0
#define SECTION_TOOLWINDOW_WL 1
#define SECTION_SAVE_FILE     2
#define SECTION_RAISE         3

/* 保存ファイル有効期限のデフォルト値（時間） */
#define DEFAULT_SAVE_FILE_EXPIRY_HOURS 24

/* 保存ファイルの有効期限（時間）
 * winfocus.toml の [save_file] セクションの expiry_hours で上書きできる。
 * --restore 実行時、ファイル更新日時から本値以上経過していればファイル不在として扱う。
 * 0 を設定すると有効期限判定は無効化される。 */
static int g_save_file_expiry_hours = DEFAULT_SAVE_FILE_EXPIRY_HOURS;

/* 復元時に受け入れるエントリ数の上限
 * 保存ファイルの破損や異常データに対する安全策。通常の使用では数十〜数百エントリ程度。 */
#define MAX_RESTORE_ENTRIES 10000

/* --raise で前面化する exe 名（設定ファイルから読み込み）
 * winfocus.toml の [raise] セクションの apps 配列で設定する。最大 32 エントリ。
 * デフォルト値は main() で設定し、load_config() で [raise] セクション検出時にクリアする。 */
#define MAX_RAISE_APPS 32
static char g_raise_apps[MAX_RAISE_APPS][256];
static int  g_raise_app_count = 0;

/* 保存するウィンドウ情報 */
typedef struct {
    HWND            hwnd;
    DWORD           pid;
    char            className[256];
    WINDOWPLACEMENT placement;  /* showCmd（表示状態）の取得と、最小化エントリの
                                 * 通常時位置（rcNormalPosition）復元に使う。
                                 * 通常状態・最大化状態の位置は rect を使う。 */
    RECT            rect;       /* GetWindowRect のスクリーン座標。SetWindowPos で復元 */
    BOOL            isTopmost;  /* WS_EX_TOPMOST フラグ（Z オーダー復元用） */
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

/* --raise で収集するウィンドウ情報 */
typedef struct {
    HWND hwnd;
    int  appIndex;   /* g_raise_apps 内のインデックス（ソート用） */
    int  enumIndex;  /* EnumWindows の列挙順（Z オーダー安定化用） */
    BOOL iconic;     /* 最小化状態 */
} RaiseEntry;

/* --raise の EnumWindows コールバックコンテキスト */
typedef struct {
    RaiseEntry *entries;
    int         count;
    int         capacity;
    DWORD       myPid;
} RaiseContext;

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
 * ウィンドウの所属プロセスの exe ファイル名を取得する
 *
 * PID からプロセスハンドルを開き、フルパスからファイル名部分を抽出する。
 * 取得失敗時は FALSE を返す。
 */
static BOOL get_process_exe_name(HWND hwnd, char *buf, int bufSize)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return FALSE;
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc == NULL) {
        return FALSE;
    }

    char fullPath[MAX_PATH];
    DWORD size = sizeof(fullPath);
    if (!QueryFullProcessImageNameA(hProc, 0, fullPath, &size)) {
        CloseHandle(hProc);
        return FALSE;
    }
    CloseHandle(hProc);

    const char *fileName = strrchr(fullPath, '\\');
    fileName = fileName ? fileName + 1 : fullPath;

    strncpy_s(buf, bufSize, fileName, _TRUNCATE);
    return TRUE;
}

/*
 * --raise の対象ウィンドウかどうかを判定する
 *
 * 可視・他プロセス・非除外クラスのウィンドウのうち、exe 名が g_raise_apps に
 * 一致するものを対象とする。一致時は *appIndex にインデックスを書き込む。
 */
static BOOL is_raise_target(HWND hwnd, DWORD myPid, int *appIndex)
{
    if (!IsWindowVisible(hwnd)) {
        return FALSE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == myPid) {
        return FALSE;
    }

    char className[256] = {0};
    if (GetClassNameA(hwnd, className, sizeof(className)) == 0) {
        return FALSE;
    }

    if (is_excluded_class(className)) {
        return FALSE;
    }

    char exeName[MAX_PATH] = {0};
    if (!get_process_exe_name(hwnd, exeName, sizeof(exeName))) {
        return FALSE;
    }

    for (int i = 0; i < g_raise_app_count; i++) {
        if (_stricmp(exeName, g_raise_apps[i]) == 0) {
            *appIndex = i;
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
 * exe が置かれているディレクトリを取得する（末尾に \ を含む）
 */
static BOOL get_exe_dir(char *buf, DWORD bufSize)
{
    DWORD len = GetModuleFileNameA(NULL, buf, bufSize);
    if (len == 0 || len >= bufSize) {
        return FALSE;
    }
    char *lastSlash = strrchr(buf, '\\');
    if (lastSlash == NULL) {
        return FALSE;
    }
    lastSlash[1] = '\0';
    return TRUE;
}

/*
 * 保存ファイルのパスを組み立てる
 */
static BOOL get_save_path(char *buf, DWORD bufSize)
{
    char exeDir[MAX_PATH];
    if (!get_exe_dir(exeDir, sizeof(exeDir))) {
        return FALSE;
    }
    int written = snprintf(buf, bufSize, "%s%s", exeDir, SAVE_FILE_NAME);
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

    /* 容量不足時の倍増拡張
     * 初期 capacity は 0 のため最初の確保は 64 エントリ。
     * realloc 失敗時は ctx->count を 0 にして列挙を中断し、不完全な部分データの保存を抑止する。 */
    if (ctx->count >= ctx->capacity) {
        int newCap = ctx->capacity == 0 ? 64 : ctx->capacity * 2;
        WindowEntry *newBuf = (WindowEntry *)realloc(ctx->entries, newCap * sizeof(WindowEntry));
        if (newBuf == NULL) {
            ctx->count = 0;  /* 部分データを破棄して保存をスキップさせる */
            return FALSE;
        }
        ctx->entries  = newBuf;
        ctx->capacity = newCap;
    }

    WindowEntry *e = &ctx->entries[ctx->count];
    /* パディングを含めゼロ初期化する。
     * realloc 後の領域は未初期化のため、構造体パディングがディスクに書き込まれる
     * と未初期化スタック・ヒープ内容がそのまま漏洩する。 */
    memset(e, 0, sizeof(*e));
    e->hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &e->pid);
    strncpy_s(e->className, sizeof(e->className), className, _TRUNCATE);
    e->placement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &e->placement)) {
        return TRUE;  /* 取得失敗（ウィンドウ破棄等）：このエントリをスキップ */
    }
    /* スクリーン座標を GetWindowRect で取得する。
     * placement.rcNormalPosition（ワークスペース座標）は WM_NCCALCSIZE カスタム実装の
     * アプリで SetWindowPlacement との非対称性が生じるため、復元には rect を使う。 */
    if (!GetWindowRect(hwnd, &e->rect)) {
        return TRUE;  /* 取得失敗（ウィンドウ破棄等）：このエントリをスキップ */
    }
    e->isTopmost = (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

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

    /* 最小化・最大化ウィンドウは通常サイズに復元してからモニタ判定を行う。
     * 最小化ウィンドウは (-32000, -32000) に配置されるため、MonitorFromWindow が
     * 最寄りモニタを返し、プライマリモニタとの判定が不正確になりうる。 */
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
     * （--restore の SetWindowPos が安定して動作するよう状態をクリーンにする） */
    ShowWindow(hwnd, SW_MINIMIZE);

    /* メッセージキュー安定化のためのウェイト */
    Sleep(5);

    return TRUE;
}

/*
 * --raise の EnumWindows コールバック
 *
 * 対象ウィンドウの HWND・appIndex・最小化状態を収集する。
 */
static BOOL CALLBACK raise_callback(HWND hwnd, LPARAM lParam)
{
    RaiseContext *ctx = (RaiseContext *)lParam;

    int appIndex;
    if (!is_raise_target(hwnd, ctx->myPid, &appIndex)) {
        return TRUE;
    }

    /* 容量不足時の倍増拡張（save_callback と同パターン） */
    if (ctx->count >= ctx->capacity) {
        int newCap = ctx->capacity == 0 ? 64 : ctx->capacity * 2;
        RaiseEntry *newBuf = (RaiseEntry *)realloc(ctx->entries, newCap * sizeof(RaiseEntry));
        if (newBuf == NULL) {
            ctx->count = 0;
            return FALSE;
        }
        ctx->entries  = newBuf;
        ctx->capacity = newCap;
    }

    RaiseEntry *e = &ctx->entries[ctx->count];
    e->hwnd      = hwnd;
    e->appIndex  = appIndex;
    e->enumIndex = ctx->count;
    e->iconic    = IsIconic(hwnd);

    ctx->count++;
    return TRUE;
}

/*
 * RaiseEntry の qsort 比較関数
 *
 * 主キー：appIndex 降順。（設定リスト末尾の exe を先頭に）
 * 副キー：enumIndex 昇順。（EnumWindows は前面→背面の順に列挙するため、
 *         昇順ソートで同一 exe 内も前面側から並ぶ）
 * ソート結果は「前面にすべき順」の並びになる。raise_windows() はこの並びの
 * 先頭から順に Z オーダーへチェーン挿入するため、appIndex・enumIndex とも
 * 元の重なり順を保ったまま、リスト末尾の exe が最終的に最前面になる。
 */
static int raise_compare(const void *a, const void *b)
{
    const RaiseEntry *ea = (const RaiseEntry *)a;
    const RaiseEntry *eb = (const RaiseEntry *)b;
    int cmp = (eb->appIndex > ea->appIndex) - (eb->appIndex < ea->appIndex);
    if (cmp != 0) return cmp;
    return (ea->enumIndex > eb->enumIndex) - (ea->enumIndex < eb->enumIndex);
}

/*
 * 設定ファイルで指定された exe のウィンドウを前面化する
 *
 * Phase 1: EnumWindows で対象ウィンドウを収集
 * Phase 2: raise_compare() で「前面にすべき順」にソートし、Z オーダーへ
 *          チェーン挿入で反映する。リスト後方の exe が最終的に最前面になる。
 *
 * チェーン挿入方式を採る理由：
 * SetWindowPos(HWND_TOP, ..., SWP_NOACTIVATE) は非アクティブ化を指定した
 * 呼び出しのため、Win32 の制約でフォアグラウンドウィンドウ（他アプリ）を
 * 追い越せない。単純に背面から HWND_TOP で積み上げる旧方式では、対象群は
 * 他アプリの直下にしか積まれず、末尾の SetForegroundWindow で 1 枚だけが
 * 前面化されて他の対象ウィンドウは他アプリの背後に取り残されていた。
 * 対策として、最前面にすべき 1 枚だけを SetForegroundWindow で確実に
 * フォアグラウンド化し、残りはその直後ろへ SetWindowPos(hwnd, prevHwnd, ...)
 * で順に連結挿入する。フォアグラウンド境界を跨ぐ操作が先頭の 1 回に限られる
 * ため、対象群全体が他アプリより手前に、相対順序を維持したまま並ぶ。
 */
static void raise_windows(DWORD myPid)
{
    RaiseContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.myPid = myPid;

    EnumWindows(raise_callback, (LPARAM)&ctx);

    if (ctx.count == 0) {
        free(ctx.entries);
        return;
    }

    qsort(ctx.entries, ctx.count, sizeof(RaiseEntry), raise_compare);

    /* 最小化解除を先に済ませる。SW_RESTORE は Z オーダーを乱すが、
     * 後続のチェーン挿入で並べ直すため問題ない */
    for (int i = 0; i < ctx.count; i++) {
        if (ctx.entries[i].iconic) {
            ShowWindow(ctx.entries[i].hwnd, SW_RESTORE);
            /* --restore の Sleep(50)（位置復元のアニメーション完了待ち）より
             * 短い 10ms で十分（Z オーダー変更のみで位置は書き戻さないため） */
            Sleep(10);
        }
    }

    /* 先頭エントリ（= 最前面にすべきウィンドウ）を SetForegroundWindow でフォアグラウンド化する。
     * 失敗時（他プロセスがフォアグラウンドロックを握っている等）は、後続の
     * SetWindowPos(HWND_TOP, ..., SWP_NOACTIVATE) も同一の Win32 制約（非アクティブ呼び出しは
     * フォアグラウンドウィンドウを追い越せない）を受けるため、この呼び出しでは前面化されない。
     * 有効な代替手段はなく、その場合は対象群内部の相対順序（後続のチェーン挿入）のみ機能する */
    SetForegroundWindow(ctx.entries[0].hwnd);
    if (GetForegroundWindow() == ctx.entries[0].hwnd) {
        SetWindowPos(ctx.entries[0].hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    Sleep(5);

    /* 2 枚目以降は直前のウィンドウの直後ろへ連結挿入し、相対順序を維持する */
    for (int i = 1; i < ctx.count; i++) {
        SetWindowPos(ctx.entries[i].hwnd, ctx.entries[i - 1].hwnd, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Sleep(5);
    }

    free(ctx.entries);
}

/* FILETIME の単位換算
 * FILETIME は 100ns 単位のカウンタ */
#define FILETIME_UNITS_PER_MS  10000ULL    /* 1 ミリ秒 = 10,000 × 100ns */
#define FILETIME_UNITS_PER_SEC 10000000ULL /* 1 秒 = 10,000,000 × 100ns */

/* ブート時刻判定の許容誤差（FILETIME 単位：2 秒） */
#define BOOT_TIME_TOLERANCE (2ULL * FILETIME_UNITS_PER_SEC)

/*
 * 保存ファイルが前回ブートのものか判定する
 *
 * ファイルの更新日時がシステム起動時刻より古い場合 TRUE を返す。
 * 起動時刻は GetSystemTimeAsFileTime() - GetTickCount64() で算出する。
 * 判定失敗時は FALSE（非 stale）を返し、復元を試みる安全側に倒す。
 */
static BOOL is_save_file_stale(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fileInfo)) {
        return FALSE;
    }

    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);
    ULONGLONG uptimeMs = GetTickCount64();

    ULARGE_INTEGER now;
    now.LowPart  = ftNow.dwLowDateTime;
    now.HighPart = ftNow.dwHighDateTime;

    /* 時刻異常時の安全側フォールバック
     * RTC バッテリ切れ等で system time が小さい、
     * またはアップタイム × FILETIME 単位がオーバーフローする場合に
     * ブート時刻を算出できないため stale 判定を無効化する。 */
    ULONGLONG uptime100ns;
    if (uptimeMs > ULLONG_MAX / FILETIME_UNITS_PER_MS) {
        return FALSE;
    }
    uptime100ns = uptimeMs * FILETIME_UNITS_PER_MS;
    if (now.QuadPart < uptime100ns) {
        return FALSE;
    }
    ULONGLONG bootTime = now.QuadPart - uptime100ns;

    /* 許容誤差を適用 */
    if (bootTime > BOOT_TIME_TOLERANCE) {
        bootTime -= BOOT_TIME_TOLERANCE;
    }

    ULARGE_INTEGER fileMtime;
    fileMtime.LowPart  = fileInfo.ftLastWriteTime.dwLowDateTime;
    fileMtime.HighPart = fileInfo.ftLastWriteTime.dwHighDateTime;

    return fileMtime.QuadPart < bootTime;
}

/*
 * ウィンドウ配置を保存する
 *
 * 保存先：exe と同じディレクトリの winfocus.dat
 */
static void save_positions(DWORD myPid)
{
    char savePath[MAX_PATH];
    if (!get_save_path(savePath, sizeof(savePath))) {
        return;
    }

    SaveContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.myPid = myPid;

    EnumWindows(save_callback, (LPARAM)&ctx);

    /* 対象ウィンドウなし時の残存ファイル削除
     * 前回保存時のデータが古い状態で残ることを防ぐため、無条件に削除を試みる。
     * ファイルが存在しなければ DeleteFileA は失敗するだけで副作用はない。 */
    if (ctx.count == 0) {
        DeleteFileA(savePath);
        free(ctx.entries);
        return;
    }

    FILE *fp = fopen(savePath, "wb");
    if (fp == NULL) {
        free(ctx.entries);
        return;
    }

    DWORD magic   = SAVE_FILE_MAGIC;
    DWORD version = SAVE_FILE_VERSION;
    /* 書き込み・クローズ失敗時は不完全ファイルを削除
     * fclose の戻り値も検査することで、バッファリングされた書き込みが
     * fclose 時に実体化する際の失敗（ディスクフル直前等）を検出する。 */
    BOOL ok = (fwrite(&magic,     sizeof(DWORD), 1, fp) == 1) &&
              (fwrite(&version,   sizeof(DWORD), 1, fp) == 1) &&
              (fwrite(&ctx.count, sizeof(int),   1, fp) == 1) &&
              ((size_t)fwrite(ctx.entries, sizeof(WindowEntry), ctx.count, fp) ==
               (size_t)ctx.count);
    if (fclose(fp) != 0) {
        ok = FALSE;
    }
    if (!ok) {
        DeleteFileA(savePath);
    }

    free(ctx.entries);
}

/*
 * 画面外ウィンドウの補正
 *
 * SetWindowPos 後にウィンドウが全モニタの範囲外にある場合、
 * プライマリモニタの作業領域左上に移動する。
 */
static void correct_offscreen(HWND hwnd)
{
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != NULL) {
        return;
    }
    RECT workArea;
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0)) {
        return;
    }
    SetWindowPos(hwnd, NULL, workArea.left, workArea.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/*
 * 保存ファイルが有効期限を過ぎたか判定する
 *
 * ファイル更新日時が現在時刻から g_save_file_expiry_hours 時間以上古い場合 TRUE を返す。
 * g_save_file_expiry_hours が 0 の場合は判定を無効化し常に FALSE を返す。
 * 取得失敗時は FALSE を返し、復元を試みる安全側に倒す。
 * 有効期限が 100ns 単位へ換算できない巨大値の場合も判定を無効化し FALSE を返す。
 */
static BOOL is_save_file_expired(const char *path)
{
    /* 0 は有効期限判定の無効化を意味する */
    if (g_save_file_expiry_hours == 0) {
        return FALSE;
    }

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fileInfo)) {
        return FALSE;
    }

    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);

    ULARGE_INTEGER now;
    now.LowPart  = ftNow.dwLowDateTime;
    now.HighPart = ftNow.dwHighDateTime;

    ULARGE_INTEGER fileMtime;
    fileMtime.LowPart  = fileInfo.ftLastWriteTime.dwLowDateTime;
    fileMtime.HighPart = fileInfo.ftLastWriteTime.dwHighDateTime;

    /* 未来の更新日時は異常値：expired 扱いにしない */
    if (fileMtime.QuadPart >= now.QuadPart) {
        return FALSE;
    }

    /* 経過時間（100ns 単位）と有効期限（時間 × 3600 秒 × FILETIME_UNITS_PER_SEC）を比較 */
    ULONGLONG elapsed     = now.QuadPart - fileMtime.QuadPart;

    /* 換算オーバーフロー時の安全側フォールバック
     * 有効期限を 100ns 単位へ換算するとラップする巨大値は、
     * 極端に短い期限として誤判定されるため判定を無効化する。 */
    if ((ULONGLONG)g_save_file_expiry_hours >
        ULLONG_MAX / (3600ULL * FILETIME_UNITS_PER_SEC)) {
        return FALSE;
    }
    ULONGLONG expiry100ns =(ULONGLONG)g_save_file_expiry_hours * 3600ULL * FILETIME_UNITS_PER_SEC;

    return elapsed > expiry100ns;
}

/*
 * 保存したウィンドウ配置を復元する
 *
 * HWND・PID・クラス名が一致するウィンドウに保存座標を SetWindowPos で適用する。
 * 最小化状態のみ rcNormalPosition 復元のため SetWindowPlacement を併用する。
 * 正常な復元ではファイルを削除しないため、何度でも復元できる。
 * stale 判定（前回ブートのデータ）の場合は削除して終了する。
 * 有効期限切れの場合は削除せず、ファイル不在として扱って終了する。
 */
static void restore_positions(void)
{
    char savePath[MAX_PATH];
    if (!get_save_path(savePath, sizeof(savePath))) {
        return;
    }

    /* 前回ブートの保存ファイルは無効：削除して終了 */
    if (is_save_file_stale(savePath)) {
        DeleteFileA(savePath);
        return;
    }

    /* 有効期限切れは削除せず、ファイルが存在しないものとして扱う */
    if (is_save_file_expired(savePath)) {
        return;
    }

    FILE *fp = fopen(savePath, "rb");
    if (fp == NULL) {
        return;
    }

    /* フォーマット検証：旧形式や破損ファイルは削除して終了する。
     * 旧形式は WindowEntry のレイアウトが異なり正しく復元できないため、
     * サイレント無視より削除して再保存を促す方が安全側に倒した動作となる。 */
    DWORD magic = 0, version = 0;
    if (fread(&magic,   sizeof(DWORD), 1, fp) != 1 ||
        fread(&version, sizeof(DWORD), 1, fp) != 1 ||
        magic != SAVE_FILE_MAGIC || version != SAVE_FILE_VERSION) {
        fclose(fp);
        DeleteFileA(savePath);
        return;
    }

    int count = 0;
    if (fread(&count, sizeof(int), 1, fp) != 1 || count <= 0 || count > MAX_RESTORE_ENTRIES) {
        fclose(fp);
        return;
    }

    WindowEntry *entries = (WindowEntry *)malloc(count * sizeof(WindowEntry));
    if (entries == NULL) {
        fclose(fp);
        return;
    }

    if (fread(entries, sizeof(WindowEntry), count, fp) != (size_t)count) {
        free(entries);
        fclose(fp);
        return;
    }
    fclose(fp);

    /* Z オーダー復元のための逆順適用
     * EnumWindows は前面→背面の順で列挙するため、保存配列を末尾から処理することで
     * 背面→前面の順に SetWindowPos を呼び出して保存時の重なり順を再現する。 */
    for (int i = count - 1; i >= 0; i--) {
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

        /* 位置・表示状態（最大化・最小化含む）を復元する。
         *
         * 位置・サイズは SetWindowPlacement.rcNormalPosition（ワークスペース座標）ではなく
         * 保存時の GetWindowRect（スクリーン座標）を SetWindowPos で適用する。
         * カスタムタイトルバー（WM_NCCALCSIZE フック）を持つアプリ（WindowsTerminal 等）では
         * placement のラウンドトリップが冪等にならないため、rect で位置精度を保証する。
         */
        RECT *r = &e->rect;
        int   w = r->right  - r->left;
        int   h = r->bottom - r->top;

        if (e->placement.showCmd == SW_SHOWMAXIMIZED) {
            /* SW_SHOWMAXIMIZED：先に保存座標のモニタへ移動してから最大化する 2 ステップ */
            /* 最小化中は SetWindowPos の位置指定が効かず、最小化直前の位置で
             * 最大化されるため、先に最小化を解除してから移動する。
             * 引数なし実行は全対象を最小化して終わるため、直後の --restore では
             * 対象がほぼ必ず最小化状態にある。 */
            if (IsIconic(e->hwnd)) {
                ShowWindow(e->hwnd, SW_RESTORE);
                Sleep(50);  /* 復元完了待ち（アニメーション込み） */
            }
            SetWindowPos(e->hwnd, NULL, r->left, r->top, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            correct_offscreen(e->hwnd);
            Sleep(10);
            ShowWindow(e->hwnd, SW_MAXIMIZE);
        }
        else if (e->placement.showCmd == SW_SHOWMINIMIZED ||
                 e->placement.showCmd == SW_SHOWMINNOACTIVE) {
            /* 最小化状態の復元
             * 最小化時の GetWindowRect は (-32000, -32000) を返すため、通常時位置は
             * rcNormalPosition から SetWindowPlacement で復元する。SW_SHOWMINNOACTIVE は
             * 非アクティブ最小化を示すが、通常時位置の取り扱いは SW_SHOWMINIMIZED と同等のため
             * 同一分岐で処理する。 */
            SetWindowPlacement(e->hwnd, &e->placement);
            ShowWindow(e->hwnd, SW_MINIMIZE);
        }
        else {
            /* SW_SHOWNORMAL 等：スクリーン座標で直接配置。冪等性を保証する。
             * 最小化中のウィンドウに SetWindowPos のみを呼んでも最小化は解除されず、
             * カスタムタイトルバーを持つアプリ（WindowsTerminal 等）では
             * 後続の手動復元時に位置がずれる。先に SW_RESTORE してから位置を書き直す。 */
            if (IsIconic(e->hwnd)) {
                ShowWindow(e->hwnd, SW_RESTORE);
                Sleep(50);  /* 復元完了待ち（アニメーション込み） */
            }
            SetWindowPos(e->hwnd, NULL, r->left, r->top, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            correct_offscreen(e->hwnd);
        }

        /* Z オーダーの復元（ベストエフォート）
         * TOPMOST ウィンドウは HWND_TOPMOST、それ以外は HWND_TOP で前面寄せする。
         * 逆順ループとの組み合わせにより保存時の重なり順をおおよそ再現する。 */
        HWND insertAfter = e->isTopmost ? HWND_TOPMOST : HWND_TOP;
        SetWindowPos(e->hwnd, insertAfter, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        Sleep(5);
    }

    free(entries);
}

/*
 * 設定ファイル（winfocus.toml）を読み込む
 *
 * exe と同じディレクトリの winfocus.toml から以下を読み込む。
 *   [toolwindow_whitelist].classes → g_whitelist
 *   [save_file].expiry_hours       → g_save_file_expiry_hours
 *   [raise].apps                   → g_raise_apps
 * ファイルが存在しない場合や値が不正な場合は各デフォルト値で動作する。
 */
static void load_config(void)
{
    char exeDir[MAX_PATH];
    if (!get_exe_dir(exeDir, sizeof(exeDir))) {
        return;
    }

    char configPath[MAX_PATH];
    int written = snprintf(configPath, sizeof(configPath), "%swinfocus.toml", exeDir);
    if (written <= 0 || (DWORD)written >= sizeof(configPath)) {
        return;
    }

    FILE *fp = fopen(configPath, "r");
    if (fp == NULL) {
        return;
    }

    int in_section = 0;
    /* 1 行の最大長
     * 配列は 1 行記述のみ対応するため、宣言容量の上限（32 エントリ × 255 文字 +
     * クォートとカンマ + キー部）を 1 行に収める必要がある。
     * 1024 バイトでは上限近くの設定が fgets で分断され、後半が黙って失われた。 */
    char line[8192];
    BOOL firstLine = TRUE;

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* UTF-8 BOM のスキップ
         * メモ帳等で UTF-8 BOM 付き保存された winfocus.toml の先頭セクションが
         * EF BB BF に阻まれて認識されない問題を回避する。先頭行から BOM を取り除き、
         * 以降の処理を BOM なし入力と等価に扱う。 */
        if (firstLine) {
            firstLine = FALSE;
            if ((unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                memmove(line, line + 3, strlen(line + 3) + 1);
            }
        }

        /* インラインコメント除去
         * ダブルクォート外の '#' 以降を切り捨てる。
         * これがないと `expiry_hours = 24 # コメント` で strtol の endptr 検査が
         * 失敗してデフォルト値に静かにフォールバックする。 */
        BOOL in_quote = FALSE;
        for (int i = 0; line[i] != '\0'; i++) {
            if (line[i] == '"') {
                in_quote = !in_quote;
            }
            else if (line[i] == '#' && !in_quote) {
                line[i] = '\0';
                break;
            }
        }

        /* 末尾の空白・改行を除去 */
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                            line[len - 1] == ' '  || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }

        /* 行頭の空白・タブを除去
         * TOML はセクションヘッダやキーのインデントを許すため、詰めずに扱うと
         * セクション判定（line[0] == '['）とキー比較が空白込みで失敗し、
         * 設定が黙ってデフォルト値にフォールバックする。 */
        int indent = 0;
        while (line[indent] == ' ' || line[indent] == '\t') {
            indent++;
        }
        if (indent > 0) {
            memmove(line, line + indent, strlen(line + indent) + 1);
        }

        if (line[0] == '\0') {
            continue;
        }

        /* セクションヘッダ */
        if (line[0] == '[') {
            if (_stricmp(line, "[toolwindow_whitelist]") == 0) {
                in_section = SECTION_TOOLWINDOW_WL;
            }
            else if (_stricmp(line, "[save_file]") == 0) {
                in_section = SECTION_SAVE_FILE;
            }
            else if (_stricmp(line, "[raise]") == 0) {
                in_section = SECTION_RAISE;
                g_raise_app_count = 0;  /* デフォルト値をクリアして設定値で上書き */
            }
            else {
                in_section = SECTION_NONE;
            }
            continue;
        }

        if (in_section == SECTION_NONE) {
            continue;
        }

        /* key = value の分離 */
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }

        /* キー部分を末尾空白トリムして取得 */
        char key[64] = {0};
        int keyLen = (int)(eq - line);
        while (keyLen > 0 && (line[keyLen - 1] == ' ' || line[keyLen - 1] == '\t')) {
            keyLen--;
        }
        if (keyLen <= 0 || keyLen >= (int)sizeof(key)) {
            continue;
        }
        strncpy_s(key, sizeof(key), line, keyLen);

        /* 値部分の先頭空白をスキップ */
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') {
            val++;
        }

        /* [toolwindow_whitelist] セクション */
        if (in_section == SECTION_TOOLWINDOW_WL && _stricmp(key, "classes") == 0) {
            char *p = val;
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

        /* [save_file] セクション
         * 0 は有効期限判定無効を意味する。
         * 非数値・負値・末尾余剰文字・INT_MAX 超過はデフォルト値維持 */
        if (in_section == SECTION_SAVE_FILE && _stricmp(key, "expiry_hours") == 0) {
            char *endptr;
            /* strtol は範囲外値で errno=ERANGE を立てつつ LONG_MAX を返し、endptr は終端を指す。
             * errno を検査しないと、INT_MAX 超過の指定が INT_MAX として受理される。 */
            errno = 0;
            long v = strtol(val, &endptr, 10);
            if (errno == 0 && endptr != val && *endptr == '\0' && v >= 0 && v <= INT_MAX) {
                g_save_file_expiry_hours = (int)v;
            }
        }

        /* [raise] セクション
         * apps 配列のパースは [toolwindow_whitelist].classes と同パターン */
        if (in_section == SECTION_RAISE && _stricmp(key, "apps") == 0) {
            char *p = val;
            while (*p && g_raise_app_count < MAX_RAISE_APPS) {
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
                    strncpy_s(g_raise_apps[g_raise_app_count],
                              sizeof(g_raise_apps[g_raise_app_count]),
                              start, entryLen);
                    g_raise_app_count++;
                }
                p++;  /* 閉じクォートをスキップ */
            }
        }

    }

    fclose(fp);
}

int main(int argc, char *argv[])
{
    /* --raise のデフォルト値（load_config で [raise] セクション検出時にクリアされる） */
    strncpy_s(g_raise_apps[0], sizeof(g_raise_apps[0]),
              "WindowsTerminal.exe", _TRUNCATE);
    g_raise_app_count = 1;

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

    if (arg1 && _stricmp(arg1, "--raise") == 0) {
        raise_windows(myPid);
        return 0;
    }

    if (arg1) {
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
