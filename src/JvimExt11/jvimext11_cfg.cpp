/* vi:set ts=8 sts=4 sw=4 et:
 *
 * VIM - Vi IMproved    jvimext11 by Sylvain Cresto
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

/*
 * jvimext11_cfg.exe: the settings window of the Windows 11 context menu
 * entry.  It is also the application that the package manifest
 * (AppxManifest.xml.in) must declare; the context menu entry itself is
 * jvimext11.dll.
 *
 * Settings (see jvimext11.h):
 * - the jvim.exe to use, empty for the automatic detection;
 * - whether to open each file in a new tab page (default: yes).
 *
 * The window also shows the version of Vim, the license and the home page.
 */

#define PROJECT_URL     L"https://www.vim.org/"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>

#include "jvimext11.h"
#include "resource.h"
#include "../version.h"

#if VIM_VERSION_PATCHLEVEL > 0
# define VERSION_STRING L"" VIM_VERSION_SHORT "." \
                                        VIM_TOSTR(VIM_VERSION_PATCHLEVEL)
#else
# define VERSION_STRING L"" VIM_VERSION_SHORT
#endif

static HFONT    title_font;
static BOOL     status_error;

/*
 * Translated text "id" (string tables in the lang directory).
 */
    static std::wstring
tr(UINT id)
{
    return load_string(GetModuleHandleW(NULL), id);
}

/*
 * Content of the jvim.exe field without surrounding spaces and quotes.
 */
    static std::wstring
get_path_field(HWND dlg)
{
    WCHAR           buf[MAX_PATH * 4];
    std::wstring    path;

    GetDlgItemTextW(dlg, IDC_GVIM_PATH, buf, ARRAYSIZE(buf));
    path = buf;
    size_t first = path.find_first_not_of(L" \t\"");
    size_t last = path.find_last_not_of(L" \t\"");
    if (first == std::wstring::npos)
        return std::wstring();
    return path.substr(first, last - first + 1);
}

/*
 * Show which jvim.exe will be used.
 */
    static void
update_status(HWND dlg)
{
    std::wstring    path = get_path_field(dlg);
    std::wstring    status;

    if (path.empty())
    {
        WCHAR   auto_path[MAX_PATH * 4];

        status_error = !get_auto_gvim_path(GetModuleHandleW(NULL), auto_path,
                                                        ARRAYSIZE(auto_path));
        if (status_error)
            status = tr(IDS_STATUS_NOT_FOUND);
        else
            status = tr(IDS_STATUS_AUTO) + auto_path;
    }
    else
    {
        status_error = !file_exists(path.c_str());
        if (status_error)
            status = tr(IDS_STATUS_MISSING);
    }
    SetDlgItemTextW(dlg, IDC_GVIM_STATUS, status.c_str());
}

/*
 * Let the user pick jvim.exe.
 */
    static void
browse(HWND dlg)
{
    WCHAR           file[MAX_PATH * 4] = L"";
    WCHAR           dir[MAX_PATH * 4] = L"";
    std::wstring    path = get_path_field(dlg);
    std::wstring    title = tr(IDS_BROWSE_TITLE);
    std::wstring    filter = tr(IDS_BROWSE_FILTER);
    OPENFILENAMEW   ofn = { sizeof(ofn) };

    // GetOpenFileNameW() wants "name\0pattern\0name\0pattern\0\0".  A
    // string table can't hold NUL characters, so IDS_BROWSE_FILTER uses "|"
    // instead: turn them into NULs.
    for (WCHAR &c : filter)
        if (c == L'|')
            c = 0;

    // Start in the folder of the current or detected jvim.exe.
    if (path.empty())
        get_auto_gvim_path(GetModuleHandleW(NULL), dir, ARRAYSIZE(dir));
    else
        lstrcpynW(dir, path.c_str(), ARRAYSIZE(dir));
    WCHAR *sep = wcsrchr(dir, L'\\');
    if (sep != NULL)
        *sep = 0;

    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAYSIZE(file);
    ofn.lpstrInitialDir = *dir != 0 ? dir : NULL;
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY
                                                            | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
        SetDlgItemTextW(dlg, IDC_GVIM_PATH, file);
}

/*
 * Save the settings.  Returns FALSE when they are invalid or not saved.
 */
    static BOOL
save_settings(HWND dlg)
{
    std::wstring    path = get_path_field(dlg);
    DWORD           use_tabs;
    HKEY            key;
    LSTATUS         err;

    use_tabs = IsDlgButtonChecked(dlg, IDC_USE_TABS) == BST_CHECKED;
    if (!path.empty() && !file_exists(path.c_str()))
    {
        MessageBoxW(dlg, tr(IDS_ERROR_MISSING).c_str(),
                    tr(IDS_SETTINGS_TITLE).c_str(), MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, IDC_GVIM_PATH));
        return FALSE;
    }

    err = RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0,
                                            KEY_SET_VALUE, NULL, &key, NULL);
    if (err == ERROR_SUCCESS)
    {
        if (path.empty())
            RegDeleteValueW(key, SETTING_GVIM_PATH);
        else
            err = RegSetValueExW(key, SETTING_GVIM_PATH, 0, REG_SZ,
                            (const BYTE *)path.c_str(),
                            (DWORD)((path.size() + 1) * sizeof(WCHAR)));
        if (err == ERROR_SUCCESS)
            err = RegSetValueExW(key, SETTING_USE_TABS, 0, REG_DWORD,
                                (const BYTE *)&use_tabs, sizeof(use_tabs));
        RegCloseKey(key);
    }
    if (err != ERROR_SUCCESS)
    {
        MessageBoxW(dlg, tr(IDS_ERROR_SAVE).c_str(),
                    tr(IDS_SETTINGS_TITLE).c_str(), MB_OK | MB_ICONERROR);
        return FALSE;
    }
    return TRUE;
}

    static void
init_dialog(HWND dlg)
{
    WCHAR       path[MAX_PATH * 4];
    HINSTANCE   inst = GetModuleHandleW(NULL);

    SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(inst,
                MAKEINTRESOURCEW(IDI_JVIMEXT11), IMAGE_ICON,
                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(inst,
                MAKEINTRESOURCEW(IDI_JVIMEXT11), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON),
                GetSystemMetrics(SM_CYSMICON), 0));

    // Header title
    LOGFONTW    lf;
    HFONT       font = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
    if (font != NULL && GetObjectW(font, sizeof(lf), &lf) != 0)
    {
        lf.lfHeight = lf.lfHeight * 3 / 2;
        lf.lfWeight = FW_SEMIBOLD;
        title_font = CreateFontIndirectW(&lf);
        SendDlgItemMessageW(dlg, IDC_TITLE, WM_SETFONT,
                                                (WPARAM)title_font, FALSE);
    }

    SetWindowTextW(dlg, tr(IDS_SETTINGS_TITLE).c_str());
    SetDlgItemTextW(dlg, IDC_SUBTITLE,
                                (tr(IDS_VERSION) + VERSION_STRING).c_str());
    SetDlgItemTextW(dlg, IDC_GROUP_GVIM, tr(IDS_GROUP_GVIM).c_str());
    SetDlgItemTextW(dlg, IDC_GROUP_OPEN, tr(IDS_GROUP_OPEN).c_str());
    SetDlgItemTextW(dlg, IDC_GVIM_LABEL, tr(IDS_GVIM_LABEL).c_str());
    SetDlgItemTextW(dlg, IDC_BROWSE, tr(IDS_BROWSE).c_str());
    SetDlgItemTextW(dlg, IDC_USE_TABS, tr(IDS_USE_TABS).c_str());
    SetDlgItemTextW(dlg, IDCANCEL, tr(IDS_CANCEL).c_str());

    // License and home page
    std::wstring about = tr(IDS_LICENSE);
    about += L"\n<a href=\"" PROJECT_URL L"\">" PROJECT_URL L"</a>";
    SetDlgItemTextW(dlg, IDC_ABOUT, about.c_str());
    std::wstring cue = tr(IDS_GVIM_CUE);
    SendDlgItemMessageW(dlg, IDC_GVIM_PATH, EM_SETCUEBANNER, TRUE,
                                                    (LPARAM)cue.c_str());

    get_custom_gvim_path(path, ARRAYSIZE(path));
    SetDlgItemTextW(dlg, IDC_GVIM_PATH, path);
    CheckDlgButton(dlg, IDC_USE_TABS,
                            get_use_tabs() ? BST_CHECKED : BST_UNCHECKED);
    update_status(dlg);
}

    static INT_PTR CALLBACK
dialog_proc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_INITDIALOG:
            init_dialog(dlg);
            return TRUE;

        case WM_CTLCOLORSTATIC:
        {
            // Display errors in red.
            int id = GetDlgCtrlID((HWND)lparam);

            if (id == IDC_SUBTITLE || id == IDC_GVIM_STATUS)
            {
                HDC dc = (HDC)wparam;

                if (id == IDC_GVIM_STATUS && status_error)
                    SetTextColor(dc, RGB(0xC4, 0x2B, 0x1C));
                else
                    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                SetBkMode(dc, TRANSPARENT);
                return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        }

        case WM_DESTROY:
            if (title_font != NULL)
                DeleteObject(title_font);
            break;

        case WM_NOTIFY:
        {
            // Open the links of the "About" text in the browser.
            NMLINK *link = (NMLINK *)lparam;

            if (link->hdr.idFrom == IDC_ABOUT
                    && (link->hdr.code == NM_CLICK
                        || link->hdr.code == NM_RETURN))
            {
                ShellExecuteW(dlg, L"open", link->item.szUrl, NULL, NULL,
                                                            SW_SHOWNORMAL);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam))
            {
                case IDC_GVIM_PATH:
                    if (HIWORD(wparam) == EN_CHANGE)
                        update_status(dlg);
                    return TRUE;
                case IDC_BROWSE:
                    browse(dlg);
                    return TRUE;
                case IDOK:
                    if (save_settings(dlg))
                        EndDialog(dlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

    int WINAPI
wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };

    InitCommonControlsEx(&icc); // for the SysLink control

    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, dialog_proc,
                                                                        0);
    return 0;
}
