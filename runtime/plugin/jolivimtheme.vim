" JoliVIM: use the "onedark" color scheme when the GUI is in dark mode.
" Maintainer:	Sylvain Cresto
" Last Change:	2026 Nov 04
"
" See |jolivim-colorscheme|.  To disable it, in your vimrc:
"	let g:loaded_jolivimtheme = 1

if exists('g:loaded_jolivimtheme') || !(has('winui') || has('gui_adwaita')) || index(v:argv, '--clean') >= 0
  finish
endif
let g:loaded_jolivimtheme = 1

" The color scheme set here.  Any other one is the choice of the user.
let s:applied = ''
let s:dark = 0

func s:Apply(name, background)
  let &background = a:background
  silent! exe 'colorscheme' a:name
  let s:applied = get(g:, 'colors_name', '')
endfunc

func s:Update()
  if get(g:, 'colors_name', '') !=# s:applied
    return
  endif
  let dark = has('gui_dark')
  if dark == s:dark
    return
  endif
  let s:dark = dark
  if dark
    call s:Apply('onedark', 'dark')
  else
    call s:Apply('default', 'light')
  endif
endfunc

augroup jolivimtheme
  au!
  au GUIEnter * call s:Update()
  au User JoliVIMDarkMode call s:Update()
augroup END
