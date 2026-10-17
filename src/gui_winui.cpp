/* vi:set ts=8 sts=4 sw=4 et: */
/*
 * VIM - Vi IMproved    by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * gui_winui.cpp: Popup menu using WinUI 3 (Windows App SDK).
 *
 * The menu is shown as a CommandBarFlyout, like the context menu of the
 * Windows 11 Notepad: items with an icon (Cut, Copy, Paste, ...) are put in
 * a bar at the top, the other items in a list below it.
 *
 * The Windows App SDK runtime is loaded when the popup menu is used for the
 * first time.  When it is not available WinUI_ShowPopupMenu() returns -1 and
 * the caller falls back to the classic Win32 popup menu.
 */

#define WIN32_LEAN_AND_MEAN

// C++/WinRT and the Windows App SDK require Windows 10.
#undef WINVER
#undef _WIN32_WINNT
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <objbase.h>
#undef GetCurrentTime   // conflicts with the XAML animation API

#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>

#include <appmodel.h>
#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>

#include "gui_winui.h"

namespace wf = winrt::Windows::Foundation;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxcp = winrt::Microsoft::UI::Xaml::Controls::Primitives;
namespace muxh = winrt::Microsoft::UI::Xaml::Hosting;
namespace muw = winrt::Microsoft::UI::Windowing;

namespace
{

// Without a XAML compiler the application has to provide the metadata of
// the standard controls itself, otherwise they can't be styled.
struct VimXamlApp : mux::ApplicationT<VimXamlApp,
                                      mux::Markup::IXamlMetadataProvider>
{
    mux::Markup::IXamlType
    GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const &type)
    {
        return provider().GetXamlType(type);
    }

    mux::Markup::IXamlType
    GetXamlType(winrt::hstring const &fullName)
    {
        return provider().GetXamlType(fullName);
    }

    winrt::com_array<mux::Markup::XmlnsDefinition>
    GetXmlnsDefinitions()
    {
        return provider().GetXmlnsDefinitions();
    }

private:
    // Created when first used, it can't be created before the Application.
    mux::XamlTypeInfo::XamlControlsXamlMetaDataProvider
    provider()
    {
        if (!m_provider)
            m_provider = mux::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return m_provider;
    }

    mux::XamlTypeInfo::XamlControlsXamlMetaDataProvider m_provider{nullptr};
};

// State of one popup menu while it is shown.
struct Session
{
    UINT    chosen = 0;
    bool    opened = false;
    bool    closed = false;
};

enum class InitState
{
    NotTried,
    Ready,
    Failed,
};

typedef BOOL (WINAPI *PreTranslateFunc)(const MSG *);

InitState s_state = InitState::NotTried;
bool s_in_popup = false;
HWND s_parent = NULL;
PreTranslateFunc s_pretranslate = NULL;

winrt::Microsoft::UI::Dispatching::DispatcherQueueController s_dqc{nullptr};
mux::Application s_app{nullptr};
muxh::WindowsXamlManager s_xaml_manager{nullptr};
muxh::DesktopWindowXamlSource s_xaml_source{nullptr};
muxc::Grid s_root{nullptr};

// Why WinUI can't be used, for WinUI_GetError().
WCHAR s_error[512] = L"";

/*
 * Write a trace message, it can be seen in the Output window of a debugger
 * or with DebugView.
 */
    void
trace(const WCHAR *fmt, ...)
{
    WCHAR   buf[600];
    va_list ap;

    va_start(ap, fmt);
    vswprintf(buf, sizeof(buf) / sizeof(buf[0]), fmt, ap);
    va_end(ap);
    OutputDebugStringW(L"[vim winui] ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

    void
set_error(const WCHAR *what, HRESULT hr, const WCHAR *detail = L"")
{
    swprintf(s_error, sizeof(s_error) / sizeof(s_error[0]),
            L"%ls failed (0x%08lX) %ls", what, (unsigned long)hr, detail);
    trace(L"%ls", s_error);
}

/*
 * Whether the Windows App SDK runtime is already available: when Vim runs
 * from an MSIX package (msix\AppxManifest.xml.in) that depends on it, it is
 * in the package graph and the bootstrapper must not be used.  Other
 * packages, like the one of JvimExt11, don't provide it.
 * GetCurrentPackageFullName() is loaded dynamically, like the bootstrap DLL.
 */
    bool
runtime_in_package(void)
{
    auto get_name = reinterpret_cast<decltype(&GetCurrentPackageFullName)>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                "GetCurrentPackageFullName"));
    UINT32 len = 0;

    if (get_name == NULL || get_name(&len, NULL) != ERROR_INSUFFICIENT_BUFFER)
        return false;   // no package identity
    return LoadLibraryExW(L"Microsoft.WindowsAppRuntime.dll", NULL, 0) != NULL;
}

/*
 * Load the Windows App SDK runtime.  The bootstrap DLL is loaded dynamically,
 * so that Vim still starts when it is not present.
 */
    bool
bootstrap(void)
{
    if (runtime_in_package())
    {
        trace(L"Windows App Runtime provided by the package");
        return true;
    }

    HMODULE hmod = LoadLibraryExW(L"Microsoft.WindowsAppRuntime.Bootstrap.dll",
            NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR
                                        | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hmod == NULL)
    {
        set_error(L"Loading Microsoft.WindowsAppRuntime.Bootstrap.dll",
                HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    auto initialize = reinterpret_cast<decltype(&MddBootstrapInitialize2)>(
            GetProcAddress(hmod, "MddBootstrapInitialize2"));
    if (initialize == NULL)
    {
        set_error(L"Finding MddBootstrapInitialize2",
                HRESULT_FROM_WIN32(GetLastError()));
        FreeLibrary(hmod);
        return false;
    }

    PACKAGE_VERSION min_version = {};
    min_version.Version = WINDOWSAPPSDK_RUNTIME_VERSION_UINT64;
    HRESULT hr = initialize(WINDOWSAPPSDK_RELEASE_MAJORMINOR,
                    WINDOWSAPPSDK_RELEASE_VERSION_TAG_W, min_version,
                    MddBootstrapInitializeOptions_None);
    if (FAILED(hr))
    {
        WCHAR detail[100];

        swprintf(detail, sizeof(detail) / sizeof(detail[0]),
                L"- is the Windows App Runtime %u.%u installed?",
                (unsigned)(WINDOWSAPPSDK_RELEASE_MAJORMINOR >> 16),
                (unsigned)(WINDOWSAPPSDK_RELEASE_MAJORMINOR & 0xFFFF));
        set_error(L"MddBootstrapInitialize2", hr, detail);
        FreeLibrary(hmod);
        return false;
    }
    trace(L"Windows App Runtime %u.%u loaded",
            (unsigned)(WINDOWSAPPSDK_RELEASE_MAJORMINOR >> 16),
            (unsigned)(WINDOWSAPPSDK_RELEASE_MAJORMINOR & 0xFFFF));
    // The runtime stays loaded until Vim exits, MddBootstrapShutdown() is
    // not called.
    return true;
}

/*
 * Initialize XAML for the current thread and create the XAML island in
 * "hwnd" that the popup menus are attached to.
 */
    bool
init_xaml(HWND hwnd)
{
    const WCHAR *step = L"init_apartment";

    try
    {
        try
        {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        }
        catch (winrt::hresult_error const &e)
        {
            // Already initialized by OLE, that's fine.
            if (e.code() != RPC_E_CHANGED_MODE)
                throw;
        }

        step = L"DispatcherQueueController";
        trace(L"%ls", step);
        s_dqc = winrt::Microsoft::UI::Dispatching::DispatcherQueueController
                                                    ::CreateOnCurrentThread();
        // The Application must be created before XAML is initialized for
        // the thread.
        step = L"Creating the XAML Application";
        trace(L"%ls", step);
        s_app = winrt::make<VimXamlApp>();
        step = L"WindowsXamlManager";
        trace(L"%ls", step);
        s_xaml_manager = muxh::WindowsXamlManager::InitializeForCurrentThread();
        step = L"XamlControlsResources";
        trace(L"%ls", step);
        s_app.Resources().MergedDictionaries().Append(
                                            muxc::XamlControlsResources());

        step = L"DesktopWindowXamlSource";
        trace(L"%ls", step);
        s_xaml_source = muxh::DesktopWindowXamlSource();
        s_xaml_source.Initialize(
                            winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd));
        s_root = muxc::Grid();
        s_xaml_source.Content(s_root);
        s_xaml_source.SiteBridge().Hide();
    }
    catch (winrt::hresult_error const &e)
    {
        APTTYPE             type;
        APTTYPEQUALIFIER    qualifier;
        WCHAR               detail[400];

        // Include the COM apartment of the thread, XAML requires STA.
        if (FAILED(CoGetApartmentType(&type, &qualifier)))
            type = (APTTYPE)-1;
        swprintf(detail, sizeof(detail) / sizeof(detail[0]),
                L"[apartment %d] %ls", (int)type, e.message().c_str());
        set_error(step, e.code(), detail);
        return false;
    }
    catch (...)
    {
        set_error(step, E_FAIL);
        return false;
    }

    // Needed for keyboard input in XAML islands, may not exist in older
    // versions of the runtime.
    HMODULE hmod = GetModuleHandleW(L"Microsoft.UI.Windowing.Core.dll");
    if (hmod != NULL)
        s_pretranslate = reinterpret_cast<PreTranslateFunc>(
                GetProcAddress(hmod, "ContentPreTranslateMessage"));

    s_parent = hwnd;
    trace(L"XAML initialized, ContentPreTranslateMessage %ls",
            s_pretranslate != NULL ? L"found" : L"not found");
    return true;
}

    bool
ensure_init(HWND hwnd)
{
    if (s_state == InitState::NotTried)
    {
        if (bootstrap() && init_xaml(hwnd))
            s_state = InitState::Ready;
        else
            s_state = InitState::Failed;
    }
    return s_state == InitState::Ready;
}

    muxc::IconElement
make_icon(WinUIIcon icon)
{
    muxc::Symbol sym;

    switch (icon)
    {
        case WINUI_ICON_UNDO:       sym = muxc::Symbol::Undo; break;
        case WINUI_ICON_REDO:       sym = muxc::Symbol::Redo; break;
        case WINUI_ICON_CUT:        sym = muxc::Symbol::Cut; break;
        case WINUI_ICON_COPY:       sym = muxc::Symbol::Copy; break;
        case WINUI_ICON_PASTE:      sym = muxc::Symbol::Paste; break;
        case WINUI_ICON_DELETE:     sym = muxc::Symbol::Delete; break;
        case WINUI_ICON_SELECTALL:  sym = muxc::Symbol::SelectAll; break;
        default:                    return nullptr;
    }
    return muxc::SymbolIcon(sym);
}

/*
 * Return a Click handler that selects "id" and closes the whole menu.
 */
    mux::RoutedEventHandler
make_click_handler(std::shared_ptr<Session> const &session, UINT id,
        winrt::weak_ref<muxc::CommandBarFlyout> const &flyout)
{
    return [session, id, flyout](wf::IInspectable const &,
                                 mux::RoutedEventArgs const &)
    {
        session->chosen = id;
        try
        {
            if (auto f = flyout.get())
                f.Hide();
        }
        catch (...)
        {
            session->closed = true;
        }
    };
}

// Returns the Click handler for a menu item with an ID.
typedef std::function<mux::RoutedEventHandler(UINT)> HandlerFactory;

// Controls of menu items by ID, to change the enabled state.
typedef std::unordered_map<UINT, muxc::Control> ControlMap;

/*
 * Add "items" to the menu "dest".  When "controls" is not NULL the created
 * controls are stored in it.
 */
    void
add_menu_items(wf::Collections::IVector<muxc::MenuFlyoutItemBase> const &dest,
        const WinUIMenuItem *items, int count,
        HandlerFactory const &factory, ControlMap *controls)
{
    for (int i = 0; i < count; ++i)
    {
        const WinUIMenuItem *item = &items[i];

        if (item->label == NULL)
        {
            dest.Append(muxc::MenuFlyoutSeparator());
        }
        else if (item->children != NULL)
        {
            muxc::MenuFlyoutSubItem sub;

            sub.Text(item->label);
            sub.IsEnabled(item->enabled != 0);
            add_menu_items(sub.Items(), item->children, item->nchildren,
                                                        factory, controls);
            if (controls != NULL)
                controls->insert_or_assign(item->id, sub);
            dest.Append(sub);
        }
        else
        {
            muxc::MenuFlyoutItem mi;

            mi.Text(item->label);
            mi.IsEnabled(item->enabled != 0);
            if (item->accel != NULL)
                mi.KeyboardAcceleratorTextOverride(item->accel);
            if (auto icon = make_icon(item->icon))
                mi.Icon(icon);
            mi.Click(factory(item->id));
            if (controls != NULL)
                controls->insert_or_assign(item->id, mi);
            dest.Append(mi);
        }
    }
}

/*
 * Fill "flyout" with "items": the ones with an icon go in the bar, the others
 * in the list below it.
 */
    void
fill_flyout(muxc::CommandBarFlyout const &flyout,
        const WinUIMenuItem *items, int count,
        std::shared_ptr<Session> const &session)
{
    winrt::weak_ref<muxc::CommandBarFlyout> weak = flyout;
    HandlerFactory factory = [session, weak](UINT id)
    {
        return make_click_handler(session, id, weak);
    };
    auto primary = flyout.PrimaryCommands();
    auto secondary = flyout.SecondaryCommands();
    bool pending_sep = false;

    for (int i = 0; i < count; ++i)
    {
        const WinUIMenuItem *item = &items[i];

        if (item->label == NULL)
        {
            // Avoid separators at the start and end and double separators,
            // they may happen when items moved to the bar.
            pending_sep = secondary.Size() > 0;
            continue;
        }

        muxc::AppBarButton button;
        button.Label(item->label);
        button.IsEnabled(item->enabled != 0);

        if (item->icon != WINUI_ICON_NONE && item->children == NULL)
        {
            button.Icon(make_icon(item->icon));
            muxc::ToolTipService::SetToolTip(button,
                                        winrt::box_value(item->label));
            button.Click(make_click_handler(session, item->id, weak));
            primary.Append(button);
            continue;
        }

        if (pending_sep)
        {
            secondary.Append(muxc::AppBarSeparator());
            pending_sep = false;
        }
        if (item->accel != NULL)
            button.KeyboardAcceleratorTextOverride(item->accel);
        if (item->mnemonic != 0)
            button.AccessKey(winrt::hstring(&item->mnemonic, 1));
        if (item->children != NULL)
        {
            muxc::MenuFlyout sub;

            add_menu_items(sub.Items(), item->children, item->nchildren,
                                                            factory, NULL);
            button.Flyout(sub);
        }
        else
            button.Click(make_click_handler(session, item->id, weak));
        secondary.Append(button);
    }
}

/*
 * Run a message loop until the popup menu "session" is closed.  Returns false
 * when the menu didn't open.
 */
    bool
popup_message_loop(std::shared_ptr<Session> const &session)
{
    DWORD   start = GetTickCount();
    MSG     msg;

    while (!session->closed)
    {
        if (!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (!session->opened)
            {
                if (GetTickCount() - start > 5000)
                    return false;
                MsgWaitForMultipleObjectsEx(0, NULL, 100, QS_ALLINPUT,
                                                        MWMO_INPUTAVAILABLE);
            }
            else
                MsgWaitForMultipleObjectsEx(0, NULL, INFINITE, QS_ALLINPUT,
                                                        MWMO_INPUTAVAILABLE);
            continue;
        }
        if (msg.message == WM_QUIT)
        {
            // Let the main loop handle it.
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (s_pretranslate != NULL && s_pretranslate(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

// Height of the MenuBar control in DIPs, used until it is loaded.
#define MENUBAR_DEFAULT_HEIGHT 40

// The menu bar, in its own XAML island.
muxh::DesktopWindowXamlSource s_menubar_source{nullptr};
muxc::Grid s_menubar_root{nullptr};
muxc::MenuBar s_menubar{nullptr};
HWND s_menubar_hwnd = NULL;
ControlMap s_menubar_controls;
std::vector<std::pair<WCHAR, muxc::MenuBarItem>> s_menubar_mnemonics;

// The find/replace bar, in its own XAML island.
muxh::DesktopWindowXamlSource s_findbar_source{nullptr};
muxc::Grid s_findbar_root{nullptr};
muxc::Border s_findbar_card{nullptr};
muxc::AutoSuggestBox s_find_box{nullptr};
muxc::TextBox s_replace_box{nullptr};
muxc::Grid s_replace_row{nullptr};
muxc::ToggleMenuFlyoutItem s_match_case{nullptr};
muxc::ToggleMenuFlyoutItem s_whole_word{nullptr};
muxc::FontIcon s_expand_icon{nullptr};
HWND s_findbar_hwnd = NULL;
int s_findbar_left = 0, s_findbar_top = 0, s_findbar_width = 0;
std::vector<winrt::hstring> s_find_history;

// Modal dialog (ContentDialog) in its own full-window XAML island.
muxh::DesktopWindowXamlSource s_dialog_source{nullptr};
muxc::Grid s_dialog_root{nullptr};
HWND s_dialog_hwnd = NULL;

// Custom title bar via AppWindow (system keeps the caption buttons).
muw::AppWindow s_appwindow{nullptr};
bool s_titlebar_active = false;

// TabView in the title bar.
muxh::DesktopWindowXamlSource s_tabbar_source{nullptr};
muxc::Grid s_tabbar_root{nullptr};
muxc::TabView s_tabview{nullptr};
muxc::Grid s_tabdrag{nullptr};	// draggable region after the tabs
HWND s_tabbar_hwnd = NULL;
bool s_tab_updating = false;	// guard against feedback when we set tabs
bool s_tab_force_rebuild = false; // recreate tab items on next update (theme)
int s_tab_island_x = 0;		// island position in the window (device px)
int s_tab_island_y = 0;

// Labels of the per-tab context menu (filled from the C side with gettext).
winrt::hstring s_tabmenu_new;
winrt::hstring s_tabmenu_close;
winrt::hstring s_tabmenu_others;
winrt::hstring s_tabmenu_right;
winrt::hstring s_tabmenu_copy;
winrt::hstring s_tabmenu_moveleft;
winrt::hstring s_tabmenu_moveright;

/*
 * TRUE when Windows apps are set to the dark theme.
 */
    static bool
winui_system_is_dark(void)
{
    DWORD   val = 1;		// default: light
    DWORD   sz = sizeof(val);

    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &val, &sz)
                                                        == ERROR_SUCCESS)
        return val == 0;
    return false;
}

/*
 * Give the keyboard focus back to Vim.
 */
    void
restore_vim_focus(void)
{
    if (s_parent != NULL)
        SetFocus(s_parent);
}

    mux::RoutedEventHandler
make_menubar_click_handler(UINT id)
{
    return [id](wf::IInspectable const &, mux::RoutedEventArgs const &)
    {
        restore_vim_focus();
        PostMessage(s_parent, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    };
}

/*
 * Return true when "hwnd" belongs to XAML: the menu bar island or the popup
 * window of one of its menus.
 */
    bool
is_xaml_window(HWND hwnd)
{
    WCHAR   cls[64];

    if (hwnd == NULL)
        return false;
    if (s_menubar_hwnd != NULL
            && (hwnd == s_menubar_hwnd || IsChild(s_menubar_hwnd, hwnd)))
        return true;
    if (s_findbar_hwnd != NULL
            && (hwnd == s_findbar_hwnd || IsChild(s_findbar_hwnd, hwnd)))
        return true;
    if (s_dialog_hwnd != NULL
            && (hwnd == s_dialog_hwnd || IsChild(s_dialog_hwnd, hwnd)))
        return true;
    if (s_tabbar_hwnd != NULL
            && (hwnd == s_tabbar_hwnd || IsChild(s_tabbar_hwnd, hwnd)))
        return true;
    if (GetClassNameW(hwnd, cls, sizeof(cls) / sizeof(cls[0])) == 0)
        return false;
    return wcsncmp(cls, L"Microsoft.UI.", 13) == 0
                                || wcscmp(cls, L"InputSiteWindowClass") == 0;
}

/*
 * Use the background color of the toolbar, and the XAML theme that fits its
 * lightness.
 */
    void
set_menubar_colors(void)
{
    bool dark = winui_system_is_dark();
    // Match the selected tab colour so the active tab visually extends into
    // the menu and toolbar (like Notepad).
    winrt::Windows::UI::Color   color = dark
            ? winrt::Windows::UI::Color{255, 0x2B, 0x2B, 0x2B}
            : winrt::Windows::UI::Color{255, 0xF8, 0xF8, 0xF8};

    s_menubar_root.Background(mux::Media::SolidColorBrush(color));
    s_menubar_root.RequestedTheme(dark ? mux::ElementTheme::Dark
                                       : mux::ElementTheme::Light);
}

    void
post_layout(void)
{
    PostMessage(s_parent, WM_WINUI_LAYOUT, 0, 0);
}

} // namespace

/*
 * Show a popup menu with "items" at screen position "x", "y" for window
 * "hwnd" and wait until it is closed.
 * Returns the ID of the selected item, zero when the menu was cancelled and
 * -1 when WinUI can't be used.
 */
    int
WinUI_ShowPopupMenu(HWND hwnd, const WinUIMenuItem *items, int count,
        int x, int y)
{
    if (s_in_popup || !ensure_init(hwnd))
        return -1;

    HWND    focus = GetFocus();
    int     result = -1;

    s_in_popup = true;
    try
    {
        auto session = std::make_shared<Session>();
        muxc::CommandBarFlyout flyout;
        winrt::event_token loaded_token{};

        flyout.AlwaysExpanded(true);
        // The island is only one pixel, the menu must be able to go outside.
        flyout.ShouldConstrainToRootBounds(false);
        fill_flyout(flyout, items, count, session);
        flyout.Opened([session](wf::IInspectable const &,
                                wf::IInspectable const &)
        {
            trace(L"menu opened");
            session->opened = true;
        });
        flyout.Closed([session](wf::IInspectable const &,
                                wf::IInspectable const &)
        {
            trace(L"menu closed, chosen %u", session->chosen);
            session->closed = true;
        });

        // Put the island at the requested position, the menu is placed
        // relative to its top-left corner.
        POINT pt = {x, y};
        ScreenToClient(s_parent, &pt);
        auto bridge = s_xaml_source.SiteBridge();
        bridge.MoveAndResize(winrt::Windows::Graphics::RectInt32{
                                                        pt.x, pt.y, 1, 1});
        bridge.Show();
        trace(L"island at %ld,%ld, root loaded: %d, XamlRoot: %d",
                pt.x, pt.y, (int)s_root.IsLoaded(),
                (int)(s_root.XamlRoot() != nullptr));

        // An exception escaping an event handler terminates Vim, catch
        // everything here.
        auto show = [flyout, session]()
        {
            try
            {
                muxcp::FlyoutShowOptions options;

                options.Position(wf::IReference<wf::Point>(wf::Point(0, 0)));
                // Like Notepad: to the right of and below the mouse pointer.
                // XAML flips it when there is no room.
                options.Placement(
                        muxcp::FlyoutPlacementMode::BottomEdgeAlignedLeft);
                options.ShowMode(muxcp::FlyoutShowMode::Standard);
                trace(L"ShowAt");
                flyout.ShowAt(s_root, options);
            }
            catch (winrt::hresult_error const &e)
            {
                set_error(L"ShowAt", e.code(), e.message().c_str());
                session->closed = true;
            }
            catch (...)
            {
                set_error(L"ShowAt", E_FAIL);
                session->closed = true;
            }
        };
        if (s_root.IsLoaded())
            show();
        else
            loaded_token = s_root.Loaded(
                    [show](wf::IInspectable const &,
                           mux::RoutedEventArgs const &)
            {
                show();
            });

        bool ok = popup_message_loop(session);
        if (!ok)
            set_error(L"Opening the menu", E_FAIL, L"(timeout)");
        else if (!session->opened)
            ok = false;     // ShowAt() failed

        if (loaded_token)
            s_root.Loaded(loaded_token);
        if (!ok)
            flyout.Hide();
        bridge.Hide();
        if (ok)
            result = (int)session->chosen;
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"Showing the menu", e.code(), e.message().c_str());
        result = -1;
    }
    catch (...)
    {
        set_error(L"Showing the menu", E_FAIL);
        result = -1;
    }
    s_in_popup = false;

    if (focus != NULL && IsWindow(focus))
        SetFocus(focus);
    if (result < 0)
        // Don't try again, use the Win32 popup menu from now on.  XAML is
        // not shut down, that is not reliable while Vim keeps running.
        s_state = InitState::Failed;
    return result;
}

/*
 * Return why WinUI can't be used, empty when it can.
 */
    const WCHAR *
WinUI_GetError(void)
{
    return s_error;
}

/*
 * Called when Vim exits.  Tearing down XAML and the Windows App Runtime is
 * not reliable, the objects are released without calling them so that their
 * destructors don't run during process exit.
 */
    void
WinUI_Final(void)
{
    trace(L"exit");
    for (auto &c : s_menubar_controls)
        winrt::detach_abi(c.second);
    for (auto &m : s_menubar_mnemonics)
        winrt::detach_abi(m.second);
    winrt::detach_abi(s_menubar);
    winrt::detach_abi(s_menubar_root);
    winrt::detach_abi(s_menubar_source);
    winrt::detach_abi(s_match_case);
    winrt::detach_abi(s_whole_word);
    winrt::detach_abi(s_expand_icon);
    winrt::detach_abi(s_find_box);
    winrt::detach_abi(s_replace_box);
    winrt::detach_abi(s_replace_row);
    winrt::detach_abi(s_findbar_card);
    winrt::detach_abi(s_findbar_root);
    winrt::detach_abi(s_findbar_source);
    winrt::detach_abi(s_dialog_root);
    winrt::detach_abi(s_dialog_source);
    winrt::detach_abi(s_tabdrag);
    winrt::detach_abi(s_tabview);
    winrt::detach_abi(s_tabbar_root);
    winrt::detach_abi(s_tabbar_source);
    winrt::detach_abi(s_root);
    winrt::detach_abi(s_xaml_source);
    winrt::detach_abi(s_xaml_manager);
    winrt::detach_abi(s_app);
    winrt::detach_abi(s_dqc);
}

/*
 * Create the WinUI menu bar in "hwnd".  Returns FALSE when WinUI can't be
 * used, the Win32 menu bar is used then.
 */
    int
WinUI_MenuBarInit(HWND hwnd)
{
    if (s_menubar)
        return TRUE;
    if (!ensure_init(hwnd))
        return FALSE;

    try
    {
        s_menubar_source = muxh::DesktopWindowXamlSource();
        s_menubar_source.Initialize(
                            winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd));
        s_menubar_root = muxc::Grid();
        s_menubar = muxc::MenuBar();
        s_menubar.HorizontalAlignment(mux::HorizontalAlignment::Left);
        s_menubar.VerticalAlignment(mux::VerticalAlignment::Center);
        s_menubar_root.Children().Append(s_menubar);
        set_menubar_colors();

        // Escape in the menu bar goes back to Vim.  The MenuBar handles the
        // key itself, also get the handled events.
        s_menubar.AddHandler(mux::UIElement::KeyDownEvent(),
                winrt::box_value(mux::Input::KeyEventHandler(
                    [](wf::IInspectable const &,
                       mux::Input::KeyRoutedEventArgs const &e)
                    {
                        if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
                        {
                            e.Handled(true);
                            restore_vim_focus();
                        }
                    })), true);
        // The size is only known when loaded.
        s_menubar.Loaded([](wf::IInspectable const &,
                            mux::RoutedEventArgs const &)
        {
            post_layout();
        });

        s_menubar_source.Content(s_menubar_root);
        s_menubar_hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(
                                s_menubar_source.SiteBridge().WindowId());
        s_menubar_source.SiteBridge().Hide();
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"Creating the menu bar", e.code(), e.message().c_str());
        s_menubar = nullptr;
        return FALSE;
    }
    catch (...)
    {
        set_error(L"Creating the menu bar", E_FAIL);
        s_menubar = nullptr;
        return FALSE;
    }
    trace(L"menu bar created");
    return TRUE;
}

/*
 * Replace the menus of the menu bar with "items", each with its children.
 */
    void
WinUI_MenuBarSetItems(const WinUIMenuItem *items, int count)
{
    if (!s_menubar)
        return;
    try
    {
        HandlerFactory factory = [](UINT id)
        {
            return make_menubar_click_handler(id);
        };
        auto bar_items = s_menubar.Items();

        s_menubar_controls.clear();
        s_menubar_mnemonics.clear();
        bar_items.Clear();
        for (int i = 0; i < count; ++i)
        {
            const WinUIMenuItem *item = &items[i];

            if (item->label == NULL || item->children == NULL)
                continue;
            muxc::MenuBarItem mbi;
            mbi.Title(item->label);
            mbi.IsEnabled(item->enabled != 0);
            if (item->mnemonic != 0)
            {
                mbi.AccessKey(winrt::hstring(&item->mnemonic, 1));
                s_menubar_mnemonics.emplace_back(
                                        (WCHAR)towlower(item->mnemonic), mbi);
            }
            add_menu_items(mbi.Items(), item->children, item->nchildren,
                                                factory, &s_menubar_controls);
            s_menubar_controls.insert_or_assign(item->id, mbi);
            bar_items.Append(mbi);
        }
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"Filling the menu bar", e.code(), e.message().c_str());
    }
    catch (...)
    {
    }
    // The size may change, let Vim position the menu bar again.
    post_layout();
}

/*
 * Enable or disable the menu item "id".
 */
    void
WinUI_MenuBarSetEnabled(UINT id, int enabled)
{
    auto it = s_menubar_controls.find(id);

    if (it == s_menubar_controls.end())
        return;
    try
    {
        it->second.IsEnabled(enabled != 0);
    }
    catch (...)
    {
    }
}

/*
 * Get the size the menu bar needs, in pixels.  Zero when not known yet.
 */
    void
WinUI_MenuBarGetSize(int *width, int *height)
{
    *width = 0;
    *height = 0;
    if (!s_menubar)
        return;
    try
    {
        float   inf = std::numeric_limits<float>::infinity();
        double  scale = GetDpiForWindow(s_parent) / 96.0;

        s_menubar.Measure(wf::Size{inf, inf});
        auto size = s_menubar.DesiredSize();
        *width = (int)(size.Width * scale + 0.99);
        *height = (int)(size.Height * scale + 0.99);
        // Before the menu bar was loaded its size is not known, use the
        // default height of the MenuBar control, so that the space is
        // reserved when Vim starts.
        if (*height <= 0)
            *height = (int)(MENUBAR_DEFAULT_HEIGHT * scale + 0.99);
    }
    catch (...)
    {
    }
}

/*
 * Set the position of the menu bar in the client area, in pixels.
 */
    void
WinUI_MenuBarSetPos(int x, int y, int w, int h)
{
    if (!s_menubar)
        return;
    try
    {
        s_menubar_source.SiteBridge().MoveAndResize(
                winrt::Windows::Graphics::RectInt32{x, y,
                                        w > 0 ? w : 1, h > 0 ? h : 1});
    }
    catch (...)
    {
    }
}

    void
WinUI_MenuBarShow(int show)
{
    if (!s_menubar)
        return;
    try
    {
        if (show)
            s_menubar_source.SiteBridge().Show();
        else
            s_menubar_source.SiteBridge().Hide();
    }
    catch (...)
    {
    }
}

/*
 * Open the menu with access key "ch", like the Win32 menu bar does for
 * Alt-ch.  When "ch" is zero (Alt or F10) only put the focus on the first
 * menu.  Returns FALSE when there is no such menu.
 */
    int
WinUI_MenuBarOpen(int ch)
{
    if (!s_menubar)
        return FALSE;
    try
    {
        muxc::MenuBarItem target{nullptr};

        if (ch == 0)
        {
            if (s_menubar.Items().Size() > 0)
                target = s_menubar.Items().GetAt(0);
        }
        else
        {
            WCHAR lower = (WCHAR)towlower((wint_t)ch);

            for (auto const &m : s_menubar_mnemonics)
                if (m.first == lower)
                {
                    target = m.second;
                    break;
                }
        }
        if (!target)
            return FALSE;

        SetFocus(s_menubar_hwnd);
        target.Focus(mux::FocusState::Keyboard);
        if (ch != 0)
        {
            auto peer = mux::Automation::Peers::FrameworkElementAutomationPeer
                                            ::CreatePeerForElement(target);
            if (peer)
                if (auto pattern = peer.GetPattern(
                    mux::Automation::Peers::PatternInterface::ExpandCollapse))
                    pattern.as<mux::Automation::Provider::
                                        IExpandCollapseProvider>().Expand();
        }
        return TRUE;
    }
    catch (...)
    {
        return FALSE;
    }
}

/*
 * Handle a keyboard message for the XAML windows (menu bar and its menus),
 * so that Vim doesn't take the keys.  Returns TRUE when it was handled.
 */
    int
WinUI_PreTranslateMessage(MSG *msg)
{
    if (s_state != InitState::Ready
            || msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST
            || !is_xaml_window(msg->hwnd))
        return FALSE;
    if (s_pretranslate != NULL && s_pretranslate(msg))
        return TRUE;
    TranslateMessage(msg);
    DispatchMessageW(msg);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Find/replace bar (floating, top-centered over the text area, like Notepad).
// ---------------------------------------------------------------------------

namespace {

    muxc::FontIcon
fluent_icon(WCHAR glyph)
{
    muxc::FontIcon icon;

    icon.FontFamily(mux::Media::FontFamily(
                                L"Segoe Fluent Icons, Segoe MDL2 Assets"));
    icon.Glyph(winrt::hstring(&glyph, 1));
    icon.FontSize(14);
    return icon;
}

    bool
checked(muxc::ToggleMenuFlyoutItem const &it)
{
    return it && it.IsChecked();
}

// Run a find/replace action by calling back into Vim.
    void
do_action(int action)
{
    if (!s_find_box)
        return;
    gui_winui_do_findrepl(action, s_find_box.Text().c_str(),
            s_replace_box ? s_replace_box.Text().c_str() : L"",
            checked(s_match_case), checked(s_whole_word));
}

    void
close_findbar(void)
{
    WinUI_HideFindBar();
    gui_winui_findbar_closed();
}

    void
set_replace_visible(bool show)
{
    if (!s_replace_row)
        return;
    s_replace_row.Visibility(show ? mux::Visibility::Visible
                                  : mux::Visibility::Collapsed);
    if (s_expand_icon)
        // ChevronDown to collapse, ChevronUp to expand.
        s_expand_icon.Glyph(winrt::hstring(show ? L"" : L""));
    WinUI_FindBarSetPos(s_findbar_left, s_findbar_top, s_findbar_width);
}

    muxc::Button
text_button(const WCHAR *label)
{
    muxc::Button b;

    b.Content(winrt::box_value(label));
    b.Height(32);
    b.VerticalAlignment(mux::VerticalAlignment::Center);
    return b;
}

    muxc::Button
icon_button(WCHAR glyph, const WCHAR *tip)
{
    muxc::Button b;

    b.Content(fluent_icon(glyph));
    b.Background(mux::Media::SolidColorBrush(
                            winrt::Windows::UI::Colors::Transparent()));
    b.BorderThickness(mux::Thickness{0, 0, 0, 0});
    b.Padding(mux::Thickness{8, 0, 8, 0});
    b.Height(32);
    b.MinWidth(36);
    b.VerticalAlignment(mux::VerticalAlignment::Center);
    b.VerticalContentAlignment(mux::VerticalAlignment::Center);
    b.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
    muxc::ToolTipService::SetToolTip(b, winrt::box_value(tip));
    return b;
}

    muxc::ColumnDefinition
col(bool star)
{
    muxc::ColumnDefinition c;

    c.Width(star ? mux::GridLengthHelper::FromValueAndType(
                                    1, mux::GridUnitType::Star)
                 : mux::GridLengthHelper::Auto());
    return c;
}

// Case-insensitive substring test; "needle_lower" is already lowercased.
    bool
contains_ci(winrt::hstring const &hay, std::wstring const &needle_lower)
{
    if (needle_lower.empty())
        return true;
    std::wstring h(hay.c_str());
    for (auto &c : h)
        c = (wchar_t)towlower(c);
    return h.find(needle_lower) != std::wstring::npos;
}

// Fill the search box suggestions from the history, filtered by its text.
    void
update_suggestions(muxc::AutoSuggestBox const &box)
{
    try
    {
        std::wstring q(box.Text().c_str());
        for (auto &c : q)
            c = (wchar_t)towlower(c);

        // AutoSuggestBox.ItemsSource needs IInspectable items, not hstring.
        auto out = winrt::single_threaded_vector<wf::IInspectable>();
        for (auto const &h : s_find_history)
        {
            if (contains_ci(h, q))
                out.Append(winrt::box_value(h));
            if (out.Size() >= 12)
                break;
        }
        box.ItemsSource(out);
    }
    catch (...)
    {
    }
}

// Build the bar once.
    bool
build_findbar(HWND hwnd, const WinUIFindLabels *lab)
{
    if (s_findbar_card)
        return true;
    if (!ensure_init(hwnd))
        return false;
    try
    {
        s_findbar_source = muxh::DesktopWindowXamlSource();
        s_findbar_source.Initialize(
                            winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd));
        s_findbar_root = muxc::Grid();
        s_findbar_root.Background(mux::Media::SolidColorBrush(
                                winrt::Windows::UI::Colors::Transparent()));

        s_findbar_card = muxc::Border();
        s_findbar_card.CornerRadius(mux::CornerRadius{8, 8, 8, 8});
        s_findbar_card.BorderThickness(mux::Thickness{1, 1, 1, 1});
        s_findbar_card.Padding(mux::Thickness{8, 6, 8, 6});
        {
            auto shadow = mux::Media::ThemeShadow();
            s_findbar_card.Shadow(shadow);
            s_findbar_card.Translation({0, 0, 24});
        }

        auto rows = muxc::Grid();
        {
            auto r0 = muxc::RowDefinition();
            r0.Height(mux::GridLengthHelper::Auto());
            auto r1 = muxc::RowDefinition();
            r1.Height(mux::GridLengthHelper::Auto());
            rows.RowDefinitions().Append(r0);
            rows.RowDefinitions().Append(r1);
        }
        rows.RowSpacing(4);

        // --- Find row: [expand][search *][prev][next][options][close]
        auto find_row = muxc::Grid();
        find_row.ColumnSpacing(2);
        find_row.ColumnDefinitions().Append(col(false));
        find_row.ColumnDefinitions().Append(col(true));
        find_row.ColumnDefinitions().Append(col(false));
        find_row.ColumnDefinitions().Append(col(false));
        find_row.ColumnDefinitions().Append(col(false));
        find_row.ColumnDefinitions().Append(col(false));

        // Expand/collapse the replace row.
        auto expand = icon_button(0xE70E, lab->expand);
        s_expand_icon = expand.Content().as<muxc::FontIcon>();
        expand.Click([](wf::IInspectable const &, mux::RoutedEventArgs const &)
        {
            set_replace_visible(s_replace_row.Visibility()
                                            == mux::Visibility::Collapsed);
        });
        muxc::Grid::SetColumn(expand, 0);
        find_row.Children().Append(expand);

        // Search box with a magnifier.
        s_find_box = muxc::AutoSuggestBox();
        s_find_box.PlaceholderText(lab->find);
        s_find_box.QueryIcon(muxc::SymbolIcon(muxc::Symbol::Find));
        s_find_box.Height(32);
        s_find_box.VerticalAlignment(mux::VerticalAlignment::Center);
        s_find_box.QuerySubmitted([](muxc::AutoSuggestBox const &,
                        muxc::AutoSuggestBoxQuerySubmittedEventArgs const &)
        {
            do_action(WINUI_FIND_NEXT);
        });
        s_find_box.KeyDown([](wf::IInspectable const &,
                              mux::Input::KeyRoutedEventArgs const &e)
        {
            if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                e.Handled(true);
                close_findbar();
            }
        });
        // Suggest from the search history as the user types.
        s_find_box.TextChanged([](muxc::AutoSuggestBox const &sender,
                        muxc::AutoSuggestBoxTextChangedEventArgs const &args)
        {
            try
            {
                if (args.Reason()
                        == muxc::AutoSuggestionBoxTextChangeReason::UserInput)
                    update_suggestions(sender);
            }
            catch (...)
            {
            }
        });
        s_find_box.SuggestionChosen([](muxc::AutoSuggestBox const &sender,
                    muxc::AutoSuggestBoxSuggestionChosenEventArgs const &args)
        {
            try
            {
                auto item = args.SelectedItem();
                if (item != nullptr)
                    sender.Text(winrt::unbox_value<winrt::hstring>(item));
            }
            catch (...)
            {
            }
        });
        muxc::Grid::SetColumn(s_find_box, 1);
        find_row.Children().Append(s_find_box);

        auto prev = icon_button(0xE74A, lab->find_prev);     // Up
        prev.Click([](wf::IInspectable const &, mux::RoutedEventArgs const &)
                    { do_action(WINUI_FIND_PREV); });
        muxc::Grid::SetColumn(prev, 2);
        find_row.Children().Append(prev);

        auto next = icon_button(0xE74B, lab->find_next);      // Down
        next.Click([](wf::IInspectable const &, mux::RoutedEventArgs const &)
                    { do_action(WINUI_FIND_NEXT); });
        muxc::Grid::SetColumn(next, 3);
        find_row.Children().Append(next);

        // Options button with a flyout holding the toggles.
        auto options = icon_button(0xE9E9, lab->options);     // Equalizer
        {
            muxc::MenuFlyout flyout;

            s_match_case = muxc::ToggleMenuFlyoutItem();
            s_match_case.Text(lab->match_case);
            flyout.Items().Append(s_match_case);

            s_whole_word = muxc::ToggleMenuFlyoutItem();
            s_whole_word.Text(lab->whole_word);
            flyout.Items().Append(s_whole_word);

            options.Flyout(flyout);
        }
        muxc::Grid::SetColumn(options, 4);
        find_row.Children().Append(options);

        auto close = icon_button(0xE711, lab->close);         // Cancel (X)
        close.Click([](wf::IInspectable const &, mux::RoutedEventArgs const &)
                    { close_findbar(); });
        muxc::Grid::SetColumn(close, 5);
        find_row.Children().Append(close);

        muxc::Grid::SetRow(find_row, 0);
        rows.Children().Append(find_row);

        // --- Replace row: [spacer][replace box *][Replace][Replace all]
        s_replace_row = muxc::Grid();
        s_replace_row.ColumnSpacing(2);
        s_replace_row.ColumnDefinitions().Append(col(false));
        s_replace_row.ColumnDefinitions().Append(col(true));
        s_replace_row.ColumnDefinitions().Append(col(false));
        s_replace_row.ColumnDefinitions().Append(col(false));
        s_replace_row.Visibility(mux::Visibility::Collapsed);
        {
            auto spacer = muxc::Border();
            spacer.Width(32);
            muxc::Grid::SetColumn(spacer, 0);
            s_replace_row.Children().Append(spacer);

            s_replace_box = muxc::TextBox();
            s_replace_box.PlaceholderText(lab->replace);
            s_replace_box.Height(32);
            s_replace_box.VerticalAlignment(mux::VerticalAlignment::Center);
            s_replace_box.KeyDown([](wf::IInspectable const &,
                                 mux::Input::KeyRoutedEventArgs const &e)
            {
                if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
                {
                    e.Handled(true);
                    do_action(WINUI_FIND_REPLACE);
                }
                else if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
                {
                    e.Handled(true);
                    close_findbar();
                }
            });
            muxc::Grid::SetColumn(s_replace_box, 1);
            s_replace_row.Children().Append(s_replace_box);

            auto one = text_button(lab->replace_one);
            one.Click([](wf::IInspectable const &, mux::RoutedEventArgs const &)
                        { do_action(WINUI_FIND_REPLACE); });
            muxc::Grid::SetColumn(one, 2);
            s_replace_row.Children().Append(one);

            auto all = text_button(lab->replace_all);
            all.Click([](wf::IInspectable const &, mux::RoutedEventArgs const &)
                        { do_action(WINUI_FIND_REPLACEALL); });
            muxc::Grid::SetColumn(all, 3);
            s_replace_row.Children().Append(all);
        }
        muxc::Grid::SetRow(s_replace_row, 1);
        rows.Children().Append(s_replace_row);

        s_findbar_card.Child(rows);
        s_findbar_root.Children().Append(s_findbar_card);

        // Escape closes the bar, even when a child (e.g. the search box)
        // already handled the key.
        s_findbar_root.AddHandler(mux::UIElement::KeyDownEvent(),
                winrt::box_value(mux::Input::KeyEventHandler(
                    [](wf::IInspectable const &,
                       mux::Input::KeyRoutedEventArgs const &e)
                    {
                        if (e.Key()
                                == winrt::Windows::System::VirtualKey::Escape)
                        {
                            e.Handled(true);
                            close_findbar();
                        }
                    })), true);

        // Theme and card colors, following the system light/dark setting.
        {
            bool dark = winui_system_is_dark();
            s_findbar_root.RequestedTheme(dark ? mux::ElementTheme::Dark
                                               : mux::ElementTheme::Light);
            winrt::Windows::UI::Color cc = dark
                    ? winrt::Windows::UI::Color{255, 0x2D, 0x2D, 0x2D}
                    : winrt::Windows::UI::Color{255, 0xFF, 0xFF, 0xFF};
            s_findbar_card.Background(mux::Media::SolidColorBrush(cc));
            s_findbar_card.BorderBrush(mux::Media::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
        }

        s_findbar_source.Content(s_findbar_root);
        s_findbar_hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(
                                s_findbar_source.SiteBridge().WindowId());
        s_findbar_source.SiteBridge().Hide();
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"Creating the find bar", e.code(), e.message().c_str());
        s_findbar_card = nullptr;
        return false;
    }
    catch (...)
    {
        set_error(L"Creating the find bar", E_FAIL);
        s_findbar_card = nullptr;
        return false;
    }
    return true;
}

} // namespace

/*
 * Show the find (replace != 0: find/replace) bar for window "hwnd", pre-filled
 * with "text".  "left"/"top"/"width" give the text area in client pixels; the
 * bar is centered horizontally at the top.  Returns FALSE when WinUI can't be
 * used.
 */
    int
WinUI_ShowFindBar(HWND hwnd, const WCHAR *text, int replace,
        int match_case, int whole_word, int left, int top, int width,
        const WinUIFindLabels *labels)
{
    if (!build_findbar(hwnd, labels))
        return FALSE;
    try
    {
        s_find_box.Text(text != NULL ? text : L"");
        if (s_match_case)
            s_match_case.IsChecked(match_case != 0);
        if (s_whole_word)
            s_whole_word.IsChecked(whole_word != 0);
        set_replace_visible(replace != 0);

        WinUI_FindBarSetPos(left, top, width);

        if (s_findbar_hwnd != NULL)
            SetFocus(s_findbar_hwnd);
        s_find_box.Focus(mux::FocusState::Programmatic);
    }
    catch (...)
    {
        return FALSE;
    }
    return TRUE;
}

/*
 * Position the find bar: a wide bar centered horizontally at the top of the
 * text area, like Notepad.
 */
    void
WinUI_FindBarSetPos(int left, int top, int width)
{
    if (!s_findbar_card)
        return;
    s_findbar_left = left;
    s_findbar_top = top;
    s_findbar_width = width;
    try
    {
        float   inf = std::numeric_limits<float>::infinity();
        double  scale = GetDpiForWindow(s_parent) / 96.0;
        int     margin = (int)(8 * scale);

        // Wide bar, but not wider than the text area.
        int     barw = width - 2 * margin;
        int     maxw = (int)(820 * scale);
        int     minw = (int)(380 * scale);
        if (barw > maxw)
            barw = maxw;
        if (barw < minw)
            barw = minw;

        // Fix the card width so the search box stretches, then measure height.
        double  barw_dip = barw / scale;
        s_findbar_card.Width(barw_dip);
        s_findbar_card.Measure(wf::Size{(float)barw_dip, inf});
        int     barh = (int)(s_findbar_card.DesiredSize().Height * scale + 0.99);
        if (barh <= 0)
            barh = (int)(48 * scale);

        int     x = left + (width - barw) / 2;
        if (x < left)
            x = left;
        int     y = top + margin;

        s_findbar_source.SiteBridge().MoveAndResize(
                winrt::Windows::Graphics::RectInt32{x, y, barw, barh});
        s_findbar_source.SiteBridge().Show();
    }
    catch (...)
    {
    }
}

    void
WinUI_HideFindBar(void)
{
    if (!s_findbar_source)
        return;
    try
    {
        s_findbar_source.SiteBridge().Hide();
    }
    catch (...)
    {
    }
}

    int
WinUI_FindBarVisible(void)
{
    return s_findbar_card != nullptr;
}

/*
 * Set the search-history suggestions (newest first), from the C side.
 */
    void
WinUI_FindBarSetHistory(const WCHAR **items, int count)
{
    try
    {
        s_find_history.clear();
        for (int i = 0; i < count; ++i)
            if (items[i] != NULL)
                s_find_history.emplace_back(items[i]);
    }
    catch (...)
    {
    }
}

// ---------------------------------------------------------------------------
// Modal dialog (ContentDialog), like Notepad's "Save changes?".
// ---------------------------------------------------------------------------

namespace {

struct DialogState
{
    bool    done = false;
    int     result = 0;
};

// Return the button label without the '&' accelerators ("&&" -> "&"), and
// the access-key letter (after a single '&') in "key", 0 when there is none.
    winrt::hstring
label_and_key(const WCHAR *s, WCHAR *key)
{
    std::wstring o;

    *key = 0;
    for (; s != NULL && *s != L'\0'; ++s)
    {
        if (*s != L'&')
            o += *s;
        else if (*(s + 1) == L'&')
        {
            o += L'&';
            ++s;
        }
        else if (*(s + 1) != L'\0')
        {
            if (*key == 0)
                *key = *(s + 1);
            // keep the letter itself in the label
        }
    }
    return winrt::hstring(o);
}

// Pump messages until "pred" is true or the timeout expires.
    template <typename Pred>
    bool
pump_until(Pred pred, DWORD timeout_ms)
{
    DWORD   start = GetTickCount();
    MSG     msg;

    while (!pred())
    {
        if (GetTickCount() - start > timeout_ms)
            return false;
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                PostQuitMessage((int)msg.wParam);
                return false;
            }
            if (s_pretranslate != NULL && s_pretranslate(&msg))
                continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        else
            MsgWaitForMultipleObjectsEx(0, NULL, 50, QS_ALLINPUT,
                                                    MWMO_INPUTAVAILABLE);
    }
    return true;
}

} // namespace

/*
 * Show a modal dialog with "message", up to "nbuttons" buttons (any number),
 * and an optional input field.  Buttons and the field live inside the
 * ContentDialog's content, so there is no 3-button limit.
 * Returns the 1-based index of the chosen button, or the last button on
 * Escape.  "inout_text" (when not NULL) holds the field text in and out.
 * Returns -1 only when WinUI itself is unavailable.
 */
    int
WinUI_ShowDialog(HWND hwnd, const WCHAR *title, const WCHAR *message,
        const WCHAR **buttons, int nbuttons, int dfltbutton,
        WCHAR *inout_text, int text_len)
{
    if (s_in_popup || nbuttons < 1 || !ensure_init(hwnd))
        return -1;

    HWND    focus = GetFocus();
    int     result = -1;

    s_in_popup = true;
    try
    {
        if (!s_dialog_source)
        {
            s_dialog_source = muxh::DesktopWindowXamlSource();
            s_dialog_source.Initialize(
                            winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd));
            s_dialog_root = muxc::Grid();
            s_dialog_source.Content(s_dialog_root);
            s_dialog_hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(
                                s_dialog_source.SiteBridge().WindowId());
        }

        RECT rc;
        GetClientRect(hwnd, &rc);
        s_dialog_source.SiteBridge().MoveAndResize(
                winrt::Windows::Graphics::RectInt32{0, 0,
                        rc.right > 0 ? rc.right : 1,
                        rc.bottom > 0 ? rc.bottom : 1});
        s_dialog_source.SiteBridge().Show();
        SetFocus(s_dialog_hwnd);

        muxc::ContentDialog dlg;

        auto state = std::make_shared<DialogState>();
        winrt::weak_ref<muxc::ContentDialog> weak = dlg;

        // Theme (light/dark) following the system setting.
        bool dark = winui_system_is_dark();

        // Top block: the title, the message and the optional input field.
        auto content = muxc::StackPanel();
        content.Spacing(10);
        content.Padding(mux::Thickness{24, 22, 24, 22});

        if (title != NULL && *title != L'\0')
        {
            muxc::TextBlock titleblock;

            titleblock.Text(title);
            titleblock.FontSize(20);
            titleblock.FontWeight(
                    winrt::Windows::UI::Text::FontWeights::SemiBold());
            titleblock.Margin(mux::Thickness{0, 0, 0, 6});
            content.Children().Append(titleblock);
        }

        muxc::TextBlock tb;
        tb.Text(message != NULL ? message : L"");
        tb.TextWrapping(mux::TextWrapping::WrapWholeWords);
        content.Children().Append(tb);

        muxc::TextBox input{nullptr};
        if (inout_text != NULL)
        {
            input = muxc::TextBox();
            input.Text(inout_text);
            content.Children().Append(input);
        }

        // The buttons, right-aligned.
        auto row = muxc::StackPanel();
        row.Orientation(muxc::Orientation::Horizontal);
        row.HorizontalAlignment(mux::HorizontalAlignment::Right);
        row.Spacing(8);

        mux::Style accent{nullptr};
        try
        {
            accent = mux::Application::Current().Resources()
                        .Lookup(winrt::box_value(L"AccentButtonStyle"))
                        .as<mux::Style>();
        }
        catch (...)
        {
        }

        muxc::Button default_btn{nullptr};
        for (int i = 0; i < nbuttons; ++i)
        {
            muxc::Button b;
            WCHAR	key;

            b.Content(winrt::box_value(label_and_key(buttons[i], &key)));
            b.MinWidth(84);
            if (key != 0)
                b.AccessKey(winrt::hstring(&key, 1));
            if (i == dfltbutton - 1)
            {
                if (accent != nullptr)
                    b.Style(accent);
                default_btn = b;
            }
            int idx = i + 1;
            b.Click([state, idx, weak](wf::IInspectable const &,
                                       mux::RoutedEventArgs const &)
            {
                state->result = idx;
                state->done = true;
                if (auto d = weak.get())
                    d.Hide();
            });
            row.Children().Append(b);
        }

        // Bottom block: the button bar, on a slightly different background.
        auto footer = muxc::Border();
        footer.Padding(mux::Thickness{24, 18, 24, 18});
        footer.BorderThickness(mux::Thickness{0, 1, 0, 0});
        footer.CornerRadius(mux::CornerRadius{0, 0, 8, 8});
        {
            winrt::Windows::UI::Color fc = dark
                ? winrt::Windows::UI::ColorHelper::FromArgb(255, 32, 32, 32)
                : winrt::Windows::UI::ColorHelper::FromArgb(255, 243, 243, 243);
            footer.Background(mux::Media::SolidColorBrush(fc));
            footer.BorderBrush(mux::Media::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
        }
        footer.Child(row);

        // Stack the two blocks.
        auto outer = muxc::Grid();
        {
            auto r0 = muxc::RowDefinition();
            r0.Height(mux::GridLengthHelper::Auto());
            auto r1 = muxc::RowDefinition();
            r1.Height(mux::GridLengthHelper::Auto());
            outer.RowDefinitions().Append(r0);
            outer.RowDefinitions().Append(r1);
        }
        muxc::Grid::SetRow(content, 0);
        muxc::Grid::SetRow(footer, 1);
        outer.Children().Append(content);
        outer.Children().Append(footer);
        outer.RequestedTheme(dark ? mux::ElementTheme::Dark
                                  : mux::ElementTheme::Light);

        // Remove the dialog's own content padding so the footer reaches the
        // edges of the dialog, and drop its minimum height so a short dialog
        // hugs its content instead of leaving a big blank area.
        try
        {
            dlg.Resources().Insert(winrt::box_value(L"ContentDialogPadding"),
                                winrt::box_value(mux::Thickness{0, 0, 0, 0}));
            dlg.Resources().Insert(winrt::box_value(L"ContentDialogMinHeight"),
                                winrt::box_value(0.0));
        }
        catch (...)
        {
        }
        dlg.Content(outer);
        dlg.Opened([default_btn](muxc::ContentDialog const &,
                            muxc::ContentDialogOpenedEventArgs const &)
        {
            if (default_btn != nullptr)
                default_btn.Focus(mux::FocusState::Keyboard);
        });

        // Enter triggers the default button, Escape the last (cancel) one.
        int dflt = (dfltbutton >= 1 && dfltbutton <= nbuttons) ? dfltbutton : 1;
        outer.KeyDown([state, weak, dflt, nbuttons](wf::IInspectable const &,
                        mux::Input::KeyRoutedEventArgs const &e)
        {
            auto key = e.Key();
            if (key == winrt::Windows::System::VirtualKey::Enter)
            {
                e.Handled(true);
                state->result = dflt;
                state->done = true;
                if (auto d = weak.get())
                    d.Hide();
            }
            else if (key == winrt::Windows::System::VirtualKey::Escape)
            {
                e.Handled(true);
                state->result = nbuttons;
                state->done = true;
                if (auto d = weak.get())
                    d.Hide();
            }
        });

        if (!pump_until([]() { return s_dialog_root.XamlRoot() != nullptr; },
                                                                    3000))
            throw winrt::hresult_error(E_FAIL, L"no XamlRoot");
        dlg.XamlRoot(s_dialog_root.XamlRoot());

        auto op = dlg.ShowAsync();
        op.Completed([state, nbuttons](
                wf::IAsyncOperation<muxc::ContentDialogResult> const &,
                wf::AsyncStatus)
        {
            // Closed without one of our buttons (should not normally happen).
            if (!state->done)
            {
                state->result = nbuttons;
                state->done = true;
            }
        });

        if (input != nullptr)
            input.Focus(mux::FocusState::Programmatic);

        if (pump_until([state]() { return state->done; }, INFINITE))
        {
            result = state->result;
            // Copy the field text back (only when a real choice was made).
            if (input != nullptr && inout_text != NULL && text_len > 0
                                                            && result >= 1)
            {
                winrt::hstring t = input.Text();
                int n = (int)t.size();
                if (n > text_len - 1)
                    n = text_len - 1;
                wmemcpy(inout_text, t.c_str(), n);
                inout_text[n] = L'\0';
            }
        }
        else
        {
            try { dlg.Hide(); } catch (...) {}
            result = nbuttons;
        }
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"Showing the dialog", e.code(), e.message().c_str());
        result = -1;
    }
    catch (...)
    {
        set_error(L"Showing the dialog", E_FAIL);
        result = -1;
    }

    try
    {
        if (s_dialog_source)
            s_dialog_source.SiteBridge().Hide();
    }
    catch (...)
    {
    }
    s_in_popup = false;

    if (focus != NULL && IsWindow(focus))
        SetFocus(focus);
    else if (s_parent != NULL)
        SetFocus(s_parent);
    return result;
}

// ---------------------------------------------------------------------------
// "About" dialog (Win11 ContentDialog with the joli.png icon + intro text).
// ---------------------------------------------------------------------------

/*
 * Show a modern About dialog: the "image_path" icon, a "title", an "info"
 * block (version, author, licence) and "hints" shown in a monospace font,
 * with a Close button.  Returns TRUE when shown, FALSE when WinUI is
 * unavailable.  All text may contain '\n' line breaks.
 */
    int
WinUI_ShowAbout(HWND hwnd, const WCHAR *image_path, const WCHAR *title,
        const WCHAR *info, const WCHAR *hints, const WCHAR *close_label,
        const WCHAR *link_text, const WCHAR *link_url)
{
    if (s_in_popup || !ensure_init(hwnd))
        return FALSE;

    HWND    focus = GetFocus();
    int     ok = FALSE;

    s_in_popup = true;
    try
    {
        if (!s_dialog_source)
        {
            s_dialog_source = muxh::DesktopWindowXamlSource();
            s_dialog_source.Initialize(
                            winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd));
            s_dialog_root = muxc::Grid();
            s_dialog_source.Content(s_dialog_root);
            s_dialog_hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(
                                s_dialog_source.SiteBridge().WindowId());
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        s_dialog_source.SiteBridge().MoveAndResize(
                winrt::Windows::Graphics::RectInt32{0, 0,
                        rc.right > 0 ? rc.right : 1,
                        rc.bottom > 0 ? rc.bottom : 1});
        s_dialog_source.SiteBridge().Show();
        SetFocus(s_dialog_hwnd);

        bool dark = winui_system_is_dark();
        auto col = [](BYTE r, BYTE g, BYTE b)
        {
            return mux::Media::SolidColorBrush(
                        winrt::Windows::UI::ColorHelper::FromArgb(255, r, g, b));
        };
        auto ink  = dark ? col(234, 239, 236) : col(31, 42, 37);
        auto grey = dark ? col(169, 180, 175) : col(91, 107, 99);

        muxc::ContentDialog dlg;

        auto content = muxc::StackPanel();
        content.Spacing(14);
        content.Padding(mux::Thickness{24, 22, 24, 20});

        // Header: icon + title/info.
        auto header = muxc::StackPanel();
        header.Orientation(muxc::Orientation::Horizontal);
        header.Spacing(18);
        header.VerticalAlignment(mux::VerticalAlignment::Center);

        if (image_path != NULL && *image_path != L'\0')
        {
            try
            {
                std::wstring u(image_path);
                for (auto &c : u)
                    if (c == L'\\')
                        c = L'/';
                winrt::Windows::Foundation::Uri uri(
                                    winrt::hstring(L"file:///" + u));
                mux::Media::Imaging::BitmapImage bmp(uri);
                muxc::Image img;
                img.Width(84);
                img.Height(84);
                img.Source(bmp);
                header.Children().Append(img);
            }
            catch (...)
            {
            }
        }

        auto headtext = muxc::StackPanel();
        headtext.Spacing(4);
        headtext.VerticalAlignment(mux::VerticalAlignment::Center);
        if (title != NULL && *title != L'\0')
        {
            muxc::TextBlock t;
            t.Text(title);
            t.FontSize(24);
            t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            t.Foreground(ink);
            headtext.Children().Append(t);
        }
        if (info != NULL && *info != L'\0')
        {
            muxc::TextBlock t;
            t.Text(info);
            t.FontSize(14);
            t.Foreground(grey);
            t.TextWrapping(mux::TextWrapping::Wrap);
            headtext.Children().Append(t);
        }
        // Dedicated repository link below the description.
        if (link_url != NULL && *link_url != L'\0')
        {
            muxc::HyperlinkButton gh;
            gh.Content(winrt::box_value(winrt::hstring(
                    link_text != NULL ? link_text : link_url)));
            gh.Padding(mux::Thickness{0, 2, 0, 0});
            gh.HorizontalAlignment(mux::HorizontalAlignment::Left);
            try
            {
                gh.NavigateUri(winrt::Windows::Foundation::Uri(link_url));
            }
            catch (...) {}
            headtext.Children().Append(gh);
        }
        header.Children().Append(headtext);
        content.Children().Append(header);

        if (hints != NULL && *hints != L'\0')
        {
            muxc::TextBlock h;
            h.Text(hints);
            h.FontSize(13);
            h.FontFamily(mux::Media::FontFamily(
                            L"Cascadia Mono, Consolas, Courier New, monospace"));
            h.Foreground(grey);
            content.Children().Append(h);
        }

        // Footer with a Close button.
        auto state = std::make_shared<DialogState>();
        winrt::weak_ref<muxc::ContentDialog> weak = dlg;

        muxc::Button closeb;
        closeb.Content(winrt::box_value(winrt::hstring(
                        close_label != NULL ? close_label : L"Close")));
        closeb.MinWidth(96);
        try
        {
            closeb.Style(mux::Application::Current().Resources()
                    .Lookup(winrt::box_value(L"AccentButtonStyle"))
                    .as<mux::Style>());
        }
        catch (...) {}
        closeb.Click([state, weak](wf::IInspectable const &,
                                   mux::RoutedEventArgs const &)
        {
            state->result = 1;
            state->done = true;
            if (auto d = weak.get())
                d.Hide();
        });

        auto footer = muxc::Border();
        footer.Padding(mux::Thickness{24, 16, 24, 16});
        footer.BorderThickness(mux::Thickness{0, 1, 0, 0});
        footer.CornerRadius(mux::CornerRadius{0, 0, 8, 8});
        footer.Background(dark ? col(32, 32, 32) : col(243, 243, 243));
        footer.BorderBrush(mux::Media::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
        {
            auto row = muxc::StackPanel();
            row.Orientation(muxc::Orientation::Horizontal);
            row.HorizontalAlignment(mux::HorizontalAlignment::Right);
            row.Children().Append(closeb);
            footer.Child(row);
        }

        auto outer = muxc::Grid();
        {
            auto r0 = muxc::RowDefinition();
            r0.Height(mux::GridLengthHelper::Auto());
            auto r1 = muxc::RowDefinition();
            r1.Height(mux::GridLengthHelper::Auto());
            outer.RowDefinitions().Append(r0);
            outer.RowDefinitions().Append(r1);
        }
        muxc::Grid::SetRow(content, 0);
        muxc::Grid::SetRow(footer, 1);
        outer.Children().Append(content);
        outer.Children().Append(footer);
        outer.RequestedTheme(dark ? mux::ElementTheme::Dark
                                  : mux::ElementTheme::Light);

        try
        {
            dlg.Resources().Insert(winrt::box_value(L"ContentDialogPadding"),
                                winrt::box_value(mux::Thickness{0, 0, 0, 0}));
            dlg.Resources().Insert(winrt::box_value(L"ContentDialogMinHeight"),
                                winrt::box_value(0.0));
        }
        catch (...) {}
        dlg.Content(outer);
        outer.KeyDown([state, weak](wf::IInspectable const &,
                        mux::Input::KeyRoutedEventArgs const &e)
        {
            if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                e.Handled(true);
                state->result = 1;
                state->done = true;
                if (auto d = weak.get())
                    d.Hide();
            }
        });

        if (!pump_until([]() { return s_dialog_root.XamlRoot() != nullptr; },
                                                                    3000))
            throw winrt::hresult_error(E_FAIL, L"no XamlRoot");
        dlg.XamlRoot(s_dialog_root.XamlRoot());

        auto op = dlg.ShowAsync();
        op.Completed([state](wf::IAsyncOperation<muxc::ContentDialogResult>
                                                const &, wf::AsyncStatus)
        {
            state->done = true;
        });
        dlg.Opened([closeb](muxc::ContentDialog const &,
                            muxc::ContentDialogOpenedEventArgs const &)
        {
            closeb.Focus(mux::FocusState::Keyboard);
        });

        pump_until([state]() { return state->done; }, INFINITE);
        ok = TRUE;
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"Showing the About dialog", e.code(), e.message().c_str());
        ok = FALSE;
    }
    catch (...)
    {
        ok = FALSE;
    }

    try
    {
        if (s_dialog_source)
            s_dialog_source.SiteBridge().Hide();
    }
    catch (...) {}
    s_in_popup = false;

    if (focus != NULL && IsWindow(focus))
        SetFocus(focus);
    else if (s_parent != NULL)
        SetFocus(s_parent);
    return ok;
}

/*
 * Modern font picker.  Returns 1 on accept (io updated), 0 on cancel, -1 when
 * the WinUI runtime is not available (caller falls back to ChooseFontW).
 */
    int
WinUI_ShowFontDialog(HWND hwnd, WinUIFontChoice *io)
{
    if (io == NULL || s_in_popup || !ensure_init(hwnd))
        return -1;

    HWND    focus = GetFocus();
    int     rc = -1;

    s_in_popup = true;
    try
    {
        if (!s_dialog_source)
        {
            s_dialog_source = muxh::DesktopWindowXamlSource();
            s_dialog_source.Initialize(
                            winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd));
            s_dialog_root = muxc::Grid();
            s_dialog_source.Content(s_dialog_root);
            s_dialog_hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(
                                s_dialog_source.SiteBridge().WindowId());
        }
        RECT r;
        GetClientRect(hwnd, &r);
        s_dialog_source.SiteBridge().MoveAndResize(
                winrt::Windows::Graphics::RectInt32{0, 0,
                        r.right > 0 ? r.right : 1, r.bottom > 0 ? r.bottom : 1});
        s_dialog_source.SiteBridge().Show();
        SetFocus(s_dialog_hwnd);

        bool dark = winui_system_is_dark();

        muxc::ContentDialog dlg;

        // Font family list.
        muxc::ListView list;
        list.Height(240);
        list.SelectionMode(muxc::ListViewSelectionMode::Single);
        int selidx = 0;
        for (int i = 0; i < io->family_count; ++i)
        {
            // Show each family name rendered in its own font (modern pickers).
            winrt::hstring fam(io->families[i]);
            muxc::TextBlock item;
            item.Text(fam);
            try { item.FontFamily(mux::Media::FontFamily(fam)); }
            catch (...) {}
            item.FontSize(16);
            list.Items().Append(item);
            if (io->family[0] != L'\0'
                    && wcscmp(io->families[i], io->family) == 0)
                selidx = i;
        }
        if (io->family_count > 0)
        {
            list.SelectedIndex(selidx);
            list.ScrollIntoView(list.Items().GetAt(selidx));
        }

        // Editable size dropdown with the usual preset sizes (like Notepad).
        muxc::ComboBox sizeBox;
        sizeBox.IsEditable(true);
        sizeBox.Width(110);
        {
            static const int presets[] = { 8, 9, 10, 11, 12, 13, 14, 16, 18,
                                        20, 22, 24, 28, 32, 36, 48, 72 };
            int cur = io->size > 0 ? io->size : 11;
            int match = -1;
            for (int k = 0; k < (int)(sizeof(presets) / sizeof(presets[0])); ++k)
            {
                sizeBox.Items().Append(
                                winrt::box_value(winrt::to_hstring(presets[k])));
                if (presets[k] == cur)
                    match = k;
            }
            if (match >= 0)
                sizeBox.SelectedIndex(match);
            else
                sizeBox.Text(winrt::to_hstring(cur));
        }

        // No Bold/Italic boxes: like the GTK font chooser, each family is shown
        // in its own style and the weight/italic of the current font are kept as
        // is (change them with syntax highlighting or ":set guifont").  The
        // preview reflects the current style.
        muxc::TextBlock preview;
        preview.Text(io->preview_text != NULL ? io->preview_text
                            : L"The quick brown fox jumps over the lazy dog");
        preview.TextWrapping(mux::TextWrapping::Wrap);
        preview.FontWeight(io->bold
                ? winrt::Windows::UI::Text::FontWeights::Bold()
                : winrt::Windows::UI::Text::FontWeights::Normal());
        preview.FontStyle(io->italic
                ? winrt::Windows::UI::Text::FontStyle::Italic
                : winrt::Windows::UI::Text::FontStyle::Normal);

        auto refresh = [list, sizeBox, preview]()
        {
            try
            {
                auto sel = list.SelectedItem();
                if (auto tb = sel.try_as<muxc::TextBlock>())
                    preview.FontFamily(mux::Media::FontFamily(tb.Text()));
                int sz = (int)wcstol(sizeBox.Text().c_str(), NULL, 10);
                if (sz < 6)
                    sz = 11;
                if (sz > 72)
                    sz = 72;
                preview.FontSize((double)sz);
            }
            catch (...) {}
        };
        refresh();

        list.SelectionChanged([refresh](wf::IInspectable const &,
                    muxc::SelectionChangedEventArgs const &) { refresh(); });
        sizeBox.SelectionChanged([refresh](wf::IInspectable const &,
                    muxc::SelectionChangedEventArgs const &) { refresh(); });
        sizeBox.TextSubmitted([refresh](muxc::ComboBox const &,
                    muxc::ComboBoxTextSubmittedEventArgs const &) { refresh(); });

        auto content = muxc::StackPanel();
        content.Spacing(12);
        content.Padding(mux::Thickness{24, 20, 24, 16});
        if (io->title != NULL && *io->title != L'\0')
        {
            muxc::TextBlock t;
            t.Text(io->title);
            t.FontSize(20);
            t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            content.Children().Append(t);
        }
        content.Children().Append(list);

        auto row = muxc::StackPanel();
        row.Orientation(muxc::Orientation::Horizontal);
        row.Spacing(16);
        row.VerticalAlignment(mux::VerticalAlignment::Bottom);
        {
            auto sizecol = muxc::StackPanel();
            sizecol.Spacing(4);
            muxc::TextBlock sl;
            sl.Text(io->size_label != NULL ? io->size_label : L"Size");
            sl.FontSize(12);
            sizecol.Children().Append(sl);
            sizecol.Children().Append(sizeBox);
            row.Children().Append(sizecol);
        }
        content.Children().Append(row);

        auto card = muxc::Border();
        card.Padding(mux::Thickness{12, 10, 12, 10});
        card.CornerRadius(mux::CornerRadius{6, 6, 6, 6});
        card.BorderThickness(mux::Thickness{1, 1, 1, 1});
        card.BorderBrush(mux::Media::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
        card.Child(preview);
        content.Children().Append(card);

        auto state = std::make_shared<DialogState>();
        winrt::weak_ref<muxc::ContentDialog> weak = dlg;

        muxc::Button okb;
        okb.Content(winrt::box_value(winrt::hstring(
                    io->ok_label != NULL ? io->ok_label : L"OK")));
        okb.MinWidth(96);
        try
        {
            okb.Style(mux::Application::Current().Resources()
                    .Lookup(winrt::box_value(L"AccentButtonStyle"))
                    .as<mux::Style>());
        }
        catch (...) {}
        muxc::Button cancelb;
        cancelb.Content(winrt::box_value(winrt::hstring(
                    io->cancel_label != NULL ? io->cancel_label : L"Cancel")));
        cancelb.MinWidth(96);

        okb.Click([state, weak, io, list, sizeBox](
                        wf::IInspectable const &, mux::RoutedEventArgs const &)
        {
            try
            {
                auto sel = list.SelectedItem();
                if (auto tb = sel.try_as<muxc::TextBlock>())
                {
                    auto fam = tb.Text();
                    wcsncpy(io->family, fam.c_str(), LF_FACESIZE - 1);
                    io->family[LF_FACESIZE - 1] = L'\0';
                }
                int sz = (int)wcstol(sizeBox.Text().c_str(), NULL, 10);
                if (sz >= 6 && sz <= 72)
                    io->size = sz;
                // io->bold / io->italic are left as they were: no style boxes.
            }
            catch (...) {}
            state->result = 1;
            state->done = true;
            if (auto d = weak.get())
                d.Hide();
        });
        cancelb.Click([state, weak](wf::IInspectable const &,
                                    mux::RoutedEventArgs const &)
        {
            state->result = 0;
            state->done = true;
            if (auto d = weak.get())
                d.Hide();
        });

        auto footer = muxc::Border();
        footer.Padding(mux::Thickness{24, 14, 24, 14});
        footer.BorderThickness(mux::Thickness{0, 1, 0, 0});
        footer.CornerRadius(mux::CornerRadius{0, 0, 8, 8});
        footer.Background(mux::Media::SolidColorBrush(dark
                ? winrt::Windows::UI::ColorHelper::FromArgb(255, 32, 32, 32)
                : winrt::Windows::UI::ColorHelper::FromArgb(255, 243, 243, 243)));
        footer.BorderBrush(mux::Media::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
        {
            auto frow = muxc::StackPanel();
            frow.Orientation(muxc::Orientation::Horizontal);
            frow.Spacing(10);
            frow.HorizontalAlignment(mux::HorizontalAlignment::Right);
            frow.Children().Append(cancelb);
            frow.Children().Append(okb);
            footer.Child(frow);
        }

        auto outer = muxc::Grid();
        {
            auto r0 = muxc::RowDefinition();
            r0.Height(mux::GridLengthHelper::Auto());
            auto r1 = muxc::RowDefinition();
            r1.Height(mux::GridLengthHelper::Auto());
            outer.RowDefinitions().Append(r0);
            outer.RowDefinitions().Append(r1);
        }
        muxc::Grid::SetRow(content, 0);
        muxc::Grid::SetRow(footer, 1);
        outer.Children().Append(content);
        outer.Children().Append(footer);
        outer.RequestedTheme(dark ? mux::ElementTheme::Dark
                                  : mux::ElementTheme::Light);

        try
        {
            dlg.Resources().Insert(winrt::box_value(L"ContentDialogPadding"),
                                winrt::box_value(mux::Thickness{0, 0, 0, 0}));
            dlg.Resources().Insert(winrt::box_value(L"ContentDialogMinHeight"),
                                winrt::box_value(0.0));
        }
        catch (...) {}
        dlg.Content(outer);
        outer.KeyDown([state, weak](wf::IInspectable const &,
                        mux::Input::KeyRoutedEventArgs const &e)
        {
            if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                e.Handled(true);
                state->result = 0;
                state->done = true;
                if (auto d = weak.get())
                    d.Hide();
            }
        });

        if (!pump_until([]() { return s_dialog_root.XamlRoot() != nullptr; },
                                                                    3000))
            throw winrt::hresult_error(E_FAIL, L"no XamlRoot");
        dlg.XamlRoot(s_dialog_root.XamlRoot());

        auto op = dlg.ShowAsync();
        op.Completed([state](wf::IAsyncOperation<muxc::ContentDialogResult>
                                                const &, wf::AsyncStatus)
        {
            state->done = true;
        });
        // Once open, scroll the current font into view and focus the list so
        // it is both visible and keyboard-navigable from the selected item.
        dlg.Opened([list, selidx](muxc::ContentDialog const &,
                        muxc::ContentDialogOpenedEventArgs const &)
        {
            try
            {
                if (selidx >= 0 && (uint32_t)selidx < list.Items().Size())
                {
                    list.ScrollIntoView(list.Items().GetAt(selidx));
                    list.UpdateLayout();
                }
                list.Focus(mux::FocusState::Programmatic);
            }
            catch (...) {}
        });

        pump_until([state]() { return state->done; }, INFINITE);
        rc = state->result;
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"Font dialog", e.code(), e.message().c_str());
        rc = -1;
    }
    catch (...)
    {
        rc = -1;
    }

    try
    {
        if (s_dialog_source)
            s_dialog_source.SiteBridge().Hide();
    }
    catch (...) {}
    s_in_popup = false;

    if (focus != NULL && IsWindow(focus))
        SetFocus(focus);
    else if (s_parent != NULL)
        SetFocus(s_parent);
    return rc;
}

// ---------------------------------------------------------------------------
// Custom title bar: extend content into the title bar (like Notepad), the
// system keeps drawing the min/max/close buttons.
// ---------------------------------------------------------------------------

/*
 * Apply the light or dark colour scheme to the title bar caption buttons and
 * to the tab strip (matching the Windows 11 Notepad look).  Only the objects
 * that already exist are touched, so this is safe to call during init and
 * again when the system theme changes.
 */
    static void
winui_apply_colors(bool dark)
{
    // Like Notepad: the tab strip is the darker/duller shade, the selected
    // tab (and the menu/toolbar) the lighter one it extends into.
    winrt::Windows::UI::Color strip = dark
            ? winrt::Windows::UI::Color{255, 0x1E, 0x1E, 0x1E}
            : winrt::Windows::UI::Color{255, 0xE8, 0xE8, 0xE8};
    winrt::Windows::UI::Color sel = dark
            ? winrt::Windows::UI::Color{255, 0x2B, 0x2B, 0x2B}
            : winrt::Windows::UI::Color{255, 0xF8, 0xF8, 0xF8};
    winrt::Windows::UI::Color hover = dark
            ? winrt::Windows::UI::Color{255, 0x2A, 0x2A, 0x2A}
            : winrt::Windows::UI::Color{255, 0xEF, 0xEF, 0xEF};
    auto clear = winrt::Windows::UI::Colors::Transparent();

    // Caption buttons and the title bar background.
    if (s_appwindow && s_titlebar_active)
    {
        auto tb = s_appwindow.TitleBar();
        winrt::Windows::UI::Color fg = dark
                ? winrt::Windows::UI::ColorHelper::FromArgb(255, 234, 239, 236)
                : winrt::Windows::UI::ColorHelper::FromArgb(255, 31, 42, 37);
        winrt::Windows::UI::Color bhover = dark
                ? winrt::Windows::UI::ColorHelper::FromArgb(255, 0x3D, 0x3D, 0x3D)
                : winrt::Windows::UI::ColorHelper::FromArgb(255, 0xDA, 0xDA, 0xDA);
        winrt::Windows::UI::Color bpress = dark
                ? winrt::Windows::UI::ColorHelper::FromArgb(255, 0x45, 0x45, 0x45)
                : winrt::Windows::UI::ColorHelper::FromArgb(255, 0xCF, 0xCF, 0xCF);
        // With ExtendsContentIntoTitleBar the title bar BackgroundColor is not
        // honoured behind the caption buttons, so colour the button background
        // itself to match the strip instead of leaving it transparent.
        tb.ButtonBackgroundColor(strip);
        tb.ButtonInactiveBackgroundColor(strip);
        tb.ButtonForegroundColor(fg);
        tb.ButtonInactiveForegroundColor(fg);
        tb.ButtonHoverBackgroundColor(bhover);
        tb.ButtonHoverForegroundColor(fg);
        tb.ButtonPressedBackgroundColor(bpress);
        tb.ButtonPressedForegroundColor(fg);
        tb.BackgroundColor(strip);
        tb.InactiveBackgroundColor(strip);
    }

    // Tab strip: a flat strip with a contrasting selected tab, like Notepad.
    if (s_tabview && s_tabbar_root)
    {
        s_tabbar_root.RequestedTheme(dark ? mux::ElementTheme::Dark
                                          : mux::ElementTheme::Light);
        s_tabbar_root.Background(mux::Media::SolidColorBrush(strip));
        auto setbrush = [](const wchar_t *key, winrt::Windows::UI::Color col)
        {
            s_tabview.Resources().Insert(
                    winrt::box_value(winrt::hstring{key}),
                    mux::Media::SolidColorBrush(col));
        };
        setbrush(L"TabViewBackground", clear);
        setbrush(L"TabViewItemHeaderBackground", clear);
        setbrush(L"TabViewItemHeaderBackgroundPointerOver", hover);
        setbrush(L"TabViewItemHeaderBackgroundPressed", sel);
        setbrush(L"TabViewItemHeaderBackgroundSelected", sel);
        setbrush(L"TabViewItemHeaderBackgroundSelectedPointerOver", sel);
        setbrush(L"TabViewItemHeaderBackgroundSelectedPressed", sel);
    }
}

/*
 * Re-apply the colour scheme after a system light/dark theme change.  The tab
 * items are flagged for recreation so they pick up the new brushes.
 */
    void
WinUI_UpdateTheme(void)
{
    try
    {
        winui_apply_colors(winui_system_is_dark());
        if (s_menubar_root)
            set_menubar_colors();
        s_tab_force_rebuild = true;
    }
    catch (...)
    {
    }
}

/*
 * Turn on the extended title bar for "hwnd".  Returns TRUE on success and
 * sets *height to the title bar height and *left/*right to the insets taken
 * by the system caption buttons (device pixels).
 */
    int
WinUI_TitleBarInit(HWND hwnd, int *height, int *left, int *right)
{
    *height = 0;
    *left = 0;
    *right = 0;
    if (!ensure_init(hwnd))
        return FALSE;
    try
    {
        auto wid = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
        s_appwindow = muw::AppWindow::GetFromWindowId(wid);
        if (!s_appwindow
                || !muw::AppWindowTitleBar::IsCustomizationSupported())
            return FALSE;

        auto tb = s_appwindow.TitleBar();
        tb.ExtendsContentIntoTitleBar(true);
        // Keep the caption buttons in the Standard (compact, top-aligned)
        // layout; the tab strip is made a bit taller than the buttons on the
        // C side (see *height below) so the tabs have room without the whole
        // title bar being as tall as "Tall".
        tb.PreferredHeightOption(muw::TitleBarHeightOption::Standard);
        s_titlebar_active = true;

        // Caption buttons + title bar background, following the system theme.
        winui_apply_colors(winui_system_is_dark());

        // Tab strip is 25% taller than the Standard caption height (~32 -> 40),
        // giving Notepad-like room without going to the full "Tall" height.
        *height = tb.Height() + tb.Height() / 4;
        *left = tb.LeftInset();
        *right = tb.RightInset();
        return TRUE;
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"Title bar", e.code(), e.message().c_str());
        s_titlebar_active = false;
        return FALSE;
    }
    catch (...)
    {
        s_titlebar_active = false;
        return FALSE;
    }
}

/*
 * Current title bar height and caption-button insets (device pixels).
 */
    void
WinUI_TitleBarGetMetrics(int *height, int *left, int *right)
{
    *height = 0;
    *left = 0;
    *right = 0;
    if (!s_appwindow || !s_titlebar_active)
        return;
    try
    {
        auto tb = s_appwindow.TitleBar();
        // Keep in sync with WinUI_TitleBarInit: tab strip is 25% taller than
        // the Standard caption height.
        *height = tb.Height() + tb.Height() / 4;
        *left = tb.LeftInset();
        *right = tb.RightInset();
    }
    catch (...)
    {
    }
}

/*
 * Declare the draggable regions of the title bar (client pixels).  Pass one
 * rectangle as {x, y, w, h} groups of 4 ints, "count" rectangles.
 */
    void
WinUI_TitleBarSetDrag(const int *rects, int count)
{
    if (!s_appwindow || !s_titlebar_active)
        return;
    try
    {
        std::vector<winrt::Windows::Graphics::RectInt32> v;
        for (int i = 0; i < count; ++i)
            v.push_back(winrt::Windows::Graphics::RectInt32{
                    rects[i * 4 + 0], rects[i * 4 + 1],
                    rects[i * 4 + 2], rects[i * 4 + 3]});
        s_appwindow.TitleBar().SetDragRectangles(
                winrt::array_view<winrt::Windows::Graphics::RectInt32 const>(
                    v.data(), v.data() + v.size()));
    }
    catch (...)
    {
    }
}

// ---------------------------------------------------------------------------
// TabView in the title bar (synced with Vim's tab pages).
// ---------------------------------------------------------------------------

/*
 * Push the draggable footer's bounds to the system as the title bar drag
 * rectangle (so the whole area after the tabs moves the window).
 */
    static void
winui_tabdrag_update(void)
{
    if (!s_tabdrag || !s_appwindow)
        return;
    try
    {
        if (!s_tabbar_root || s_tabbar_root.XamlRoot() == nullptr)
            return;
        double scale = s_tabbar_root.XamlRoot().RasterizationScale();
        auto t = s_tabdrag.TransformToVisual(s_tabbar_root);
        auto p = t.TransformPoint(wf::Point{0, 0});
        winrt::Windows::Graphics::RectInt32 r{
            s_tab_island_x + (int)(p.X * scale),
            s_tab_island_y + (int)(p.Y * scale),
            (int)(s_tabdrag.ActualWidth() * scale),
            (int)(s_tabdrag.ActualHeight() * scale)};
        if (r.Width > 0 && r.Height > 0)
            s_appwindow.TitleBar().SetDragRectangles(
                winrt::array_view<winrt::Windows::Graphics::RectInt32 const>(
                    &r, &r + 1));
    }
    catch (...)
    {
    }
}

/*
 * Walk the TabView's visual tree and clear the item container transitions of
 * the inner ListView(s), so adding/removing a tab is instant.  Without this a
 * freshly added tab plays an entrance animation and its label only appears
 * once the animation settles (visible as a delay when system animations are
 * enabled).
 */
    static void
winui_disable_tab_animations(mux::DependencyObject const &root)
{
    int n = mux::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < n; ++i)
    {
        auto child = mux::Media::VisualTreeHelper::GetChild(root, i);
        if (auto lv = child.try_as<muxc::ListView>())
            lv.ItemContainerTransitions(nullptr);
        winui_disable_tab_animations(child);
    }
}

/*
 * Build the per-tab context menu (New tab / Close tab / Close other tabs /
 * Close tabs to the right).  The clicked tab's index is resolved at click
 * time from its position in the TabView.
 */
    static muxc::MenuFlyout
winui_make_tab_flyout(muxc::TabViewItem const &item)
{
    muxc::MenuFlyout flyout;
    winrt::weak_ref<muxc::TabViewItem> wi = item;

    // "accel" is the equivalent Vim command, shown greyed on the right.
    auto add_item = [&](winrt::hstring const &text, const wchar_t *accel,
                        int action) -> muxc::MenuFlyoutItem
    {
        if (text.empty())
            return nullptr;
        muxc::MenuFlyoutItem mi;
        mi.Text(text);
        if (accel != nullptr && *accel != L'\0')
            mi.KeyboardAcceleratorTextOverride(accel);
        mi.Click([wi, action](wf::IInspectable const &,
                              mux::RoutedEventArgs const &)
        {
            try
            {
                if (action == 0)	// New tab: no index needed.
                {
                    gui_winui_tab_new();
                    return;
                }
                auto it = wi.get();
                uint32_t idx = 0;
                if (!it || !s_tabview
                        || !s_tabview.TabItems().IndexOf(it, idx))
                    return;
                int nr = (int)idx + 1;
                switch (action)
                {
                    case 1: gui_winui_tab_close(nr); break;
                    case 2: gui_winui_tab_close_others(nr); break;
                    case 3: gui_winui_tab_close_right(nr); break;
                    case 4: gui_winui_tab_copy_name(nr); break;
                    case 5: gui_winui_tab_move_left(nr); break;
                    case 6: gui_winui_tab_move_right(nr); break;
                }
            }
            catch (...)
            {
            }
        });
        flyout.Items().Append(mi);
        return mi;
    };

    add_item(s_tabmenu_new, L":tabnew", 0);
    muxc::MenuFlyoutItem moveL = add_item(s_tabmenu_moveleft, L":tabmove -1", 5);
    muxc::MenuFlyoutItem moveR = add_item(s_tabmenu_moveright, L":tabmove +1", 6);
    if (!s_tabmenu_new.empty())
        flyout.Items().Append(muxc::MenuFlyoutSeparator());
    add_item(s_tabmenu_close, L":tabclose", 1);
    muxc::MenuFlyoutItem others = add_item(s_tabmenu_others, L":tabonly", 2);
    muxc::MenuFlyoutItem right = add_item(s_tabmenu_right, L"", 3);
    if (!s_tabmenu_copy.empty())
        flyout.Items().Append(muxc::MenuFlyoutSeparator());
    muxc::MenuFlyoutItem copy = add_item(s_tabmenu_copy, L"", 4);

    // Enable/disable the context-sensitive items each time the menu opens:
    // move left/right need a neighbour on that side, "other tabs" needs >1 tab,
    // "to the right" needs a tab after this one, "copy file name" needs a file.
    flyout.Opening([wi, moveL, moveR, others, right, copy](
                        wf::IInspectable const &, wf::IInspectable const &)
    {
        try
        {
            auto it = wi.get();
            uint32_t idx = 0;
            if (!it || !s_tabview
                    || !s_tabview.TabItems().IndexOf(it, idx))
                return;
            uint32_t total = s_tabview.TabItems().Size();
            int nr = (int)idx + 1;
            if (moveL != nullptr)
                moveL.IsEnabled(idx > 0);
            if (moveR != nullptr)
                moveR.IsEnabled(idx + 1 < total);
            if (others != nullptr)
                others.IsEnabled(total > 1);
            if (right != nullptr)
                right.IsEnabled(idx + 1 < total);
            if (copy != nullptr)
                copy.IsEnabled(gui_winui_tab_has_file(nr) != 0);
        }
        catch (...)
        {
        }
    });
    return flyout;
}

/*
 * Store the (localized) labels used by the per-tab context menu.
 */
    void
WinUI_TabBarSetMenuLabels(const WCHAR *newtab, const WCHAR *close,
        const WCHAR *others, const WCHAR *right, const WCHAR *copyname,
        const WCHAR *moveleft, const WCHAR *moveright)
{
    if (newtab != NULL)
        s_tabmenu_new = newtab;
    if (close != NULL)
        s_tabmenu_close = close;
    if (others != NULL)
        s_tabmenu_others = others;
    if (right != NULL)
        s_tabmenu_right = right;
    if (copyname != NULL)
        s_tabmenu_copy = copyname;
    if (moveleft != NULL)
        s_tabmenu_moveleft = moveleft;
    if (moveright != NULL)
        s_tabmenu_moveright = moveright;
}

/*
 * Create the TabView island.  Returns TRUE on success.
 */
    int
WinUI_TabBarInit(HWND hwnd, const WCHAR *icon_path)
{
    if (s_tabview)
        return TRUE;
    if (!ensure_init(hwnd))
        return FALSE;
    try
    {
        s_tabbar_source = muxh::DesktopWindowXamlSource();
        s_tabbar_source.Initialize(
                            winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd));
        s_tabbar_root = muxc::Grid();

        s_tabview = muxc::TabView();
        s_tabview.TabWidthMode(muxc::TabViewWidthMode::Equal);
        s_tabview.CanReorderTabs(true);
        s_tabview.CanDragTabs(false);
        s_tabview.IsAddTabButtonVisible(true);
        // A TabView reserves an (empty, for us) content region below the tab
        // strip.  Align it to the bottom so the tabs sit flush against the
        // menu bar instead of leaving a grey band below them.
        s_tabview.VerticalAlignment(mux::VerticalAlignment::Bottom);

        // Kill the tab entrance/reposition animation so a new tab and its
        // label appear instantly (the inner ListView exists only once the
        // template is applied, hence doing this on Loaded).
        s_tabview.Loaded([](wf::IInspectable const &, mux::RoutedEventArgs const &)
        {
            try
            {
                winui_disable_tab_animations(s_tabview);
            }
            catch (...)
            {
            }
        });

        // A transparent region after the tabs that drags the window.  It
        // fills the space left of the caption buttons; its bounds are pushed
        // to the system as the drag rectangle whenever they change.
        s_tabdrag = muxc::Grid();
        s_tabdrag.Background(mux::Media::SolidColorBrush(
                            winrt::Windows::UI::Colors::Transparent()));
        s_tabdrag.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        s_tabdrag.VerticalAlignment(mux::VerticalAlignment::Stretch);
        s_tabdrag.MinWidth(1);
        s_tabview.TabStripFooter(s_tabdrag);
        s_tabdrag.SizeChanged([](wf::IInspectable const &,
                                 mux::SizeChangedEventArgs const &)
        {
            winui_tabdrag_update();
        });

        // Small app icon (joli.png) to the left of the tabs.
        if (icon_path != NULL && *icon_path != L'\0')
        {
            try
            {
                std::wstring u(icon_path);
                for (auto &c : u)
                    if (c == L'\\')
                        c = L'/';
                winrt::Windows::Foundation::Uri uri(
                                    winrt::hstring(L"file:///" + u));
                mux::Media::Imaging::BitmapImage bmp(uri);
                muxc::Image img;
                img.Width(18);
                img.Height(18);
                img.Source(bmp);

                // A transparent button so clicking the icon opens the
                // Windows system menu (Restore/Move/Size/...).
                muxc::Button iconbtn;
                iconbtn.Content(img);
                iconbtn.Background(mux::Media::SolidColorBrush(
                            winrt::Windows::UI::Colors::Transparent()));
                iconbtn.BorderThickness(mux::Thickness{0, 0, 0, 0});
                iconbtn.Padding(mux::Thickness{10, 0, 6, 0});
                iconbtn.VerticalAlignment(mux::VerticalAlignment::Stretch);
                iconbtn.Click([](wf::IInspectable const &,
                                 mux::RoutedEventArgs const &)
                {
                    gui_winui_show_sysmenu();
                });
                s_tabview.TabStripHeader(iconbtn);
            }
            catch (...)
            {
            }
        }

        s_tabview.AddTabButtonClick([](muxc::TabView const &,
                                       wf::IInspectable const &)
        {
            gui_winui_tab_new();
        });
        s_tabview.TabCloseRequested([](muxc::TabView const &sender,
                        muxc::TabViewTabCloseRequestedEventArgs const &args)
        {
            uint32_t idx = 0;
            if (sender.TabItems().IndexOf(args.Tab(), idx))
                gui_winui_tab_close((int)idx + 1);
        });
        s_tabview.SelectionChanged([](wf::IInspectable const &,
                        muxc::SelectionChangedEventArgs const &)
        {
            if (s_tab_updating)
                return;
            int sel = s_tabview.SelectedIndex();
            if (sel >= 0)
                gui_winui_tab_select(sel + 1);
        });
        // Drag-to-reorder: a move shows up as a Remove followed by an Insert;
        // act on the Insert (its Index is the tab's new position) and tell Vim
        // to move the (now selected) tab there.  Our own updates are guarded by
        // s_tab_updating.
        s_tabview.TabItemsChanged([](muxc::TabView const &,
                        wf::Collections::IVectorChangedEventArgs const &args)
        {
            if (s_tab_updating)
                return;
            if (args.CollectionChange()
                            == wf::Collections::CollectionChange::ItemInserted)
                gui_winui_tab_moved((int)args.Index());
        });

        s_tabbar_root.Children().Append(s_tabview);

        // Tab strip colours, following the system light/dark theme.
        winui_apply_colors(winui_system_is_dark());

        s_tabbar_source.Content(s_tabbar_root);
        s_tabbar_hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(
                                s_tabbar_source.SiteBridge().WindowId());
        s_tabbar_source.SiteBridge().Hide();
    }
    catch (winrt::hresult_error const &e)
    {
        set_error(L"TabView", e.code(), e.message().c_str());
        s_tabview = nullptr;
        return FALSE;
    }
    catch (...)
    {
        s_tabview = nullptr;
        return FALSE;
    }
    return TRUE;
}

/*
 * Rebuild the tabs from Vim: "labels" (count) and the 0-based "selected".
 */
    void
WinUI_TabBarSetItems(const WCHAR **labels, int count, int selected)
{
    if (!s_tabview)
        return;
    try
    {
        s_tab_updating = true;
        auto items = s_tabview.TabItems();

        // After a theme change, recreate the items so they pick up the new
        // brushes instead of keeping the ones resolved at creation time.
        if (s_tab_force_rebuild)
        {
            items.Clear();
            s_tab_force_rebuild = false;
        }

        // Update existing tabs in place, add/remove only what changed, so a
        // plain selection change does not rebuild (and flicker) the strip.
        for (int i = 0; i < count; ++i)
        {
            winrt::hstring h(labels[i]);

            if ((uint32_t)i < items.Size())
            {
                auto t = items.GetAt(i).as<muxc::TabViewItem>();
                auto cur = winrt::unbox_value_or<winrt::hstring>(
                                                    t.Header(), L"");
                if (cur != h)
                    t.Header(winrt::box_value(h));
            }
            else
            {
                muxc::TabViewItem t;
                t.Header(winrt::box_value(h));
                t.IsClosable(true);
                t.ContextFlyout(winui_make_tab_flyout(t));
                items.Append(t);
            }
        }
        while (items.Size() > (uint32_t)count)
            items.RemoveAtEnd();
        if (selected >= 0 && selected < count)
            s_tabview.SelectedIndex(selected);
        s_tab_updating = false;
    }
    catch (...)
    {
        s_tab_updating = false;
    }
}

/*
 * Select tab "idx" (0-based) without firing the Vim callback.
 */
    void
WinUI_TabBarSetSelected(int idx)
{
    if (!s_tabview)
        return;
    try
    {
        s_tab_updating = true;
        if (idx >= 0 && (uint32_t)idx < s_tabview.TabItems().Size())
            s_tabview.SelectedIndex(idx);
        s_tab_updating = false;
    }
    catch (...)
    {
        s_tab_updating = false;
    }
}

/*
 * Position the tab bar island (client pixels) and show/hide it.
 */
    void
WinUI_TabBarSetPos(int x, int y, int w, int h, int show, int right_inset)
{
    if (!s_tabbar_source)
        return;
    try
    {
        s_tab_island_x = x;
        s_tab_island_y = y;
        s_tabbar_source.SiteBridge().MoveAndResize(
                winrt::Windows::Graphics::RectInt32{x, y,
                                        w > 0 ? w : 1, h > 0 ? h : 1});
        // The island spans the full width so its dark background fills the
        // area under the caption buttons; inset the TabView on the right
        // (device px -> DIPs) so the tabs do not slide under the buttons.
        if (s_tabview)
        {
            double scale = 1.0;
            if (s_tabbar_root && s_tabbar_root.XamlRoot() != nullptr)
                scale = s_tabbar_root.XamlRoot().RasterizationScale();
            if (scale <= 0.0)
                scale = 1.0;
            double m = right_inset > 0 ? right_inset / scale : 0.0;
            s_tabview.Margin(mux::Thickness{0, 0, m, 0});
        }
        if (show)
            s_tabbar_source.SiteBridge().Show();
        else
            s_tabbar_source.SiteBridge().Hide();
        winui_tabdrag_update();
    }
    catch (...)
    {
    }
}
