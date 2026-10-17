/* vi:set ts=8 sts=4 sw=4 noet: */
/*
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * gui_w32_winui.c: WinUI integration glue for the Win32 GUI.
 *
 * The implementations of the WinUI-backed popup menu, tab popup, find bar,
 * file and modal dialogs live here; gui_w32.c calls them at points guarded by
 * "#ifdef FEAT_WINUI".  The C++/WinRT code itself is in gui_winui.cpp.
 */

#include "vim.h"

#if defined(FEAT_GUI_MSWIN) && defined(FEAT_WINUI)

# include <commctrl.h>
# include <windowsx.h>
# include <shobjidl.h>	// for IFileDialog (modern file dialogs)
# include "version.h"
# include "gui_winui.h"
# include "gui_w32_winui.h"

// Shared with gui_w32.c.
extern HWND s_hwnd;
extern HWND s_textArea;
extern HWND s_tabhwnd;
extern WCHAR *convert_filterW(char_u *s);

# if defined(FEAT_WINUI)
/*
 * Show the tabline popup menu with WinUI.  "pt" is the screen position.
 * Returns FALSE when WinUI can't be used (the Win32 menu is used then).
 */
    int
winui_tabline_popup(POINT pt)
{
    WinUIMenuItem   items[3];
    int		    n = 0;
    int		    id;
    int		    i;

    vim_memset(items, 0, sizeof(items));
    if (first_tabpage->tp_next != NULL)
    {
	items[n].label = (WCHAR *)enc_to_utf16((char_u *)_("Close tab"), NULL);
	items[n].id = TABLINE_MENU_CLOSE;
	items[n].enabled = TRUE;
	++n;
    }
    items[n].label = (WCHAR *)enc_to_utf16((char_u *)_("New tab"), NULL);
    items[n].id = TABLINE_MENU_NEW;
    items[n].enabled = TRUE;
    ++n;
    items[n].label = (WCHAR *)enc_to_utf16((char_u *)_("Open tab..."), NULL);
    items[n].id = TABLINE_MENU_OPEN;
    items[n].enabled = TRUE;
    ++n;

    id = WinUI_ShowPopupMenu(s_hwnd, items, n, (int)pt.x, (int)pt.y);

    for (i = 0; i < n; ++i)
	vim_free((void *)items[i].label);

    if (id < 0)
	return FALSE;	    // WinUI not available, use the Win32 menu

    if (id > 0)
    {
	TCHITTESTINFO	htinfo;
	int		idx;

	if (ScreenToClient(s_tabhwnd, &pt) != 0)
	{
	    htinfo.pt.x = pt.x;
	    htinfo.pt.y = pt.y;
	    idx = TabCtrl_HitTest(s_tabhwnd, &htinfo);
	    if (idx == -1)
		idx = 0;
	    else
		idx += 1;
	    send_tabline_menu_event(idx, id);
	}
    }
    return TRUE;
}
# endif

# if defined(FEAT_WINUI) && defined(MSWIN_FIND_REPLACE)
/*
 * Called by the WinUI find bar to run a find or replace, see WINUI_FIND_*.
 */
    void
gui_winui_do_findrepl(
	int		action,
	const WCHAR	*find,
	const WCHAR	*repl,
	int		match_case,
	int		whole_word)
{
    char_u	*find_text;
    char_u	*repl_text = NULL;
    int		flags = 0;
    int		down = TRUE;

    find_text = utf16_to_enc((short_u *)find, NULL);
    if (find_text == NULL || *find_text == NUL)
    {
	vim_free(find_text);
	return;
    }
    if (repl != NULL)
	repl_text = utf16_to_enc((short_u *)repl, NULL);

    switch (action)
    {
	case WINUI_FIND_PREV:	   flags = FRD_FINDNEXT; down = FALSE; break;
	case WINUI_FIND_REPLACE:   flags = FRD_REPLACE; break;
	case WINUI_FIND_REPLACEALL: flags = FRD_REPLACEALL; break;
	default:		   flags = FRD_FINDNEXT; break;
    }
    if (match_case)
	flags |= FRD_MATCH_CASE;
    if (whole_word)
	flags |= FRD_WHOLE_WORD;

    gui_do_findrepl(flags, find_text, repl_text, down);

    vim_free(find_text);
    vim_free(repl_text);
}

// 'hlsearch' value saved while the find bar highlights matches, -1 = inactive.
static int s_findbar_saved_hls = -1;

/*
 * Turn on search highlighting so the matches show up (in yellow), like
 * Notepad.  The previous 'hlsearch' value is restored when the bar closes.
 */
    static void
winui_findbar_highlight_on(void)
{
    if (s_findbar_saved_hls == -1)
	s_findbar_saved_hls = p_hls;
    p_hls = TRUE;
    set_no_hlsearch(FALSE);
    redraw_all_later(UPD_NOT_VALID);
}

/*
 * Called by the WinUI find bar when it is closed: stop highlighting, restore
 * 'hlsearch' and give the focus back to Vim.
 */
    void
gui_winui_findbar_closed(void)
{
    if (s_findbar_saved_hls != -1)
    {
	p_hls = s_findbar_saved_hls;
	s_findbar_saved_hls = -1;
	set_no_hlsearch(TRUE);
	redraw_all_later(UPD_NOT_VALID);
    }
    (void)SetFocus(s_hwnd);
}

/*
 * Geometry of the text area in client pixels, for placing the find bar.
 */
    static void
winui_textarea_rect(int *left, int *top, int *width)
{
    RECT    rc;
    POINT   tl;

    GetWindowRect(s_textArea, &rc);
    tl.x = rc.left;
    tl.y = rc.top;
    ScreenToClient(s_hwnd, &tl);
    *left = tl.x;
    *top = tl.y;
    *width = rc.right - rc.left;
}

/*
 * Reposition the find bar when the text area moved or was resized.
 */
    void
winui_findbar_reposition(void)
{
    int	    left, top, width;

    if (!WinUI_FindBarVisible())
	return;
    winui_textarea_rect(&left, &top, &width);
    WinUI_FindBarSetPos(left, top, width);
}

/*
 * Pass Vim's search history (newest first) to the find bar for suggestions.
 */
    static void
winui_set_find_history(void)
{
    histentry_T	*he = get_histentry(HIST_SEARCH);
    int		len = get_hislen();
    WCHAR	**items;
    int		*idx;
    int		i, j, cnt = 0, n = 0;

    if (he == NULL || len <= 0)
    {
	WinUI_FindBarSetHistory(NULL, 0);
	return;
    }
    items = ALLOC_MULT(WCHAR *, len);
    idx = ALLOC_MULT(int, len);
    if (items == NULL || idx == NULL)
    {
	vim_free(items);
	vim_free(idx);
	return;
    }
    // Collect non-empty entries.
    for (i = 0; i < len; ++i)
	if (he[i].hisstr != NULL && *he[i].hisstr != NUL)
	    idx[cnt++] = i;
    // Sort by hisnum descending (newest first); cnt is small.
    for (i = 1; i < cnt; ++i)
    {
	int k = idx[i];

	for (j = i - 1; j >= 0 && he[idx[j]].hisnum < he[k].hisnum; --j)
	    idx[j + 1] = idx[j];
	idx[j + 1] = k;
    }
    for (i = 0; i < cnt; ++i)
    {
	items[n] = (WCHAR *)enc_to_utf16(he[idx[i]].hisstr, NULL);
	if (items[n] != NULL)
	    ++n;
    }
    WinUI_FindBarSetHistory((const WCHAR **)items, n);
    for (i = 0; i < n; ++i)
	vim_free(items[i]);
    vim_free(items);
    vim_free(idx);
}

/*
 * Show the WinUI find (replace != 0: find/replace) bar.  Returns FALSE when
 * WinUI can't be used, the Win32 dialog is used then.
 */
    int
winui_show_findbar(char_u *arg, int replace)
{
    int		wword = FALSE;
    int		mcase = !p_ic;
    char_u	*text;
    WCHAR	*wtext;
    int		left, top, width;
    int		ret;

    WinUIFindLabels labels;
    WCHAR	*lbuf[11];
    int		i;

    text = get_find_dialog_text(arg, &wword, &mcase);
    wtext = (WCHAR *)enc_to_utf16(text != NULL ? text : (char_u *)"", NULL);
    winui_textarea_rect(&left, &top, &width);

    // Translated labels, using Vim's own gettext strings.
    labels.find	       = lbuf[0]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Find what:"), NULL);
    labels.replace     = lbuf[1]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Replace with:"), NULL);
    labels.match_case  = lbuf[2]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Match case"), NULL);
    labels.whole_word  = lbuf[3]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Match whole word only"), NULL);
    labels.find_next   = lbuf[4]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Find Next"), NULL);
    labels.find_prev   = lbuf[5]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Up"), NULL);
    labels.replace_one = lbuf[6]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Replace"), NULL);
    labels.replace_all = lbuf[7]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Replace All"), NULL);
    labels.close       = lbuf[8]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Close"), NULL);
    labels.expand      = lbuf[9]  = (WCHAR *)enc_to_utf16(
					(char_u *)_("Replace"), NULL);
    labels.options     = lbuf[10] = (WCHAR *)enc_to_utf16(
					(char_u *)_("Options"), NULL);

    ret = WinUI_ShowFindBar(s_hwnd, wtext, replace, mcase, wword,
						left, top, width, &labels);
    if (ret)
    {
	winui_set_find_history();
	winui_findbar_highlight_on();
    }

    for (i = 0; i < (int)ARRAY_LENGTH(lbuf); ++i)
	vim_free(lbuf[i]);
    vim_free(wtext);
    vim_free(text);
    return ret;
}
# endif

# ifdef FEAT_MENU
/*
 * Return the icon to use for "menu" in the WinUI popup menu.  The
 * untranslated name is used, so that it works for all languages.
 */
    static WinUIIcon
winui_menu_icon(vimmenu_T *menu)
{
    static const struct
    {
	char	    *name;
	WinUIIcon   icon;
    } icons[] = {
	{"Undo", WINUI_ICON_UNDO},
	{"Redo", WINUI_ICON_REDO},
	{"Cut", WINUI_ICON_CUT},
	{"Copy", WINUI_ICON_COPY},
	{"Paste", WINUI_ICON_PASTE},
	{"Delete", WINUI_ICON_DELETE},
	{"Select All", WINUI_ICON_SELECTALL},
    };
    char_u  *name = menu->dname;
    int	    i;

#  ifdef FEAT_MULTI_LANG
    if (menu->en_dname != NULL)
	name = menu->en_dname;
#  endif
    for (i = 0; i < (int)ARRAY_LENGTH(icons); ++i)
	if (STRCMP(name, icons[i].name) == 0)
	    return icons[i].icon;
    return WINUI_ICON_NONE;
}

    void
winui_free_items(WinUIMenuItem *items, int count)
{
    int	i;

    for (i = 0; i < count; ++i)
    {
	vim_free((void *)items[i].label);
	vim_free((void *)items[i].accel);
	winui_free_items(items[i].children, items[i].nchildren);
    }
    vim_free(items);
}

/*
 * Create the WinUI items for the menu entries "menu" and the ones following
 * it, for menu mode "mode".  Hidden entries are skipped, unless "all" is
 * TRUE, then they are disabled and the top level only has the menus of the
 * menu bar (for the menu bar).
 * Returns the number of items, -1 when out of memory.
 */
    int
winui_make_items(vimmenu_T *menu, int mode, int all, WinUIMenuItem **itemsp)
{
    WinUIMenuItem   *items;
    WinUIMenuItem   *item;
    vimmenu_T	    *m;
    int		    len = 0;
    int		    count = 0;

    for (m = menu; m != NULL; m = m->next)
	++len;
    *itemsp = items = ALLOC_CLEAR_MULT(WinUIMenuItem, len + 1);
    if (items == NULL)
	return -1;

    for (m = menu; m != NULL; m = m->next)
    {
	WinUIIcon   icon = winui_menu_icon(m);

	if (m->dname[0] == ']'
#  ifdef FEAT_TEAROFF
		|| STRCMP(m->dname, TEAR_STRING) == 0
#  endif
		|| (m->modes & MENU_ALL_MODES) == 0
		|| (all && m->parent == NULL && !menu_is_menubar(m->name)))
	    continue;
	// Items in the bar are greyed instead of hidden, so that the bar
	// doesn't change between modes.
	if (!all && (m->modes & mode) == 0 && icon == WINUI_ICON_NONE
					&& vim_strchr(p_go, GO_GREY) == NULL)
	    continue;
	if (all)
	    icon = WINUI_ICON_NONE;

	item = &items[count++];
	item->id = m->id;
	item->enabled = (m->modes & m->enabled & mode) != 0;
	if (menu_is_separator(m->dname))
	    continue;

	item->icon = icon;
	item->label = (WCHAR *)enc_to_utf16(m->dname, NULL);
	if (item->label == NULL)
	    goto fail;
	if (m->actext != NULL && *m->actext != NUL)
	{
	    item->accel = (WCHAR *)enc_to_utf16(m->actext, NULL);
	    if (item->accel == NULL)
		goto fail;
	}
	if (m->mnemonic > 0 && m->mnemonic < 0x10000)
	    item->mnemonic = (WCHAR)m->mnemonic;
	if (m->children != NULL)
	{
	    item->nchildren = winui_make_items(m->children, mode, all,
							    &item->children);
	    if (item->nchildren < 0)
	    {
		item->nchildren = 0;
		goto fail;
	    }
	}
    }
    return count;

fail:
    winui_free_items(items, count);
    *itemsp = NULL;
    return -1;
}

/*
 * Show "menu" as a WinUI 3 popup menu at screen position "x", "y".
 * Returns FALSE when WinUI can't be used.
 */
    int
winui_show_popupmenu(vimmenu_T *menu, int x, int y)
{
    WinUIMenuItem   *items;
    int		    count;
    int		    id;

    count = winui_make_items(menu->children, get_menu_mode_flag(), FALSE,
									&items);
    if (count < 0)
	return FALSE;
    id = WinUI_ShowPopupMenu(s_hwnd, items, count, x, y);
    winui_free_items(items, count);
    if (id < 0)
    {
	const WCHAR *err = WinUI_GetError();

	if (p_verbose > 0 && *err != L'\0')
	{
	    char_u *msg = utf16_to_enc((short_u *)err, NULL);

	    if (msg != NULL)
	    {
		verbose_enter();
		smsg("WinUI popup menu: %s", msg);
		verbose_leave();
		vim_free(msg);
	    }
	}
	return FALSE;
    }

    // Like TrackPopupMenu() does.
    if (id > 0)
	PostMessage(s_hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    return TRUE;
}

# endif // FEAT_MENU

# if defined(FEAT_WINUI)
/*
 * Modern Open/Save file dialog using IFileDialog (the Windows Explorer-style
 * dialog).  Sets "*ok" to FALSE when the dialog can't be created (the caller
 * then uses the classic common dialog); TRUE when it ran (returns the chosen
 * file, or NULL when cancelled).
 */
    char_u *
winui_browse(
	int saving, char_u *title, char_u *dflt, char_u *ext,
	char_u *initdir, char_u *filter, int *ok)
{
    IFileDialog		*pfd = NULL;
    WCHAR		*wtitle = NULL, *wext = NULL, *wdflt = NULL;
    WCHAR		*winitdir = NULL, *wfilter = NULL;
    COMDLG_FILTERSPEC	*specs = NULL;
    char_u		*result = NULL;
    HRESULT		hr;
    int			did_coinit = FALSE;
    DWORD		opts = 0;

    *ok = FALSE;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED
						| COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr))
	did_coinit = TRUE;
    else if (hr != RPC_E_CHANGED_MODE)
	return NULL;

    hr = CoCreateInstance(
	    saving ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog,
	    NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void **)&pfd);
    if (FAILED(hr) || pfd == NULL)
	goto done;

    if (title != NULL && (wtitle = enc_to_utf16(title, NULL)) != NULL)
	pfd->lpVtbl->SetTitle(pfd, wtitle);
    if (ext != NULL && (wext = enc_to_utf16(ext, NULL)) != NULL)
	pfd->lpVtbl->SetDefaultExtension(pfd, wext);
    if (dflt != NULL && (wdflt = enc_to_utf16(dflt, NULL)) != NULL)
	pfd->lpVtbl->SetFileName(pfd, wdflt);

    // Convert the filter to an array of name/spec pairs.
    wfilter = convert_filterW(filter);
    if (wfilter != NULL)
    {
	WCHAR	*w = wfilter;
	int	cnt = 0;
	int	i;

	while (*w != L'\0')
	{
	    while (*w != L'\0') ++w;	// skip name
	    ++w;
	    while (*w != L'\0') ++w;	// skip spec
	    ++w;
	    ++cnt;
	}
	if (cnt > 0)
	    specs = ALLOC_MULT(COMDLG_FILTERSPEC, cnt);
	if (specs != NULL)
	{
	    w = wfilter;
	    for (i = 0; i < cnt; ++i)
	    {
		specs[i].pszName = w;
		while (*w != L'\0') ++w;
		++w;
		specs[i].pszSpec = w;
		while (*w != L'\0') ++w;
		++w;
	    }
	    pfd->lpVtbl->SetFileTypes(pfd, (UINT)cnt, specs);
	}
    }

    if (initdir != NULL && *initdir != NUL
	    && (winitdir = enc_to_utf16(initdir, NULL)) != NULL)
    {
	IShellItem	*psi = NULL;
	WCHAR		*w;

	for (w = winitdir; *w != L'\0'; ++w)
	    if (*w == L'/')
		*w = L'\\';
	if (SUCCEEDED(SHCreateItemFromParsingName(winitdir, NULL,
					&IID_IShellItem, (void **)&psi))
		&& psi != NULL)
	{
	    pfd->lpVtbl->SetFolder(pfd, psi);
	    psi->lpVtbl->Release(psi);
	}
    }

    if (SUCCEEDED(pfd->lpVtbl->GetOptions(pfd, &opts)))
	pfd->lpVtbl->SetOptions(pfd, opts | FOS_NOCHANGEDIR
		| FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM
		| FOS_NOREADONLYRETURN);

    // From here the dialog is shown: no fall back to the classic dialog.
    *ok = TRUE;
    if (SUCCEEDED(pfd->lpVtbl->Show(pfd, s_hwnd)))
    {
	IShellItem	*psiResult = NULL;

	if (SUCCEEDED(pfd->lpVtbl->GetResult(pfd, &psiResult))
		&& psiResult != NULL)
	{
	    PWSTR	path = NULL;

	    if (SUCCEEDED(psiResult->lpVtbl->GetDisplayName(psiResult,
					    SIGDN_FILESYSPATH, &path))
		    && path != NULL)
	    {
		char_u	*p8 = utf16_to_enc((short_u *)path, NULL);

		if (p8 != NULL)
		{
		    result = vim_strsave(shorten_fname1(p8));
		    vim_free(p8);
		}
		CoTaskMemFree(path);
	    }
	    psiResult->lpVtbl->Release(psiResult);
	}
    }

done:
    if (pfd != NULL)
	pfd->lpVtbl->Release(pfd);
    vim_free(wtitle);
    vim_free(wext);
    vim_free(wdflt);
    vim_free(winitdir);
    vim_free(wfilter);
    vim_free(specs);
    if (did_coinit)
	CoUninitialize();
    (void)SetFocus(s_hwnd);
    return result;
}
# endif

# if defined(FEAT_WINUI)
/*
 * Show a modal dialog with a WinUI ContentDialog.  Handles any number of
 * buttons and an optional input field ("textfield").  Returns the 1-based
 * button index, or -1 when WinUI itself is unavailable.
 */
    int
winui_dialog(
	char_u *title, char_u *message, char_u *buttons, int dfltbutton,
	char_u *textfield)
{
    int		n = 1;
    int		i = 0;
    char_u	*p;
    char_u	*start;
    WCHAR	**wb;
    WCHAR	*wtitle;
    WCHAR	*wmsg;
    WCHAR	*wfield = NULL;
    int		ret;

    for (p = buttons; *p != NUL; ++p)
	if (*p == DLG_BUTTON_SEP)
	    ++n;

    wb = ALLOC_CLEAR_MULT(WCHAR *, n);
    if (wb == NULL)
	return -1;
    start = buttons;
    for (;;)
    {
	char_u	*end = vim_strchr(start, DLG_BUTTON_SEP);
	char_u	*seg = end == NULL ? vim_strsave(start)
				   : vim_strnsave(start, end - start);

	if (seg != NULL)
	{
	    wb[i] = (WCHAR *)enc_to_utf16(seg, NULL);
	    vim_free(seg);
	}
	++i;
	if (end == NULL)
	    break;
	start = end + 1;
    }

    wtitle = title != NULL ? (WCHAR *)enc_to_utf16(title, NULL) : NULL;
    wmsg = (WCHAR *)enc_to_utf16(message != NULL ? message : (char_u *)"",
									NULL);
    if (textfield != NULL)
    {
	WCHAR	*tmp;

	wfield = ALLOC_CLEAR_MULT(WCHAR, IOSIZE);
	if (wfield != NULL)
	{
	    tmp = (WCHAR *)enc_to_utf16(textfield, NULL);
	    if (tmp != NULL)
	    {
		wcsncpy(wfield, tmp, IOSIZE - 1);
		wfield[IOSIZE - 1] = L'\0';
		vim_free(tmp);
	    }
	}
    }

    ret = WinUI_ShowDialog(s_hwnd, wtitle, wmsg, (const WCHAR **)wb, n,
						dfltbutton, wfield, IOSIZE);

    // Copy the edited field text back to Vim.
    if (ret >= 1 && textfield != NULL && wfield != NULL)
    {
	char_u	*back = utf16_to_enc((short_u *)wfield, NULL);

	if (back != NULL)
	{
	    vim_strncpy(textfield, back, IOSIZE - 1);
	    vim_free(back);
	}
    }

    for (i = 0; i < n; ++i)
	vim_free(wb[i]);
    vim_free(wb);
    vim_free(wtitle);
    vim_free(wmsg);
    vim_free(wfield);
    return ret;
}
# endif

// Callback for do_in_runtimepath(): keep the first match in the buffer.
    static void
winui_find_cb(char_u *fname, void *ck)
{
    char_u *buf = (char_u *)ck;

    if (buf[0] == NUL && fname != NULL && *fname != NUL)
	vim_strncpy(buf, fname, MAXPATHL - 1);
}

/*
 * Show the modern "About" dialog: the joli.png icon and the intro text.
 * Returns TRUE when shown.
 */
    int
gui_mch_show_about(void)
{
    char_u	imgbuf[MAXPATHL];
    char_u	verpart[64];
    char_u	info[512];
    char_u	hints[512];
    WCHAR	*wimg = NULL;
    WCHAR	*wtitle;
    WCHAR	*winfo;
    WCHAR	*whints;
    WCHAR	*wclose;
    int		ret;

    // Look for joli.png in 'runtimepath' (e.g. in $VIMRUNTIME).
    imgbuf[0] = NUL;
    (void)do_in_runtimepath((char_u *)"joli.png", 0, winui_find_cb, imgbuf);

    if (highest_patch())
	vim_snprintf((char *)verpart, sizeof(verpart), "%s.%d",
					VIM_VERSION_MEDIUM, highest_patch());
    else
	vim_snprintf((char *)verpart, sizeof(verpart), "%s",
					VIM_VERSION_MEDIUM);

    vim_snprintf((char *)info, sizeof(info), "%s%s%s\n%s\n%s\n%s",
	    _("version "), verpart,_(", a playful fork of Vim by Sylvain Cresto"),
	    _("It's open source and freely distributable, making Vim look as"),
	    _("good as it edits. But no offense, it's just for fun ;-)"),
	    _("It's an independent project not affiliated with the Vim project"));

    vim_snprintf((char *)hints, sizeof(hints), "%s\n%s\n%s\n\n%s",
	    _("type  :help<Enter>  or  <F1>  for on-line help"),
	    _("type  :q<Enter>               to exit         "),
	    _("type  :help version9<Enter>   for version info"),
	    _("Help poor children in Uganda!"));

    if (imgbuf[0] != NUL)
	wimg = (WCHAR *)enc_to_utf16(imgbuf, NULL);
    wtitle = (WCHAR *)enc_to_utf16((char_u *)_("JoliVIM"), NULL);
    winfo = (WCHAR *)enc_to_utf16(info, NULL);
    whints = (WCHAR *)enc_to_utf16(hints, NULL);
    wclose = (WCHAR *)enc_to_utf16((char_u *)_("Close"), NULL);

    // Dedicated link to the JoliVIM repository under the description.
    ret = WinUI_ShowAbout(s_hwnd, wimg, wtitle, winfo, whints, wclose,
			    L"https://github.com/scresto09/jolivim",
			    L"https://github.com/scresto09/jolivim");

    vim_free(wimg);
    vim_free(wtitle);
    vim_free(winfo);
    vim_free(whints);
    vim_free(wclose);
    return ret;
}

/*
 * EnumFontFamiliesExW callback: collect unique fixed-pitch family names into
 * the growarray in "lparam".
 */
    static int CALLBACK
winui_font_enum_cb(
    const LOGFONTW	*lf,
    const TEXTMETRICW	*tm,
    DWORD		fonttype UNUSED,
    LPARAM		lparam)
{
    garray_T	*ga = (garray_T *)lparam;
    const WCHAR	*name = lf->lfFaceName;
    WCHAR	**arr;
    int		i;
    WCHAR	*copy;

    // Only monospace fonts; skip the "@" vertical variants and empty names.
    if (name[0] == L'@' || name[0] == L'\0')
	return 1;
    if (tm->tmPitchAndFamily & TMPF_FIXED_PITCH)
	return 1;		// a variable-pitch font

    arr = (WCHAR **)ga->ga_data;
    for (i = 0; i < ga->ga_len; ++i)
	if (wcscmp(arr[i], name) == 0)
	    return 1;		// already collected

    if (ga_grow(ga, 1) == FAIL)
	return 1;
    copy = (WCHAR *)alloc((wcslen(name) + 1) * sizeof(WCHAR));
    if (copy == NULL)
	return 1;
    wcscpy(copy, name);
    ((WCHAR **)ga->ga_data)[ga->ga_len++] = copy;
    return 1;
}

    static int
winui_font_cmp(const void *a, const void *b)
{
    return wcscmp(*(WCHAR * const *)a, *(WCHAR * const *)b);
}

/*
 * Modern WinUI font picker, replacing ChooseFontW.  Fills "lf" and returns 1 on
 * accept, 0 on cancel, -1 when WinUI is unavailable (fall back to ChooseFontW).
 */
    int
gui_mch_winui_choose_font(LOGFONTW *lf)
{
    garray_T		ga;
    WinUIFontChoice	fc;
    HDC			hdc;
    LOGFONTW		qlf;
    int			i;
    int			dpi = 96;
    int			px;
    int			r;
    WCHAR		*wtitle = NULL, *wsize = NULL, *wbold = NULL;
    WCHAR		*witalic = NULL, *wprev = NULL, *wok = NULL;
    WCHAR		*wcancel = NULL;

    ga_init2(&ga, sizeof(WCHAR *), 40);

    hdc = GetDC(s_hwnd);
    if (hdc != NULL)
    {
	CLEAR_FIELD(qlf);
	qlf.lfCharSet = DEFAULT_CHARSET;
	EnumFontFamiliesExW(hdc, &qlf,
			(FONTENUMPROCW)winui_font_enum_cb, (LPARAM)&ga, 0);
	dpi = GetDeviceCaps(hdc, LOGPIXELSY);
	ReleaseDC(s_hwnd, hdc);
    }
    if (dpi <= 0)
	dpi = 96;
    if (ga.ga_len > 1)
	qsort(ga.ga_data, (size_t)ga.ga_len, sizeof(WCHAR *), winui_font_cmp);

    CLEAR_FIELD(fc);
    fc.families = (const WCHAR **)ga.ga_data;
    fc.family_count = ga.ga_len;
    wcsncpy(fc.family, lf->lfFaceName, LF_FACESIZE - 1);
    fc.family[LF_FACESIZE - 1] = L'\0';
    px = lf->lfHeight < 0 ? -lf->lfHeight : lf->lfHeight;
    fc.size = px > 0 ? (int)((double)px * 72.0 / dpi + 0.5) : 11;
    if (fc.size < 6)
	fc.size = 6;
    if (fc.size > 72)
	fc.size = 72;
    fc.bold = lf->lfWeight >= FW_BOLD;
    fc.italic = lf->lfItalic ? 1 : 0;

    wtitle  = (WCHAR *)enc_to_utf16((char_u *)_("Select Font"), NULL);
    wsize   = (WCHAR *)enc_to_utf16((char_u *)_("Size"), NULL);
    wbold   = (WCHAR *)enc_to_utf16((char_u *)_("Bold"), NULL);
    witalic = (WCHAR *)enc_to_utf16((char_u *)_("Italic"), NULL);
    wprev   = (WCHAR *)enc_to_utf16(
			(char_u *)_("The quick brown fox jumps over the lazy dog"), NULL);
    wok     = (WCHAR *)enc_to_utf16((char_u *)_("OK"), NULL);
    wcancel = (WCHAR *)enc_to_utf16((char_u *)_("Cancel"), NULL);
    fc.title = wtitle;
    fc.size_label = wsize;
    fc.style_bold = wbold;
    fc.style_italic = witalic;
    fc.preview_text = wprev;
    fc.ok_label = wok;
    fc.cancel_label = wcancel;

    r = WinUI_ShowFontDialog(s_hwnd, &fc);

    if (r == 1)
    {
	wcsncpy(lf->lfFaceName, fc.family, LF_FACESIZE - 1);
	lf->lfFaceName[LF_FACESIZE - 1] = L'\0';
	lf->lfHeight = -MulDiv(fc.size, dpi, 72);
	lf->lfWidth = 0;
	lf->lfWeight = fc.bold ? FW_BOLD : FW_NORMAL;
	lf->lfItalic = (BYTE)(fc.italic ? TRUE : FALSE);
    }

    for (i = 0; i < ga.ga_len; ++i)
	vim_free(((WCHAR **)ga.ga_data)[i]);
    ga_clear(&ga);
    vim_free(wtitle);
    vim_free(wsize);
    vim_free(wbold);
    vim_free(witalic);
    vim_free(wprev);
    vim_free(wok);
    vim_free(wcancel);
    return r;
}

#endif // FEAT_GUI_MSWIN && FEAT_WINUI
