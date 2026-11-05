" Tests for the JoliVIM-specific, script-testable additions.
" The WinUI GUI itself (dialogs, title-bar tabs, dark mode, ...) can only be
" verified by hand; see TESTING.JoliVIM.md for the manual checklist.

source util/check.vim
source util/shared.vim

" The 'toolbariconstyle' ('tbst') option: value validation and completion.
func Test_toolbariconstyle_option()
  CheckMSWindows
  if !exists('+toolbariconstyle')
    throw 'Skipped: the toolbariconstyle option is not available'
  endif

  " Default value.
  set toolbariconstyle&
  call assert_equal('auto', &toolbariconstyle)

  " Valid values, also through the short name 'tbst'.
  for val in ['win11', 'classic', 'auto']
    exe 'set tbst=' .. val
    call assert_equal(val, &tbst)
    call assert_equal(val, &toolbariconstyle)
  endfor

  " An invalid value is rejected and leaves the option unchanged.
  set tbst=auto
  call assert_fails('set tbst=bogus', 'E474:')
  call assert_equal('auto', &tbst)

  " Command-line completion offers exactly the valid values.
  call assert_equal(['auto', 'classic', 'win11'],
        \ sort(getcompletion('set tbst=', 'cmdline')))

  set toolbariconstyle&
endfunc

" The :about command is defined (shows the GUI About dialog, or falls back to
" the text intro).  Only check that it exists so the test does not block on
" wait_return in a non-GUI run.
func Test_about_command_defined()
  call assert_equal(2, exists(':about'))
endfunc

" has("gui_dark") is only true in the GUI.
func Test_gui_dark_feature()
  if !has('gui_running')
    call assert_false(has('gui_dark'))
  endif
endfunc

" Start the GNOME GUI, return g:colors_name and the dark state after startup.
func s:GuiColors(args)
  let lines =<< trim END
    call writefile([get(g:, 'colors_name', ''), string(has('gui_dark'))], 'Xcolors')
    qa!
  END
  call writefile(lines, 'Xcolors.vim', 'D')
  call delete('Xcolors')
  call system(GetVimCommand() .. ' -g -f ' .. a:args .. ' -S Xcolors.vim')
  call WaitForAssert({-> assert_true(filereadable('Xcolors'))})
  let res = readfile('Xcolors')
  call delete('Xcolors')
  return res
endfunc

" In dark mode the "onedark" color scheme is used, unless the user picked one.
func Test_dark_colorscheme()
  CheckFeature gui_adwaita
  CheckCanRunGui

  let rc = '-u NORC -N -U NONE --cmd "set go+=d"'
  call assert_equal(['onedark', '1'], s:GuiColors(rc))
  call assert_equal(['desert', '1'], s:GuiColors(rc .. ' --cmd "colorscheme desert"'))
  call assert_equal('', s:GuiColors(rc .. ' --cmd "let g:loaded_jolivimtheme = 1"')[0])
endfunc

" The color scheme follows the dark mode while Vim is running, until the user
" selects another one.
func Test_dark_colorscheme_switch()
  CheckFeature gui_adwaita
  CheckCanRunGui

  let lines =<< trim END
    let g:res = []
    func Add()
      call add(g:res, get(g:, 'colors_name', '') .. ' ' .. &background)
    endfunc
    " One timer at a time: timers that are due together run newest first.
    let g:steps = [[100, 'set go+=d'], [1500, 'call Add()'],
          \ [100, 'set go-=d'], [1500, 'call Add()'],
          \ [100, 'colorscheme desert'], [100, 'set go+=d'], [1500, 'call Add()'],
          \ [100, "call writefile(g:res, 'Xcolors')"], [0, 'qa!']]
    func Run(cmd, timer)
      exe a:cmd
      call Next()
    endfunc
    func Next()
      let [delay, cmd] = remove(g:steps, 0)
      call timer_start(delay, function('Run', [cmd]))
    endfunc
    call Next()
  END
  call writefile(lines, 'Xcolors.vim', 'D')
  call delete('Xcolors')
  let cmd = 'ADW_DEBUG_COLOR_SCHEME=prefer-light ' .. GetVimCommand()
  call system(cmd .. ' -g -f -u NORC -N -U NONE -S Xcolors.vim')
  call WaitForAssert({-> assert_true(filereadable('Xcolors'))})
  call assert_equal(['onedark dark', 'default light', 'desert dark'], readfile('Xcolors'))
  call delete('Xcolors')
endfunc

" Errors in a gvimrc don't block starting the GUI and are in the messages.
func Test_startup_error_banner()
  CheckFeature gui_adwaita
  CheckCanRunGui

  call writefile(['thisisnotacommand'], 'Xgvimrc', 'D')
  let lines =<< trim END
    call writefile([execute('messages')], 'Xcolors')
    qa!
  END
  call writefile(lines, 'Xcolors.vim', 'D')
  call delete('Xcolors')
  " Without --gui-dialog-file, that uses a dialog instead of the banner.
  let cmd = substitute(GetVimCommand(), '--gui-dialog-file \S\+', '', 'g')
  call system(cmd .. ' -g -f -u NORC -N -U Xgvimrc -S Xcolors.vim')
  call WaitForAssert({-> assert_true(filereadable('Xcolors'))})
  call assert_match('E492', readfile('Xcolors')->join())
  call delete('Xcolors')
endfunc

" Started from a terminal, errors in a gvimrc don't go to that terminal.
func Test_startup_error_not_in_terminal()
  CheckFeature gui_adwaita
  CheckFeature terminal
  CheckCanRunGui

  call writefile(['thisisnotacommand'], 'Xgvimrc', 'D')
  let lines =<< trim END
    call writefile([execute('messages')], 'Xcolors')
    qa!
  END
  call writefile(lines, 'Xcolors.vim', 'D')
  call delete('Xcolors')
  let cmd = substitute(GetVimCommand(), '--gui-dialog-file \S\+', '', 'g')
  let buf = term_start(cmd .. ' -g -f -u NORC -N -U Xgvimrc -S Xcolors.vim')
  call WaitForAssert({-> assert_true(filereadable('Xcolors'))})
  call WaitForAssert({-> assert_equal('finished', term_getstatus(buf))})
  call assert_match('E492', readfile('Xcolors')->join())
  call assert_notmatch('E492', term_getline(buf, 1) .. term_getline(buf, 2))
  call delete('Xcolors')
  exe buf .. 'bwipe!'
endfunc

" vim: shiftwidth=2 sts=2 expandtab
