/* vi:set ts=8 sts=4 sw=4 et:
 *
 * VIM - Vi IMproved    jvimext11 by Sylvain Cresto
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

/*
 * Settings shared by jvimext11.dll (the context menu entry) and
 * jvimext11_cfg.exe (the settings window), the lookup of jvim.exe and the
 * loading of the translated texts.
 *
 * The settings are stored in HKEY_CURRENT_USER\Software\JoliVIM\JvimExt11.
 * JoliVIM deliberately keeps its own registry namespace and never touches
 * Vim's Software\Vim keys, so it can be installed next to a regular Vim
 * without interfering with it.  The package that registers the menu entry
 * (AppxManifest.xml.in) declares the "unvirtualizedResources" capability, so
 * both programs see the real registry.
 */

#ifndef JVIMEXT11_H
#define JVIMEXT11_H

#include <windows.h>
#include <string>

#define SETTINGS_KEY            L"Software\\JoliVIM\\JvimExt11"
#define SETTING_GVIM_PATH       L"GvimPath"     // REG_SZ, empty: automatic
#define SETTING_USE_TABS        L"UseTabs"      // REG_DWORD, default 1

    static inline BOOL
file_exists(PCWSTR path)
{
    return *path != 0 && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/*
 * Locate jvim.exe automatically.  Returns TRUE and fills "buf" when jvim.exe
 * exists.
 * Look in the parent directory of the module "inst": the installer puts
 * jvimext11.dll and jvimext11_cfg.exe in the JvimExt11 directory next to
 * jvim.exe.  JoliVIM does not fall back to Vim's registry keys on purpose, so
 * that it never points at a regular Vim installed on the same machine; if
 * jvim.exe is not found next to the DLL its path is set in the settings window.
 */
    static inline BOOL
get_auto_gvim_path(HINSTANCE inst, WCHAR *buf, DWORD len)
{
    DWORD   n;

    n = GetModuleFileNameW(inst, buf, len);
    if (n > 0 && n < len)
    {
        WCHAR   *sep;

        // Remove the file name, then the JvimExt11 directory.
        for (int i = 0; i < 2 && (sep = wcsrchr(buf, L'\\')) != NULL; i++)
            *sep = 0;
        if (sep != NULL && wcslen(buf) + 10 < len)
        {
            wcscat(buf, L"\\jvim.exe");
            if (file_exists(buf))
                return TRUE;
        }
    }
    *buf = 0;
    return FALSE;
}

/*
 * The jvim.exe chosen in the settings window, or an empty string.
 */
    static inline void
get_custom_gvim_path(WCHAR *buf, DWORD len)
{
    DWORD   size = len * sizeof(WCHAR);

    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, SETTING_GVIM_PATH,
                            RRF_RT_REG_SZ, NULL, buf, &size) != ERROR_SUCCESS)
        *buf = 0;
}

/*
 * Whether to open each file in a new tab page (default: yes).
 */
    static inline BOOL
get_use_tabs(void)
{
    DWORD   value;
    DWORD   size = sizeof(value);
    LSTATUS err;

    err = RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, SETTING_USE_TABS,
                                    RRF_RT_REG_DWORD, NULL, &value, &size);
    return err != ERROR_SUCCESS || value != 0;
}

/*
 * Text "id" from the string table of module "inst" in language "lang", or
 * an empty string.
 */
    static inline std::wstring
load_string_lang(HINSTANCE inst, UINT id, LANGID lang)
{
    HRSRC       res;
    HGLOBAL     mem;
    const WCHAR *text;

    // RT_STRING, without depending on UNICODE.
    res = FindResourceExW(inst, MAKEINTRESOURCEW(6),
                                    MAKEINTRESOURCEW(id / 16 + 1), lang);
    if (res == NULL || (mem = LoadResource(inst, res)) == NULL
            || (text = (const WCHAR *)LockResource(mem)) == NULL)
        return std::wstring();
    for (UINT i = 0; i < id % 16; i++)
        text += 1 + *text;
    return std::wstring(text + 1, *text);
}

/*
 * Text "id" from the string table of module "inst", in the language of the
 * user when there is a translation, otherwise in English.  The translations
 * are in the lang directory.
 */
    static inline std::wstring
load_string(HINSTANCE inst, UINT id)
{
    LANGID          user = GetUserDefaultUILanguage();
    std::wstring    text;

    text = load_string_lang(inst, id,
                        MAKELANGID(PRIMARYLANGID(user), SUBLANG_NEUTRAL));
    if (text.empty())
        text = load_string_lang(inst, id,
                        MAKELANGID(LANG_ENGLISH, SUBLANG_NEUTRAL));
    return text;
}

#endif // JVIMEXT11_H
