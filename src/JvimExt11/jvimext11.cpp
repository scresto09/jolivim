/* vi:set ts=8 sts=4 sw=4 et:
 *
 * VIM - Vi IMproved    jvimext11 by Sylvain Cresto
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

/*
 * jvimext11 is a DLL which implements IExplorerCommand for the Windows 11
 * File Explorer context menu: "Edit in JoliVIM" on files and "Open with
 * JoliVIM" on folders.
 *
 * Windows 11 only shows the entries of applications with a package identity
 * at the root of the context menu.  The COM class is therefore declared in
 * the package manifest (AppxManifest.xml.in), not registered by
 * DllRegisterServer.  The installer registers that package with the Vim
 * directory as external location.  On older Windows versions, or in "Show
 * more options", gvimext.dll provides the "Edit with Vim" entries.
 *
 * There is a single entry on purpose: Explorer groups the entries of one
 * package in a submenu named after the package, and only shows an entry at
 * the root of the menu when it is the package's only one.
 *
 * gVim is found next to the DLL, through the registry value written by the
 * installer, or chosen in the settings window, see jvimext11.h.  The files
 * are opened in the gVim already running, if any, with
 * "gvim --remote-tab-silent" (default) or "gvim --remote-silent", depending
 * on the settings.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <new>
#include <string>

#include "jvimext11.h"
#include "resource.h"

// {AF778FC4-B163-466E-9EB9-79E0224B7B24}
static const CLSID CLSID_VimEditCommand =
{
    0xaf778fc4, 0xb163, 0x466e,
    { 0x9e, 0xb9, 0x79, 0xe0, 0x22, 0x4b, 0x7b, 0x24 }
};

static HINSTANCE    g_hinst;
static LONG         g_cDllRef;

/*
 * Return the full path of jvim.exe to use: the one chosen in the settings
 * when it exists, otherwise the one found automatically.  An empty string
 * when there is none.
 */
    static std::wstring
gvim_path(void)
{
    WCHAR   path[MAX_PATH * 4];

    get_custom_gvim_path(path, ARRAYSIZE(path));
    if (file_exists(path)
            || get_auto_gvim_path(g_hinst, path, ARRAYSIZE(path)))
        return std::wstring(path);
    return std::wstring();
}

/*
 * Whether "path" is a directory.
 */
    static BOOL
is_directory(PCWSTR path)
{
    DWORD   attr = GetFileAttributesW(path);

    return attr != INVALID_FILE_ATTRIBUTES
                                && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/*
 * Menu title: "Edit" for files, "Open" when only folders are selected (Vim
 * shows them with netrw).
 */
    static std::wstring
get_title(BOOL folders)
{
    return load_string(g_hinst, folders ? IDS_OPEN_IN_VIM : IDS_EDIT_IN_VIM);
}

/*
 * Number of items selected, or 0 if unknown.
 */
    static DWORD
item_count(IShellItemArray *items)
{
    DWORD   count = 0;

    if (items == NULL || FAILED(items->GetCount(&count)))
        return 0;
    return count;
}

/*
 * Whether all the selected items are folders.
 */
    static BOOL
only_folders(IShellItemArray *items)
{
    DWORD   count = item_count(items);
    BOOL    folders = count > 0;

    for (DWORD i = 0; i < count && folders; i++)
    {
        IShellItem  *item;
        LPWSTR      path;

        folders = FALSE;
        if (FAILED(items->GetItemAt(i, &item)))
            break;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            folders = is_directory(path);
            CoTaskMemFree(path);
        }
        item->Release();
    }
    return folders;
}

class VimCommand final : public IExplorerCommand
{
public:
    VimCommand() : m_cRef(1)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB qit[] = {
            QITABENT(VimCommand, IExplorerCommand),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&m_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        LONG cRef = InterlockedDecrement(&m_cRef);

        if (cRef == 0)
            delete this;
        return cRef;
    }

    // IExplorerCommand
    IFACEMETHODIMP GetTitle(IShellItemArray *items, LPWSTR *ppszName)
    {
        return SHStrDupW(get_title(only_folders(items)).c_str(), ppszName);
    }

    IFACEMETHODIMP GetIcon(IShellItemArray *, LPWSTR *ppszIcon)
    {
        std::wstring icon = gvim_path();

        if (icon.empty())
        {
            *ppszIcon = NULL;
            return E_FAIL;
        }
        icon += L",0";
        return SHStrDupW(icon.c_str(), ppszIcon);
    }

    IFACEMETHODIMP GetToolTip(IShellItemArray *, LPWSTR *ppszInfotip)
    {
        *ppszInfotip = NULL;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP GetCanonicalName(GUID *pguidCommandName)
    {
        *pguidCommandName = CLSID_VimEditCommand;
        return S_OK;
    }

    IFACEMETHODIMP GetState(IShellItemArray *, BOOL, EXPCMDSTATE *pCmdState)
    {
        // Without Vim there is nothing to launch: hide the entry.
        *pCmdState = gvim_path().empty() ? ECS_HIDDEN : ECS_ENABLED;
        return S_OK;
    }

    IFACEMETHODIMP Invoke(IShellItemArray *items, IBindCtx *)
    {
        std::wstring    gvim = gvim_path();
        std::wstring    cmdline;
        std::wstring    workdir;
        DWORD           count = item_count(items);

        if (gvim.empty() || count == 0)
            return E_FAIL;

        // Open the files in the gVim already running, if any (the "GVIM"
        // server), otherwise start a new gVim.  With the "tab pages" setting
        // (default), each file goes to a new tab page (":tab drop" goes to
        // the tab page of a file that is already open); "-p" is for a new
        // gVim, since without a server "--remote-tab-silent" edits the files
        // without tab pages.
        cmdline = L"\"" + gvim + L"\"";
        cmdline += get_use_tabs() ? L" -p --remote-tab-silent"
                                  : L" --remote-silent";

        for (DWORD i = 0; i < count; i++)
        {
            IShellItem  *item;
            LPWSTR      path;

            if (FAILED(items->GetItemAt(i, &item)))
                continue;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                cmdline += L" \"";
                cmdline += path;
                cmdline += L"\"";
                // Start a new gVim in the first folder, or in the folder of
                // the first file.
                if (workdir.empty())
                {
                    workdir = path;
                    if (!is_directory(path))
                    {
                        size_t sep = workdir.find_last_of(L'\\');

                        if (sep != std::wstring::npos)
                            workdir.resize(sep);
                        else
                            workdir.clear();
                    }
                }
                CoTaskMemFree(path);
            }
            item->Release();
        }

        STARTUPINFOW        si = { sizeof(si) };
        PROCESS_INFORMATION pi;

        // Let the gVim server bring its window to the foreground.
        AllowSetForegroundWindow(ASFW_ANY);

        if (!CreateProcessW(gvim.c_str(), &cmdline[0], NULL, NULL, FALSE, 0,
                            NULL, workdir.empty() ? NULL : workdir.c_str(),
                            &si, &pi))
            return HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return S_OK;
    }

    IFACEMETHODIMP GetFlags(EXPCMDFLAGS *pFlags)
    {
        *pFlags = ECF_DEFAULT;
        return S_OK;
    }

    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand **ppEnum)
    {
        *ppEnum = NULL;
        return E_NOTIMPL;
    }

private:
    ~VimCommand()
    {
        InterlockedDecrement(&g_cDllRef);
    }

    LONG        m_cRef;
};

class ClassFactory final : public IClassFactory
{
public:
    ClassFactory() : m_cRef(1)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB qit[] = {
            QITABENT(ClassFactory, IClassFactory),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&m_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        LONG cRef = InterlockedDecrement(&m_cRef);

        if (cRef == 0)
            delete this;
        return cRef;
    }

    // IClassFactory
    IFACEMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv)
    {
        *ppv = NULL;
        if (pUnkOuter != NULL)
            return CLASS_E_NOAGGREGATION;

        VimCommand *cmd = new (std::nothrow) VimCommand();

        if (cmd == NULL)
            return E_OUTOFMEMORY;
        HRESULT hr = cmd->QueryInterface(riid, ppv);
        cmd->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock)
    {
        if (fLock)
            InterlockedIncrement(&g_cDllRef);
        else
            InterlockedDecrement(&g_cDllRef);
        return S_OK;
    }

private:
    ~ClassFactory()
    {
        InterlockedDecrement(&g_cDllRef);
    }

    LONG        m_cRef;
};

    STDAPI_(BOOL)
DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hinst = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

    STDAPI
DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    *ppv = NULL;
    if (!IsEqualCLSID(rclsid, CLSID_VimEditCommand))
        return CLASS_E_CLASSNOTAVAILABLE;

    ClassFactory *factory = new (std::nothrow) ClassFactory();

    if (factory == NULL)
        return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

    STDAPI
DllCanUnloadNow(void)
{
    return g_cDllRef > 0 ? S_FALSE : S_OK;
}
