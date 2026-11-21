// crx2dng_gui - Native Win32 GUI for Canon EOS M50 CRX -> DaVinci Resolve CinemaDNG
// Zero external dependencies, Drag & Drop, multi-clip batch queue, live FPS/MB/s telemetry.

#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "converter_core.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

enum ControlIds
{
    IDC_LIST_CLIPS   = 1001,
    IDC_BTN_ADD_FILE = 1002,
    IDC_BTN_ADD_DIR  = 1003,
    IDC_BTN_REMOVE   = 1004,
    IDC_BTN_CLEAR    = 1005,
    IDC_EDIT_OUTDIR  = 1006,
    IDC_BTN_BROWSE   = 1007,
    IDC_CHK_PACK     = 1008,
    IDC_CHK_FULL     = 1009,
    IDC_CHK_FILLDROP = 1015,
    IDC_CMB_THREADS  = 1010,
    IDC_PROGRESS     = 1011,
    IDC_LBL_STATUS   = 1012,
    IDC_BTN_CONVERT  = 1013,
    IDC_BTN_OPEN_OUT = 1014
};

static constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
static constexpr UINT WM_APP_CLIP_STATUS = WM_APP + 2;
static constexpr UINT WM_APP_ALL_DONE = WM_APP + 3;

struct QueueItem
{
    crx_engine::ClipInfo info;
    std::wstring status = L"Ready";
    fs::path last_out_dir;
};

struct AppState
{
    HWND hwnd = nullptr;
    HWND hList = nullptr;
    HWND hOutEdit = nullptr;
    HWND hChkPack = nullptr;
    HWND hChkFull = nullptr;
    HWND hChkFillDrop = nullptr;
    HWND hCmbThreads = nullptr;
    HWND hProgress = nullptr;
    HWND hStatus = nullptr;
    HWND hBtnConvert = nullptr;
    HWND hBtnOpenOut = nullptr;
    HFONT hFont = nullptr;
    HFONT hBoldFont = nullptr;

    std::vector<QueueItem> items;
    std::mutex status_mtx;
    std::wstring live_status;
    int live_pct = 0;

    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::thread worker_thread;
    fs::path last_completed_dir;
};

static AppState g_app;

static std::wstring utf8_to_wide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static void set_item_status(int idx, const std::wstring &text)
{
    if (idx < 0 || idx >= (int)g_app.items.size()) return;
    g_app.items[idx].status = text;
    LVITEMW lvi{};
    lvi.iSubItem = 4;
    lvi.pszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(g_app.hList, LVM_SETITEMTEXTW, idx, reinterpret_cast<LPARAM>(&lvi));
}

static void refresh_list()
{
    ListView_DeleteAllItems(g_app.hList);
    for (int i = 0; i < (int)g_app.items.size(); ++i)
    {
        const auto &it = g_app.items[i];
        const auto &h = it.info.hdr;

        std::wstring name = it.info.first_file.filename().wstring();
        if (it.info.chunks.size() > 1)
            name += L" (+" + std::to_wstring(it.info.chunks.size() - 1) + L")";

        wchar_t res[64];
        swprintf(res, 64, L"%ux%u  %u-bit %s",
                 it.info.crop_w, it.info.crop_h, h.bits,
                 (h.mode == 4) ? L"C-RAW" : L"RAW");

        wchar_t frames[96];
        double fps = h.fps_x1000 / 1000.0;
        double dur = fps > 0.1 ? (it.info.timeline_frames / fps) : 0.0;
        if (it.info.dropped_frames > 0)
            swprintf(frames, 96, L"%zu (+%zu drops) @ %.2ffps", it.info.frames.size(), it.info.dropped_frames, fps);
        else
            swprintf(frames, 96, L"%zu  (%.1fs @ %.2ffps)", it.info.frames.size(), dur, fps);

        wchar_t sz[32];
        swprintf(sz, 32, L"%.1f MB", it.info.total_in_bytes / 1048576.0);

        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.pszText = const_cast<LPWSTR>(name.c_str());
        ListView_InsertItem(g_app.hList, &lvi);
        ListView_SetItemText(g_app.hList, i, 1, res);
        ListView_SetItemText(g_app.hList, i, 2, frames);
        ListView_SetItemText(g_app.hList, i, 3, sz);
        ListView_SetItemText(g_app.hList, i, 4, const_cast<LPWSTR>(it.status.c_str()));
    }
}

static bool has_crx_extension(const fs::path &p)
{
    std::wstring ext = p.extension().wstring();
    for (auto &c : ext) c = towupper(c);
    return ext == L".CRX";
}

static void add_crx_path(const fs::path &p)
{
    if (fs::is_directory(p))
    {
        std::vector<fs::path> found;
        for (const auto &entry : fs::directory_iterator(p))
        {
            if (entry.is_regular_file() && has_crx_extension(entry.path()))
                found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        for (const auto &f : found) add_crx_path(f);
        return;
    }

    if (!has_crx_extension(p)) return;
    for (const auto &existing : g_app.items)
        if (existing.info.first_file == p) return;

    QueueItem qi;
    std::string err;
    if (crx_engine::inspect_crx_clip(p, qi.info, err))
    {
        g_app.items.push_back(std::move(qi));
    }
    else
    {
        std::wstring msg = L"Skipped " + p.filename().wstring() + L": " + utf8_to_wide(err);
        SetWindowTextW(g_app.hStatus, msg.c_str());
    }
}

static void on_add_files()
{
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.hwnd;
    ofn.lpstrFilter = L"Canon ML CRX Movies (*.CRX)\0*.CRX\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return;

    const wchar_t *p = buf.data();
    std::wstring dir = p;
    p += dir.size() + 1;
    if (*p == 0)
    {
        add_crx_path(fs::path(dir));
    }
    else
    {
        while (*p)
        {
            std::wstring fn = p;
            add_crx_path(fs::path(dir) / fn);
            p += fn.size() + 1;
        }
    }
    refresh_list();
    if (!g_app.items.empty())
    {
        wchar_t msg[128];
        swprintf(msg, 128, L"%zu clip(s) queued — 100%% lossless sensor RAW to CinemaDNG.", g_app.items.size());
        SetWindowTextW(g_app.hStatus, msg);
    }
}

static bool browse_folder(const wchar_t *title, std::wstring &out_path)
{
    IFileDialog *pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (FAILED(hr) || !pfd) return false;
    DWORD opts = 0;
    pfd->GetOptions(&opts);
    pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    pfd->SetTitle(title);
    bool ok = false;
    if (SUCCEEDED(pfd->Show(g_app.hwnd)))
    {
        IShellItem *psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi)) && psi)
        {
            PWSTR pszPath = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
            {
                out_path = pszPath;
                CoTaskMemFree(pszPath);
                ok = true;
            }
            psi->Release();
        }
    }
    pfd->Release();
    return ok;
}

static void set_controls_enabled(bool enabled)
{
    EnableWindow(GetDlgItem(g_app.hwnd, IDC_BTN_ADD_FILE), enabled);
    EnableWindow(GetDlgItem(g_app.hwnd, IDC_BTN_ADD_DIR), enabled);
    EnableWindow(GetDlgItem(g_app.hwnd, IDC_BTN_REMOVE), enabled);
    EnableWindow(GetDlgItem(g_app.hwnd, IDC_BTN_CLEAR), enabled);
    EnableWindow(g_app.hOutEdit, enabled);
    EnableWindow(GetDlgItem(g_app.hwnd, IDC_BTN_BROWSE), enabled);
    EnableWindow(g_app.hChkPack, enabled);
    EnableWindow(g_app.hChkFull, enabled);
    EnableWindow(g_app.hChkFillDrop, enabled);
    EnableWindow(g_app.hCmbThreads, enabled);
    SetWindowTextW(g_app.hBtnConvert, enabled ? L"Convert to CinemaDNG" : L"Cancel");
}

static void start_conversion()
{
    if (g_app.items.empty())
    {
        MessageBoxW(g_app.hwnd, L"Add or drag-and-drop at least one .CRX clip first.", L"crx2dng", MB_ICONINFORMATION);
        return;
    }

    wchar_t out_buf[MAX_PATH * 2] = {};
    GetWindowTextW(g_app.hOutEdit, out_buf, MAX_PATH * 2);
    std::wstring custom_out = out_buf;
    bool pack_bits = (SendMessageW(g_app.hChkPack, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool full_frame = (SendMessageW(g_app.hChkFull, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool fill_drops = (SendMessageW(g_app.hChkFillDrop, BM_GETCHECK, 0, 0) == BST_CHECKED);

    int sel = (int)SendMessageW(g_app.hCmbThreads, CB_GETCURSEL, 0, 0);
    int threads = 0;
    if (sel > 0) threads = (int)SendMessageW(g_app.hCmbThreads, CB_GETITEMDATA, sel, 0);

    g_app.running = true;
    g_app.cancel = false;
    set_controls_enabled(false);
    SendMessageW(g_app.hProgress, PBM_SETPOS, 0, 0);

    if (g_app.worker_thread.joinable()) g_app.worker_thread.join();

    g_app.worker_thread = std::thread([custom_out, pack_bits, full_frame, fill_drops, threads]() {
        size_t total_clips = g_app.items.size();
        size_t total_frames_all = 0;
        for (const auto &it : g_app.items) total_frames_all += it.info.frames.size();
        size_t frames_before = 0;

        for (size_t i = 0; i < total_clips && !g_app.cancel.load(); ++i)
        {
            const auto &clip = g_app.items[i].info;
            fs::path out_dir = custom_out.empty()
                ? (clip.first_file.parent_path() / (clip.first_file.stem().wstring() + L"_dng"))
                : (fs::path(custom_out) / (clip.first_file.stem().wstring() + L"_dng"));

            g_app.items[i].last_out_dir = out_dir;

            crx_engine::ConvertOptions opt{};
            opt.output_dir = out_dir;
            opt.pack_bits = pack_bits;
            opt.full_frame = full_frame;
            opt.fill_drops = fill_drops;
            opt.threads = threads;

            auto *msg_start = new std::wstring(L"Converting...");
            PostMessageW(g_app.hwnd, WM_APP_CLIP_STATUS, (WPARAM)i, reinterpret_cast<LPARAM>(msg_start));

            crx_engine::ProgressInfo stats{};
            std::string err;
            bool ok = crx_engine::convert_crx_clip(
                clip, opt, g_app.cancel,
                [&](const crx_engine::ProgressInfo &pi) {
                    size_t overall_done = frames_before + pi.done;
                    int pct = total_frames_all ? (int)((overall_done * 1000) / total_frames_all) : 0;
                    wchar_t st[256];
                    swprintf(st, 256,
                             L"Clip %zu/%zu (%s): %zu/%zu frames  |  %.1f fps  |  %.1f MB/s",
                             i + 1, total_clips, clip.first_file.filename().c_str(),
                             pi.done, pi.total, pi.fps, pi.mb_per_sec);
                    {
                        std::lock_guard<std::mutex> lk(g_app.status_mtx);
                        g_app.live_status = st;
                        g_app.live_pct = pct;
                    }
                    PostMessageW(g_app.hwnd, WM_APP_PROGRESS, 0, 0);

                    wchar_t cs[64];
                    int cpct = pi.total ? (int)((pi.done * 100) / pi.total) : 0;
                    swprintf(cs, 64, L"%d%% (%.1f fps)", cpct, pi.fps);
                    PostMessageW(g_app.hwnd, WM_APP_CLIP_STATUS, (WPARAM)i,
                                 reinterpret_cast<LPARAM>(new std::wstring(cs)));
                },
                stats, err);

            frames_before += clip.frames.size();
            if (ok)
            {
                g_app.last_completed_dir = out_dir;
                wchar_t done_s[64];
                swprintf(done_s, 64, L"Done (%.1f fps)", stats.fps);
                PostMessageW(g_app.hwnd, WM_APP_CLIP_STATUS, (WPARAM)i,
                             reinterpret_cast<LPARAM>(new std::wstring(done_s)));
            }
            else
            {
                std::wstring es = g_app.cancel.load() ? L"Cancelled" : (L"Error: " + utf8_to_wide(err));
                PostMessageW(g_app.hwnd, WM_APP_CLIP_STATUS, (WPARAM)i,
                             reinterpret_cast<LPARAM>(new std::wstring(es)));
            }
        }

        PostMessageW(g_app.hwnd, WM_APP_ALL_DONE, g_app.cancel.load() ? 1 : 0, 0);
    });
}

static void layout_controls(int W, int H)
{
    const int pad = 12;
    const int top_btn_h = 28;
    const int bottom_h = 178;
    const int list_y = pad + top_btn_h + 8;
    const int list_h = std::max(120, H - list_y - bottom_h);

    MoveWindow(GetDlgItem(g_app.hwnd, IDC_BTN_ADD_FILE), pad, pad, 130, top_btn_h, TRUE);
    MoveWindow(GetDlgItem(g_app.hwnd, IDC_BTN_ADD_DIR),  pad + 138, pad, 130, top_btn_h, TRUE);
    MoveWindow(GetDlgItem(g_app.hwnd, IDC_BTN_REMOVE),   pad + 276, pad, 95, top_btn_h, TRUE);
    MoveWindow(GetDlgItem(g_app.hwnd, IDC_BTN_CLEAR),    pad + 379, pad, 85, top_btn_h, TRUE);

    MoveWindow(g_app.hList, pad, list_y, W - 2 * pad, list_h, TRUE);

    int y = list_y + list_h + 10;
    MoveWindow(GetDlgItem(g_app.hwnd, 2001), pad, y + 4, 95, 22, TRUE);
    MoveWindow(g_app.hOutEdit, pad + 98, y, W - 2 * pad - 98 - 92, 24, TRUE);
    MoveWindow(GetDlgItem(g_app.hwnd, IDC_BTN_BROWSE), W - pad - 86, y, 86, 24, TRUE);

    y += 32;
    MoveWindow(g_app.hChkPack, pad, y, 310, 22, TRUE);
    MoveWindow(g_app.hChkFull, pad + 320, y, 220, 22, TRUE);
    MoveWindow(GetDlgItem(g_app.hwnd, 2002), W - pad - 200, y + 3, 60, 20, TRUE);
    MoveWindow(g_app.hCmbThreads, W - pad - 135, y - 1, 135, 160, TRUE);

    y += 26;
    MoveWindow(g_app.hChkFillDrop, pad, y, 420, 22, TRUE);

    y += 30;
    MoveWindow(g_app.hProgress, pad, y, W - 2 * pad, 20, TRUE);

    y += 26;
    MoveWindow(g_app.hStatus, pad, y, W - 2 * pad, 20, TRUE);

    y += 24;
    MoveWindow(g_app.hBtnConvert, W - pad - 185, y, 185, 32, TRUE);
    MoveWindow(g_app.hBtnOpenOut, W - pad - 185 - 150 - 8, y, 150, 32, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_app.hwnd = hwnd;
        DragAcceptFiles(hwnd, TRUE);

        g_app.hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_app.hBoldFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        auto mk = [&](const wchar_t *cls, const wchar_t *txt, DWORD style, int id, HFONT f = nullptr) {
            HWND h = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                     0, 0, 10, 10, hwnd, (HMENU)(intptr_t)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)(f ? f : g_app.hFont), TRUE);
            return h;
        };

        mk(L"BUTTON", L"+ Add .CRX Files...", BS_PUSHBUTTON, IDC_BTN_ADD_FILE);
        mk(L"BUTTON", L"+ Add Folder...",     BS_PUSHBUTTON, IDC_BTN_ADD_DIR);
        mk(L"BUTTON", L"Remove",              BS_PUSHBUTTON, IDC_BTN_REMOVE);
        mk(L"BUTTON", L"Clear All",           BS_PUSHBUTTON, IDC_BTN_CLEAR);

        g_app.hList = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, 0, 10, 10, hwnd, (HMENU)IDC_LIST_CLIPS, nullptr, nullptr);
        SendMessageW(g_app.hList, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        ListView_SetExtendedListViewStyle(g_app.hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

        const struct { const wchar_t *title; int w; } cols[] = {
            { L"Clip", 170 }, { L"Format", 175 }, { L"Frames / FPS", 165 }, { L"Size", 85 }, { L"Status", 145 }
        };
        for (int i = 0; i < 5; ++i)
        {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            c.cx = cols[i].w;
            c.pszText = const_cast<LPWSTR>(cols[i].title);
            ListView_InsertColumn(g_app.hList, i, &c);
        }

        mk(L"STATIC", L"Output Folder:", SS_LEFT, 2001);
        g_app.hOutEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_OUTDIR, nullptr, nullptr);
        SendMessageW(g_app.hOutEdit, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
        SendMessageW(g_app.hOutEdit, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"(Default: <clip>_dng next to source .CRX)"));
        mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON, IDC_BTN_BROWSE);

        g_app.hChkPack = mk(L"BUTTON", L"12/14-bit Packed DNG (25% smaller, lossless)", BS_AUTOCHECKBOX, IDC_CHK_PACK);
        SendMessageW(g_app.hChkPack, BM_SETCHECK, BST_CHECKED, 0);
        g_app.hChkFull = mk(L"BUTTON", L"Keep uncropped sensor frame", BS_AUTOCHECKBOX, IDC_CHK_FULL);
        g_app.hChkFillDrop = mk(L"BUTTON", L"Fill dropped frames with hold-frames (keeps audio sync & 1 clip in Resolve)", BS_AUTOCHECKBOX, IDC_CHK_FILLDROP);
        SendMessageW(g_app.hChkFillDrop, BM_SETCHECK, BST_CHECKED, 0);

        mk(L"STATIC", L"Threads:", SS_RIGHT, 2002);
        g_app.hCmbThreads = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_CMB_THREADS);
        int hw = (int)std::thread::hardware_concurrency();
        if (hw <= 0) hw = 8;
        wchar_t auto_lbl[48];
        swprintf(auto_lbl, 48, L"Auto (%d threads)", hw);
        SendMessageW(g_app.hCmbThreads, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(auto_lbl));
        SendMessageW(g_app.hCmbThreads, CB_SETITEMDATA, 0, 0);
        int presets[] = { 32, 24, 16, 12, 8, 4 };
        for (int t : presets)
        {
            if (t <= hw)
            {
                wchar_t lb[32];
                swprintf(lb, 32, L"%d threads", t);
                LRESULT idx = SendMessageW(g_app.hCmbThreads, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lb));
                SendMessageW(g_app.hCmbThreads, CB_SETITEMDATA, idx, t);
            }
        }
        SendMessageW(g_app.hCmbThreads, CB_SETCURSEL, 0, 0);

        g_app.hProgress = CreateWindowExW(
            0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            0, 0, 10, 10, hwnd, (HMENU)IDC_PROGRESS, nullptr, nullptr);
        SendMessageW(g_app.hProgress, PBM_SETRANGE32, 0, 1000);

        g_app.hStatus = mk(L"STATIC", L"Drag & drop .CRX files or a folder here to convert to CinemaDNG for DaVinci Resolve.", SS_LEFT, IDC_LBL_STATUS);
        g_app.hBtnOpenOut = mk(L"BUTTON", L"Open Output Folder", BS_PUSHBUTTON, IDC_BTN_OPEN_OUT);
        EnableWindow(g_app.hBtnOpenOut, FALSE);
        g_app.hBtnConvert = mk(L"BUTTON", L"Convert to CinemaDNG", BS_DEFPUSHBUTTON, IDC_BTN_CONVERT, g_app.hBoldFont);

        RECT rc{};
        GetClientRect(hwnd, &rc);
        layout_controls(rc.right, rc.bottom);
        return 0;
    }

    case WM_SIZE:
        layout_controls(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_DROPFILES:
    {
        if (g_app.running.load()) return 0;
        HDROP hDrop = (HDROP)wp;
        UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i)
        {
            wchar_t path[MAX_PATH * 2];
            if (DragQueryFileW(hDrop, i, path, MAX_PATH * 2))
                add_crx_path(fs::path(path));
        }
        DragFinish(hDrop);
        refresh_list();
        if (!g_app.items.empty())
        {
            wchar_t msg[128];
            swprintf(msg, 128, L"%zu clip(s) queued — ready to convert.", g_app.items.size());
            SetWindowTextW(g_app.hStatus, msg);
        }
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == IDC_BTN_ADD_FILE) on_add_files();
        else if (id == IDC_BTN_ADD_DIR)
        {
            std::wstring dir;
            if (browse_folder(L"Select folder containing .CRX clips (e.g. H:\\ML\\CRX)", dir))
            {
                add_crx_path(fs::path(dir));
                refresh_list();
                wchar_t msg[128];
                swprintf(msg, 128, L"%zu clip(s) queued — ready to convert.", g_app.items.size());
                SetWindowTextW(g_app.hStatus, msg);
            }
        }
        else if (id == IDC_BTN_REMOVE)
        {
            std::vector<int> sel;
            int idx = -1;
            while ((idx = ListView_GetNextItem(g_app.hList, idx, LVNI_SELECTED)) != -1)
                sel.push_back(idx);
            for (int i = (int)sel.size() - 1; i >= 0; --i)
                g_app.items.erase(g_app.items.begin() + sel[i]);
            refresh_list();
        }
        else if (id == IDC_BTN_CLEAR)
        {
            g_app.items.clear();
            refresh_list();
            SetWindowTextW(g_app.hStatus, L"Queue cleared.");
        }
        else if (id == IDC_BTN_BROWSE)
        {
            std::wstring dir;
            if (browse_folder(L"Select output parent folder for CinemaDNG clips", dir))
                SetWindowTextW(g_app.hOutEdit, dir.c_str());
        }
        else if (id == IDC_BTN_CONVERT)
        {
            if (g_app.running.load())
            {
                g_app.cancel = true;
                SetWindowTextW(g_app.hStatus, L"Cancelling...");
            }
            else
            {
                start_conversion();
            }
        }
        else if (id == IDC_BTN_OPEN_OUT)
        {
            if (!g_app.last_completed_dir.empty() && fs::exists(g_app.last_completed_dir))
                ShellExecuteW(hwnd, L"open", g_app.last_completed_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    case WM_APP_PROGRESS:
    {
        std::wstring st;
        int pct = 0;
        {
            std::lock_guard<std::mutex> lk(g_app.status_mtx);
            st = g_app.live_status;
            pct = g_app.live_pct;
        }
        SendMessageW(g_app.hProgress, PBM_SETPOS, pct, 0);
        SetWindowTextW(g_app.hStatus, st.c_str());
        return 0;
    }

    case WM_APP_CLIP_STATUS:
    {
        auto *s = reinterpret_cast<std::wstring *>(lp);
        if (s)
        {
            set_item_status((int)wp, *s);
            delete s;
        }
        return 0;
    }

    case WM_APP_ALL_DONE:
    {
        g_app.running = false;
        set_controls_enabled(true);
        bool cancelled = (wp != 0);
        if (!cancelled)
        {
            SendMessageW(g_app.hProgress, PBM_SETPOS, 1000, 0);
            SetWindowTextW(g_app.hStatus, L"All clips converted! Import the _dng folder(s) directly into DaVinci Resolve.");
            if (!g_app.last_completed_dir.empty())
                EnableWindow(g_app.hBtnOpenOut, TRUE);
        }
        else
        {
            SetWindowTextW(g_app.hStatus, L"Conversion cancelled.");
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_app.running.load())
        {
            g_app.cancel = true;
            if (g_app.worker_thread.joinable()) g_app.worker_thread.join();
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_app.worker_thread.joinable()) g_app.worker_thread.join();
        if (g_app.hFont) DeleteObject(g_app.hFont);
        if (g_app.hBoldFont) DeleteObject(g_app.hBoldFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
    wc.lpszClassName = L"Crx2DngMainWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName,
        L"Canon EOS M50 CRX \x2192 CinemaDNG Converter (DaVinci Resolve)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 540,
        nullptr, nullptr, hInst, nullptr);

    // Support passing .CRX file(s) on command line / drag-drop onto exe
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
    {
        for (int i = 1; i < argc; ++i)
            add_crx_path(fs::path(argv[i]));
        refresh_list();
    }
    if (argv) LocalFree(argv);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
