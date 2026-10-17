/* vi:set ts=8 sts=4 sw=4 et: */
/*
 * VIM - Vi IMproved    by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * gui_winui.h: Interface to the WinUI 3 popup menu (gui_winui.cpp).
 */

#ifndef GUI_WINUI_H
#define GUI_WINUI_H

#ifdef __cplusplus
extern "C" {
#endif

// Icon of an item; items with an icon are shown in the bar at the top.
typedef enum
{
    WINUI_ICON_NONE = 0,
    WINUI_ICON_UNDO,
    WINUI_ICON_REDO,
    WINUI_ICON_CUT,
    WINUI_ICON_COPY,
    WINUI_ICON_PASTE,
    WINUI_ICON_DELETE,
    WINUI_ICON_SELECTALL,
} WinUIIcon;

typedef struct WinUIMenuItem WinUIMenuItem;

struct WinUIMenuItem
{
    const WCHAR     *label;     // displayed name, NULL for a separator
    const WCHAR     *accel;     // accelerator text or NULL
    WCHAR           mnemonic;   // access key or 0
    WinUIIcon       icon;
    UINT            id;         // command ID sent with WM_COMMAND
    int             enabled;
    WinUIMenuItem   *children;  // items of a sub-menu or NULL
    int             nchildren;
};

// Posted to the Vim window when the menu bar needs to be positioned again.
#define WM_WINUI_LAYOUT (WM_APP + 0x101)

void WinUI_Final(void);
const WCHAR *WinUI_GetError(void);
int WinUI_ShowPopupMenu(HWND hwnd, const WinUIMenuItem *items, int count,
        int x, int y);

int WinUI_MenuBarInit(HWND hwnd);
void WinUI_MenuBarSetItems(const WinUIMenuItem *items, int count);
void WinUI_MenuBarSetEnabled(UINT id, int enabled);
void WinUI_MenuBarGetSize(int *width, int *height);
void WinUI_MenuBarSetPos(int x, int y, int w, int h);
void WinUI_MenuBarShow(int show);
int WinUI_MenuBarOpen(int ch);
int WinUI_PreTranslateMessage(MSG *msg);

int WinUI_TitleBarInit(HWND hwnd, int *height, int *left, int *right);
void WinUI_TitleBarGetMetrics(int *height, int *left, int *right);
void WinUI_TitleBarSetDrag(const int *rects, int count);
void WinUI_SetForceDark(int force);
void WinUI_UpdateTheme(void);

int WinUI_TabBarInit(HWND hwnd, const WCHAR *icon_path);
void WinUI_TabBarSetMenuLabels(const WCHAR *newtab, const WCHAR *close,
        const WCHAR *others, const WCHAR *right, const WCHAR *copyname,
        const WCHAR *moveleft, const WCHAR *moveright);
void WinUI_TabBarSetItems(const WCHAR **labels, int count, int selected);
void WinUI_TabBarSetSelected(int idx);
void WinUI_TabBarSetPos(int x, int y, int w, int h, int show, int right_inset);

// Implemented in gui_w32.c, called by the TabView.
void gui_winui_tab_select(int nr);
void gui_winui_tab_new(void);
void gui_winui_tab_close(int nr);
void gui_winui_tab_close_others(int nr);
void gui_winui_tab_close_right(int nr);
void gui_winui_tab_copy_name(int nr);
int gui_winui_tab_has_file(int nr);
void gui_winui_tab_move_left(int nr);
void gui_winui_tab_move_right(int nr);
void gui_winui_tab_moved(int idx);
void gui_winui_show_sysmenu(void);

int WinUI_ShowDialog(HWND hwnd, const WCHAR *title, const WCHAR *message,
        const WCHAR **buttons, int nbuttons, int dfltbutton,
        WCHAR *inout_text, int text_len);
int WinUI_ShowAbout(HWND hwnd, const WCHAR *image_path, const WCHAR *title,
        const WCHAR *info, const WCHAR *hints, const WCHAR *close_label,
        const WCHAR *link_text, const WCHAR *link_url);

// Modern font picker.  In/out fields carry the current and the chosen font;
// the "families" list and the labels are inputs filled on the C side.
typedef struct
{
    WCHAR        family[LF_FACESIZE];	// in/out: selected family name
    int          size;			// in/out: point size
    int          bold;			// in/out
    int          italic;		// in/out
    const WCHAR **families;		// in: sorted list of family names
    int          family_count;		// in
    const WCHAR *title;			// in: localized labels
    const WCHAR *size_label;
    const WCHAR *style_bold;
    const WCHAR *style_italic;
    const WCHAR *preview_text;
    const WCHAR *ok_label;
    const WCHAR *cancel_label;
} WinUIFontChoice;

// Returns 1 when the user accepted (io updated), 0 when cancelled, -1 when the
// WinUI runtime is not available (caller should fall back to ChooseFontW).
int WinUI_ShowFontDialog(HWND hwnd, WinUIFontChoice *io);

// TRUE while one of the dialogs above is shown.
int WinUI_DialogActive(void);

// Find/replace bar actions.
#define WINUI_FIND_NEXT		1
#define WINUI_FIND_PREV		2
#define WINUI_FIND_REPLACE	3
#define WINUI_FIND_REPLACEALL	4

// Translated labels for the find bar (filled on the C side with gettext).
typedef struct
{
    const WCHAR *find;		// search box placeholder
    const WCHAR *replace;	// replace box placeholder
    const WCHAR *match_case;
    const WCHAR *whole_word;
    const WCHAR *find_next;
    const WCHAR *find_prev;
    const WCHAR *replace_one;
    const WCHAR *replace_all;
    const WCHAR *close;
    const WCHAR *expand;
    const WCHAR *options;
} WinUIFindLabels;

int WinUI_ShowFindBar(HWND hwnd, const WCHAR *text, int replace,
        int match_case, int whole_word, int left, int top, int width,
        const WinUIFindLabels *labels);
void WinUI_FindBarSetPos(int left, int top, int width);
void WinUI_FindBarSetHistory(const WCHAR **items, int count);
void WinUI_HideFindBar(void);
int WinUI_FindBarVisible(void);

// Implemented in gui_w32.c, called by the find bar.
void gui_winui_do_findrepl(int action, const WCHAR *find, const WCHAR *repl,
        int match_case, int whole_word);
void gui_winui_findbar_closed(void);

#ifdef __cplusplus
}
#endif

#endif // GUI_WINUI_H
