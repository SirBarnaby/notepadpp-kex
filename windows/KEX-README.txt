Notepad++ KEX
=============

This is the official Notepad++ portable package of the same version, except that notepad++.exe
is built from Notepad++'s source with the KEX patch: four views, snap-out windows and the KEX
features below, built in. It calls itself "Notepad++ KEX" in the title bar, the taskbar and the
About box, and its icon is Notepad++'s own with a small sparkle badge in the corner. (plugins\KexSuite is an empty leftover for older update scripts; Notepad++
KEX ignores it and Install-KEX.cmd removes it.)
The updater folder is removed and disableNppAutoUpdate.xml is present, so an official update
can't silently replace the KEX build. (The official notepad++.exe would drop the four views.)

Before you start: right-click the zip > Properties > tick "Unblock" > OK, then extract it.
Windows may still say "unknown publisher" the first time, because this build isn't signed.

Two ways to use it
------------------
1. Try it on its own: run notepad++.exe in this folder. It keeps its settings in this folder
   and doesn't touch your installed Notepad++.
2. Put it into your installed Notepad++: double-click Install-KEX.cmd and allow the admin prompt.
   Your settings, sessions and plugins stay. If your Notepad++ is older than this build, the
   official installer of this version runs first (downloaded from Notepad++'s own GitHub and
   checked for Notepad++'s signature). Uninstall-KEX.cmd puts the official notepad++.exe back.

   At the end it asks whether to install new releases automatically. If you say yes, a daily
   task checks this project's GitHub releases; when a KEX build of a new Notepad++ comes out it
   downloads it and installs it the next time Notepad++ is closed, with a notification.
   Log: %LOCALAPPDATA%\KEX-Notepad++\update.log

   The task runs with admin rights, so it only installs a build signed with the KEX key (each
   GitHub build signs its zip; the matching public key is inside Install-KEX.ps1). Its script and
   downloads are kept in %ProgramFiles%\KEX-Notepad++, which only administrators can change. If
   the repository loses its signing key (the KEX_SIGNING_KEY secret under Settings > Secrets and
   variables > Actions), builds are published unsigned and the task skips them with a notification.

Four views
----------
  * Right-click a tab > Move/Clone to Other View works as before; View > Move/Clone Current
    Document now also has Move to View 1-4, Clone to View 1-4 and Focus View 1-4.
  * The views form a 2x2 grid. Right-clicking the middle splitter and rotating it rotates the grid.
  * Snap out: drag a tab outside Notepad++, or View > Move/Clone Current Document > Move to New
    Window / Snap This View Out. Snap it back by closing its window, dragging its window onto the
    tab bar of Notepad++, or Snap All Views Back In.
  * All the new commands can be given shortcuts in Settings > Shortcut Mapper.
  * Older plugins only know two views. Their commands are blocked (with a message) while view 3
    or 4 has focus, so they can't edit the wrong file.

KEX features (the KEX menu)
---------------------------
The menu bar has a KEX menu of its own, between Plugins and Window: Editing, Tabs, Windows,
Highlighting, Quick Note and Effects. Each submenu has an on/off switch for every feature on that
page and "Settings...", which opens the KEX Settings window on the page of the same name
(KEX > Settings... opens it on the page you looked at last). It is a window of its own, apart from
Notepad++'s Preferences, laid out the same way; each feature has its on/off tick and its settings
next to it, and changes apply at once.
  * Tab snapping (Tabs): press a tab anywhere (its name too) and pull; the tab lifts off the
    bar and follows the mouse. Towards the edge of a view a preview shows where a new view will
    open (half of that view, so a quarter of the window once there are two side by side); drop it
    there. Drop it on another view to move it into that view. Hold Ctrl to clone instead of move,
    Esc cancels. "Lift the tab off the bar" can be unticked. Switched off, dropping a tab on its
    own view shows the old Move/Clone menu.
  * A Notepad++ for each desktop (Windows): starting Notepad++ (taskbar, Start, opening a
    file) on a virtual desktop that has no Notepad++ window opens a new window there instead of
    switching you back to the old one. The new window starts with the same tabs in the same
    views, the same active tabs, carets and scroll positions, the same token styles, and a copy
    of every untitled tab; files show what is saved on disk. After that the windows are independent: each desktop's
    window keeps its own session and crash backup (session-desktop-<id>.xml and
    backup\desktop-<id>\ in Notepad++'s settings folder) and its own log, so starting Notepad++
    on that desktop again brings back its tabs and unsaved text.
    Windows never move between desktops. On a desktop that already has one, Notepad++ behaves
    as usual.
  * Falling letters: deleted text falls off the screen, CS2D style. Opacity (%) makes them
    see-through.
  * Smooth caret: the caret glides to its new position. Blink: Fade in and out (default), On and
    off like Notepad++'s caret, or Never.
  * JetBrains shortcuts (Editing): the IntelliJ editor keys in the editing views, e.g.
    Alt+J / Shift+Alt+J / Ctrl+Shift+Alt+J next / unselect / all occurrences (by default they
    match case and whole words the way Smart Highlighting does; "Alt+J case" and "Whole words"
    on the page change that), Ctrl+W and
    Ctrl+Shift+W extend / shrink the selection, Ctrl+/ comment, Ctrl+D duplicate, Ctrl+Y delete
    line, Shift+Alt+Up/Down move line, Alt+Left/Right switch tabs, Shift+Enter new line below.
    They are real commands: KEX > Editing > JetBrains Commands lists them with their
    keys, and Settings > Shortcut Mapper > Plugin commands (plugin KEX) changes any key ("Change
    keys..." on the page opens it there). Untick "Also the keys Notepad++ uses" to keep
    Notepad++'s own Ctrl+Y (redo), Ctrl+W (close), Ctrl+R, F11 and Ctrl+Num +/- (zoom).
  * Quick note (Ctrl+Alt+M by default): a small borderless note fades in at the
    centre of the screen over whatever you're doing, even with Notepad++ minimised. Press it again
    (or Esc) to fade it out and go back to what you were doing. Drag its top edge to move it and
    its edges to size it. What you type is saved as you go (kex-quicknote.txt in Notepad++'s
    settings folder) and is there next time. Options: line numbers, word wrap, current line
    highlight, always on top, opening where you left it, hiding when you click elsewhere, and its
    opacity. "Note file" keeps the note's text in a file of your choice instead (its encoding and
    line endings are kept; KEX > Quick Note > Open Note File in Notepad++ opens it). The
    smooth caret, falling letters and the copy/cut/delete flashes work in the note too, each with
    its own tick on the page. With a Notepad++ on several desktops,
    the first window answers the hotkey and opens the note on the desktop you're on.
  * F12 hover tabs: in F12 (post-it) mode, touching the top edge of any view shows that view's
    own tab bar over the text, with its close buttons, right-click menu and dragging.
  * Glass text area (Windows, off by default): Windows' own blur shows through the
    background of the text, live, so it follows the window as you move it. Menus, toolbar, tabs
    and title bar stay solid. Style: Blur (default) or Acrylic (Windows' frosted texture, much
    stronger; it can lag while you drag the window on Windows 10). Tint (%) sets how much of the
    theme colour stays on top of the blur (default 40). Text edges: Sharp (default) draws the text
    without smoothing while the glass is on, because smoothed letters always show a thin outline
    of the theme's background on the glass; Smooth and "As Notepad++ draws them" keep smoothing.
    "Margins too" puts the line numbers and folding margin on glass as well. Clicks on the glass
    go to the text under them. Needs Windows' "Transparency effects" switched on (Settings >
    Personalization > Colors). If it misbehaves on your PC, kex.ini [Glass] takes Engine=self
    (blur on the window itself) and KeepRenderer=1.
  * Live token styles (Highlighting): Search > Style All Occurrences of Token (also in the
    right-click menu) keeps following the text: occurrences you type or paste later get the style
    too, and one you edit into something else loses it. The same submenu has Bold, Italic and
    Clear This Token. Search > Style One Token has Bold and Italic too, for just that one
    occurrence (or any selection, coloured or not). Ctrl+Shift+H (Token Style Menu in Shortcut Mapper) pops the same choices
    up at the caret. Clear Style
    > Clear all Styles clears them. Bold and italic work in every language, plain text included.
    Match case and whole word follow Notepad++'s Highlighting page;
    the colours are Mark Style 1-5 in the Style Configurator. The styled tokens of each file are
    remembered (kex-tokens.txt in Notepad++'s settings folder) unless you untick that.
  * Copy, cut and delete flashes (Effects): copied text flashes once, cut text flashes
    twice, and deleting a selection flashes it red. Each can be switched off, and Strength (%)
    makes them fainter or brighter.
  * Tab slide (Effects): switching tabs slides the next document in from the side it is on,
    like switching desktops in Windows 10. "Slide (ms)" sets its length.
  * Tab close and open (Effects): a closed tab collapses and the tabs on its right slide over to
    close the gap, as in Chrome and Firefox; a new tab opens out. The document area changes at
    once; with Tab slide on, the document that takes a closed one's place slides in from its side.
    "Close and open (ms)" sets the length.
  * Save glint (Effects): saving a file sends a glint across its tab, wherever the tab is. Only
    when the tab can't be seen (tab bar hidden, or scrolled away) does it run along the view's top.
  * Paint splash (Effects): giving text a colour (Style All Occurrences of Token, Style One
    Token, the token menu) splashes paint: the word lights up in its colour and drops fly off it.
    "Drops per token" sets how many.
    The effects follow Windows' "Animation effects" setting unless you untick that. The falling
    letters, flashes and splashes are drawn on a thread of their own and put on screen by Windows'
    compositor; the tab slide and the caret are drawn by Notepad++'s own renderer (DirectWrite,
    on the graphics card, by default).

New Notepad++ versions
----------------------
The four views are a source patch. A GitHub Actions build applies it to every new Notepad++
release, tests it, and publishes the result (see kex-repo.txt for the repository). If a release
ever changes Notepad++ too much for the patch, that build stops, an issue is opened in the
repository, and you keep the version you have until the patch is updated.

source\ holds everything this was built from (patch, KEX feature source, KexApi.h for plugin authors).
Notepad++ is GPL (license.txt); the KEX changes are under the same licence.
