# README #

This library implements advanced Win32 console input with mouse and clipboard support. 

### Features ###

* Unicode-only
* Single-line and multi-line mode
* Mouse and keyboard selection
* Clipboard support via Ctrl+C, Ctrl+X, Ctrl+V
* Highlight matching bracket
* Clickable links anywhere in the output (can be clicked during `read_input()`)
* surround selection with (...), [...], "..." when one of the delimiters is entered.

### Goals/TODO ###

* Input history
* (customizable) auto-completion with Tab/Shift+Tab/Esc
* Expand/shrink selection with Ctrl+./Ctrl+Shift+.
* (customizable) syntax highlighting