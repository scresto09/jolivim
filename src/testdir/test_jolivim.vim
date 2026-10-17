" Tests for the JoliVIM-specific, script-testable additions.
" The WinUI GUI itself (dialogs, title-bar tabs, dark mode, ...) can only be
" verified by hand; see TESTING.JoliVIM.md for the manual checklist.

source util/check.vim

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

" vim: shiftwidth=2 sts=2 expandtab
