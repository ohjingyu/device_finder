// build:
//   windres app.rc -O coff -o app_res.o
//   gcc -O2 -mwindows -static device_finder.c app_res.o -o device_finder.exe -liphlpapi -lws2_32 -lcomctl32
// 같은 망의 IP마다 80포트로 `GET /` 한 번만 보내 HTML 제목으로 펌웨어 기종을 판별한다 (읽기 전용).
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <ctype.h>

#define WM_FOUND (WM_APP + 1)
#define WM_TICK  (WM_APP + 2)
#define WM_DONE  (WM_APP + 3)
#define THREADS  64
#define ID_RESCAN 101
#define ID_OPENALL 102

// 기종 판별 규칙: exe 옆 devices.txt 에서 읽는다 ("첫 화면 HTML에 포함된 문자열 = 표시할 기기 이름")
#define MAX_KINDS 32 // shortcut: 규칙 최대 32개, 더 필요하면 값만 키우면 됨
static struct { char needle[128]; wchar_t name[64]; } kinds[MAX_KINDS];
static int nkinds;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void load_config(void) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *sl = wcsrchr(path, L'\\');
    if (sl) wcscpy(sl + 1, L"devices.txt");
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char line[512];
    while (nkinds < MAX_KINDS && fgets(line, sizeof line, f)) {
        char *p = line, *eq;
        if (!memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;  // UTF-8 BOM
        if (*p == '#' || !(eq = strchr(p, '='))) continue;
        *eq = 0;
        char *needle = trim(p), *name = trim(eq + 1);
        if (!*needle || !*name) continue;
        snprintf(kinds[nkinds].needle, sizeof kinds[0].needle, "%s", needle);
        MultiByteToWideChar(CP_UTF8, 0, name, -1, kinds[nkinds].name, 64);
        nkinds++;
    }
    fclose(f);
}

static HWND hwnd, htitle, hstatus, hbar, hlist, hrescan, hopenall;
static HFONT fnt, fnt_title;
static int dpi = 96;
static DWORD base;          // shortcut: /24 고정, 더 넓은 망은 일부만 검색됨
static char gwstr[16];
static volatile LONG next_ip, running, scanned;

#define S(x) MulDiv((x), dpi, 96)

// 기종 번호(kinds 인덱스+1), 해당 없으면 0
static int probe(DWORD ip) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(80); a.sin_addr.s_addr = ip;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    connect(s, (struct sockaddr *)&a, sizeof a);
    fd_set w; FD_ZERO(&w); FD_SET(s, &w);
    struct timeval tv = {1, 0};
    int kind = 0;
    if (select(0, NULL, &w, NULL, &tv) == 1) {
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        DWORD to = 1500; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
        const char req[] = "GET / HTTP/1.0\r\nHost: finder\r\nConnection: close\r\n\r\n";
        char buf[2049]; int n = 0, r;
        if (send(s, req, sizeof req - 1, 0) > 0)
            while (n < 2048 && !kind && (r = recv(s, buf + n, 2048 - n, 0)) > 0) {
                n += r; buf[n] = 0;
                for (int k = 0; k < nkinds; k++)
                    if (strstr(buf, kinds[k].needle)) { kind = k + 1; break; }
            }
    }
    closesocket(s);
    return kind;
}

static DWORD WINAPI worker(LPVOID p) {
    LONG i;
    while ((i = InterlockedIncrement(&next_ip)) <= 254) {
        int kind = probe(base | ((DWORD)i << 24));
        if (kind) PostMessage(hwnd, WM_FOUND, i, kind);
        PostMessage(hwnd, WM_TICK, 0, 0);
    }
    if (InterlockedDecrement(&running) == 0) PostMessage(hwnd, WM_DONE, 0, 0);
    return 0;
}

static int find_gateway(void) {
    ULONG len = 0;
    gwstr[0] = 0;
    GetAdaptersInfo(NULL, &len);
    IP_ADAPTER_INFO *a = malloc(len), *c;
    if (!a || GetAdaptersInfo(a, &len) != NO_ERROR) { free(a); return 0; }
    for (c = a; c; c = c->Next)
        if (strcmp(c->GatewayList.IpAddress.String, "0.0.0.0") && c->GatewayList.IpAddress.String[0]) {
            snprintf(gwstr, sizeof gwstr, "%s", c->GatewayList.IpAddress.String);
            break;
        }
    free(a);
    return c != NULL;
}

static void ip_text(wchar_t *t, size_t n, int last) {
    BYTE *b = (BYTE *)&base;
    swprintf(t, n, L"%d.%d.%d.%d", b[0], b[1], b[2], last);
}

static void open_site(int last) {
    wchar_t ip[32], url[48];
    ip_text(ip, 32, last);
    swprintf(url, 48, L"http://%ls", ip);
    // chrome이 없으면 기본 브라우저로
    if ((INT_PTR)ShellExecuteW(NULL, L"open", L"chrome", url, NULL, SW_SHOW) <= 32)
        ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOW);
}

static int item_last(int row) {
    LVITEMW it = {0};
    it.mask = LVIF_PARAM; it.iItem = row;
    SendMessageW(hlist, LVM_GETITEMW, 0, (LPARAM)&it);
    return (int)it.lParam;
}

static void add_item(int last, int kind) {
    wchar_t ip[32], url[48];
    int n = (int)SendMessageW(hlist, LVM_GETITEMCOUNT, 0, 0), at = n;
    for (int k = 0; k < n; k++)
        if (item_last(k) > last) { at = k; break; }
    ip_text(ip, 32, last);
    swprintf(url, 48, L"http://%ls", ip);
    LVITEMW it = {0};
    it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = at; it.pszText = ip; it.lParam = last;
    at = (int)SendMessageW(hlist, LVM_INSERTITEMW, 0, (LPARAM)&it);
    LVITEMW sub = {0};
    sub.iSubItem = 1; sub.pszText = kinds[kind - 1].name;
    SendMessageW(hlist, LVM_SETITEMTEXTW, at, (LPARAM)&sub);
    sub.iSubItem = 2; sub.pszText = url;
    SendMessageW(hlist, LVM_SETITEMTEXTW, at, (LPARAM)&sub);
}

static void layout(void) {
    RECT rc; GetClientRect(hwnd, &rc);
    int m = S(20), w = rc.right - 2 * m, bh = S(38);
    MoveWindow(htitle, m, S(16), w, S(34), TRUE);
    MoveWindow(hstatus, m, S(56), w, S(22), TRUE);
    MoveWindow(hbar, m, S(86), w, S(8), TRUE);
    MoveWindow(hlist, m, S(106), w, rc.bottom - S(106) - bh - 2 * m, TRUE);
    MoveWindow(hrescan, m, rc.bottom - bh - m, S(130), bh, TRUE);
    MoveWindow(hopenall, m + S(142), rc.bottom - bh - m, S(150), bh, TRUE);
    SendMessageW(hlist, LVM_SETCOLUMNWIDTH, 0, S(180));
    SendMessageW(hlist, LVM_SETCOLUMNWIDTH, 1, S(170));
    SendMessageW(hlist, LVM_SETCOLUMNWIDTH, 2, LVSCW_AUTOSIZE_USEHEADER);
}

static void start_scan(void) {
    SendMessageW(hlist, LVM_DELETEALLITEMS, 0, 0);
    SendMessage(hbar, PBM_SETPOS, 0, 0);
    if (!nkinds) {
        SetWindowTextW(hstatus, L"devices.txt가 없거나 비어 있습니다. devices.example.txt를 복사해서 만들어 주세요.");
        return;
    }
    if (!find_gateway()) {
        SetWindowTextW(hstatus, L"게이트웨이를 찾지 못했습니다. 네트워크 연결을 확인하세요.");
        return;
    }
    base = inet_addr(gwstr) & 0x00FFFFFF;
    scanned = 0; next_ip = 0; running = THREADS;
    EnableWindow(hrescan, FALSE);
    EnableWindow(hopenall, FALSE);
    for (int k = 0; k < THREADS; k++) CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
}

static void on_done(void) {
    wchar_t t[160];
    int n = (int)SendMessageW(hlist, LVM_GETITEMCOUNT, 0, 0);
    EnableWindow(hrescan, TRUE);
    EnableWindow(hopenall, n > 0);
    SendMessage(hbar, PBM_SETPOS, 254, 0);
    if (!n) {
        SetWindowTextW(hstatus, L"펌웨어가 설치된 기기를 찾지 못했습니다.");
        MessageBoxW(hwnd, L"펌웨어가 설치된 기기를 찾지 못했습니다.", L"검색 완료", MB_OK | MB_ICONINFORMATION);
        return;
    }
    swprintf(t, 160, L"검색 완료: 기기 %d대 발견 — IP를 클릭하면 사이트가 열립니다", n);
    SetWindowTextW(hstatus, t);
    swprintf(t, 160, L"기기 %d대를 찾았습니다.\n모든 기기의 사이트를 여시겠습니까?\n(아니요: 목록에서 IP를 클릭한 기기만 열기)", n);
    if (MessageBoxW(hwnd, t, L"검색 완료", MB_YESNO | MB_ICONQUESTION) == IDYES)
        for (int k = 0; k < n; k++) open_site(item_last(k));
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, HFONT f, HMENU id) {
    HWND h = CreateWindowW(cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd, id, GetModuleHandle(NULL), NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)f, TRUE);
    return h;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    wchar_t t[96];
    switch (m) {
    case WM_FOUND: add_item((int)w, (int)l); return 0;
    case WM_TICK:
        swprintf(t, 96, L"%hs 대역 검색 중...  %ld / 254", gwstr, ++scanned);
        SetWindowTextW(hstatus, t);
        SendMessage(hbar, PBM_SETPOS, scanned, 0);
        return 0;
    case WM_DONE: on_done(); return 0;
    case WM_CTLCOLORSTATIC: return (LRESULT)GetSysColorBrush(COLOR_WINDOW); // 라벨 배경을 창 색과 동일하게
    case WM_SIZE: if (hlist) layout(); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)l)->ptMinTrackSize = (POINT){S(560), S(420)}; return 0;
    case WM_COMMAND:
        if (LOWORD(w) == ID_RESCAN) start_scan();
        if (LOWORD(w) == ID_OPENALL) {
            int n = (int)SendMessageW(hlist, LVM_GETITEMCOUNT, 0, 0);
            for (int k = 0; k < n; k++) open_site(item_last(k));
        }
        return 0;
    case WM_NOTIFY: {
        NMITEMACTIVATE *a = (NMITEMACTIVATE *)l;
        if (a->hdr.hwndFrom == hlist && a->hdr.code == LVN_ITEMACTIVATE && a->iItem >= 0)
            open_site(item_last(a->iItem));
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    HDC dc = GetDC(NULL); dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc);
    INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&ic);
    fnt = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    fnt_title = CreateFontW(-S(24), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"DevFinder";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    hwnd = CreateWindowW(L"DevFinder", L"Device Finder", WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, S(860), S(620), NULL, NULL, hi, NULL);
    htitle = mk(L"STATIC", L"Device Finder", 0, fnt_title, NULL);
    hstatus = mk(L"STATIC", L"게이트웨이 확인 중...", 0, fnt, NULL);
    hbar = mk(PROGRESS_CLASSW, NULL, PBS_SMOOTH, fnt, NULL);
    SendMessage(hbar, PBM_SETRANGE32, 0, 254);
    hlist = mk(WC_LISTVIEWW, NULL, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, fnt, NULL);
    SendMessageW(hlist, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_ONECLICKACTIVATE | LVS_EX_UNDERLINEHOT | LVS_EX_DOUBLEBUFFER);
    static wchar_t *cols[] = {L"IP 주소", L"기종", L"접속 주소 (클릭하면 열림)"};
    for (int k = 0; k < 3; k++) {
        LVCOLUMNW c = {0}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = cols[k]; c.cx = S(150);
        SendMessageW(hlist, LVM_INSERTCOLUMNW, k, (LPARAM)&c);
    }
    hrescan = mk(L"BUTTON", L"다시 검색", BS_PUSHBUTTON | WS_TABSTOP, fnt, (HMENU)ID_RESCAN);
    hopenall = mk(L"BUTTON", L"모두 열기", BS_PUSHBUTTON | WS_TABSTOP, fnt, (HMENU)ID_OPENALL);
    layout();
    ShowWindow(hwnd, show);

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    load_config();
    start_scan();
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}
