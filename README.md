# README #

This library implements advanced Win32 console input with mouse and clipboard support. 

### Features ###

* Unicode-only
* Single-line and multi-line mode
* Mouse and keyboard selection
* Clipboard support via Ctrl+C, Ctrl+X, Ctrl+V
* Search console output with Ctrl+F
* Clickable links anywhere in the output (can be clicked during `hyper_console_readline()`)
* Select any part of the console window with mark mode (mouse or Ctrl+M): 
  - Rectangle selection with pressed Alt key, otherwise line selection. 
  - during mark mode: Tab to select next/previous link. Space to click current link.
* Highlight matching bracket
* surround selection with (...), [...], "..." when one of the delimiters is entered.
* Input history
* customizable auto-completion with Tab/Shift+Tab/Esc
* customizable keyboard shortcuts

### Goals/TODO ###

* (customizable) syntax highlighting
