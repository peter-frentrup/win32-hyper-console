# README #

This library implements advanced Win32 console input with hyperlinks, mouse and clipboard support. 

This software is available under the *GNU Library General Public License (LGPL 2.0)*, see [LICENSE.md](LICENSE.md).

### Features ###

* Unicode-only
* Single-line and multi-line input
* Mouse and keyboard selection
* Clipboard support via Ctrl+C, Ctrl+X, Ctrl+V
* Search console output with Ctrl+F
* Clickable links anywhere in the output (can be clicked during `hyper_console_readline()`)
* Select any part of the console window with mark mode (mouse or Ctrl+M): 
  - Rectangle selection with pressed Alt key, otherwise line selection. 
  - during mark mode: Tab to select next/previous link. Space to click current link.
* Highlight matching bracket
* surround selection with (...), [...], "..." when one of these delimiters is entered.
* Input history
* Customizable auto-completion with Tab/Shift+Tab/Esc
* Customizable keyboard shortcuts

### Goals/TODO ###

* (customizable) syntax highlighting
