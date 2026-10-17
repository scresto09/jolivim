/* vi:set ts=8 sts=4 sw=4 noet: */
/*
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 */

/*
 * gui_w32_winui.h: prototypes for gui_w32_winui.c, called from gui_w32.c.
 */
#ifndef GUI_W32_WINUI_H
#define GUI_W32_WINUI_H

#if defined(FEAT_WINUI)
int winui_show_popupmenu(vimmenu_T *menu, int x, int y);
int winui_make_items(vimmenu_T *menu, int mode, int all, WinUIMenuItem **itemsp);
void winui_free_items(WinUIMenuItem *items, int count);
int winui_tabline_popup(POINT pt);
int winui_dialog(char_u *title, char_u *message, char_u *buttons,
	int dfltbutton, char_u *textfield);
char_u *winui_browse(int saving, char_u *title, char_u *dflt, char_u *ext,
	char_u *initdir, char_u *filter, int *ok);
# ifdef MSWIN_FIND_REPLACE
int winui_show_findbar(char_u *arg, int replace);
void winui_findbar_reposition(void);
# endif
#endif

#endif // GUI_W32_WINUI_H
