<p align="center">
  <img src="assets/four-views-tab-snapping.gif" alt="A tab is dragged to the edge of a view and dropped, building a 2x2 grid of four editing views in Notepad++" width="100%" />
</p>

<h1 align="center">Notepad++ KEX</h1>

<p align="center">
  <em>Notepad++ with four views, a smooth caret, JetBrains keys and more.</em>
</p>

<p align="center">
  <a href="https://github.com/SirBarnaby/notepadpp-kex/releases/latest"><img src="https://img.shields.io/github/v/release/SirBarnaby/notepadpp-kex?label=download&style=flat&color=24292f" alt="Download the latest release" /></a>
  &nbsp;
  <a href="https://github.com/SirBarnaby/notepadpp-kex/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/SirBarnaby/notepadpp-kex/build.yml?branch=main&label=daily%20build&style=flat" alt="Daily build status" /></a>
  &nbsp;
  <img src="https://img.shields.io/badge/Windows-64--bit-24292f?style=flat" alt="64-bit Windows" />
  &nbsp;
  <a href="LICENSE"><img src="https://img.shields.io/badge/licence-GPL%20v3-24292f?style=flat" alt="GPL v3 licence" /></a>
</p>

Notepad++ KEX is Notepad++ with extra features compiled in: four editor views, tab snapping, a smooth caret, JetBrains shortcuts, a global quick note and more. Each build is made from the official Notepad++ source of the same version. Your settings and plugins keep working, and every feature can be switched off.

<p align="center"><sub><a href="#features">Features</a> · <a href="#see-it-in-action">See it in action</a> · <a href="#install">Install</a> · <a href="#faq">FAQ</a> · <a href="#licence">Licence</a></sub></p>

## Features

- **Four views**: a 2×2 grid instead of two views. Move or clone tabs between any of them.
- **Tab snapping**: drag a tab to the edge of a view to split it. <kbd>Ctrl</kbd> clones, <kbd>Esc</kbd> cancels.
- **Snap-out windows**: drag a tab out of Notepad++ and its view gets its own window.
- **Smooth caret**: the caret glides instead of jumping. It can fade, blink, or stay on.
- **Falling letters**: deleted text falls off the screen. Their opacity can be set.
- **Copy, cut and delete**: copied text flashes, cut text lifts off the page, deleted text shrinks away.
- **Tab slide**: switching tabs slides the next document in, like switching desktops in Windows 10.
- **Tab close**: a closed tab collapses and the tabs beside it slide over, as in Chrome and Firefox. New tabs open out.
- **Save glint**: a glint runs across a tab when its file is saved.
- **Touchpad tab swipe**: swipe two fingers left or right to move to the next tab. The tab follows your fingers, and you can stop half way.
- **Paint splash**: giving a word a colour splashes paint off it.
- **JetBrains keys**: <kbd>Alt</kbd>+<kbd>J</kbd>, <kbd>Ctrl</kbd>+<kbd>W</kbd>, <kbd>Ctrl</kbd>+<kbd>/</kbd>, <kbd>Ctrl</kbd>+<kbd>D</kbd>, <kbd>Shift</kbd>+<kbd>Alt</kbd>+<kbd>↑</kbd>/<kbd>↓</kbd> and more. Rebind any of them in *Shortcut Mapper*.
- **Live token styles**: styled words stay styled as you type. Bold and italic too, for every occurrence or just one.
- **Quick note**: <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>M</kbd> opens a note over any app. It can keep its text in a file you choose.
- **Per-desktop windows**: each virtual desktop gets its own Notepad++ with the same tabs and token styles, its own session and crash backup.
- **F12 hover tabs**: in F12 mode, the tab bar appears when you touch the top of a view.
- **Glass**: Windows' live blur behind the text, with clean letter edges. Off by default.
- **On/off switches**: one per feature, in the KEX submenus at the bottom of the *Settings* menu.
- **Automatic updates**: a KEX build follows every Notepad++ release. Builds are signed, and the updater refuses unsigned ones.

## See it in action

### Smooth caret

<p align="center"><img src="assets/smooth-caret.gif" alt="Typing three lines of text while the caret glides smoothly behind the letters" /></p>

### JetBrains keys and falling letters

<kbd>Alt</kbd>+<kbd>J</kbd> selects the next `void`, both are deleted at once, then `int` is typed into both places.

<p align="center"><img src="assets/jetbrains-keys-falling-letters.gif" alt="Two occurrences of void selected with Alt+J, deleted with falling letters, and replaced by int in both places" /></p>

### Live token styles

Right-click > *Style all occurrences of token*. New occurrences pick up the style as you type them.

<p align="center"><img src="assets/live-token-styles.gif" alt="Right-click menu styling every occurrence of Sony in red and bold, and a newly typed Sony picking up the style" /></p>

### Quick note

<kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>M</kbd> shows and hides the note from any app. The text is saved automatically.

<p align="center"><img src="assets/quick-note.gif" alt="A small dark note fading in over a web browser with Ctrl+Alt+M, receiving text, and fading out again" width="100%" /></p>

### Settings

KEX has its own menu in the menu bar, between *Plugins* and *Window*, with an on/off switch for every feature. *KEX* > *Settings...* opens the KEX Settings window: the options for each feature and whether it is running, apart from Notepad++'s own *Preferences*.

<p align="center"><img src="assets/preferences.gif" alt="Clicking through the Editing, Tabs, Highlighting and Windows pages of the KEX settings" /></p>

## Install

Requires 64-bit Windows.

1. Download `Notepad++-<version>-KEX.zip` from the [latest release](https://github.com/SirBarnaby/notepadpp-kex/releases/latest).
2. Right-click the zip > *Properties* > tick **Unblock**, then extract it.
3. Then either:
   - **Portable:** run `notepad++.exe` from the extracted folder. It keeps its own settings and doesn't touch an installed Notepad++.
   - **Installed:** run `Install-KEX.cmd`. It puts the KEX `notepad++.exe` into your installed Notepad++ and keeps your settings, sessions and plugins. It can also turn on automatic updates.

> [!NOTE]
> The build isn't code-signed, so Windows may show an "unknown publisher" warning. If your installed Notepad++ is older than the KEX build, the installer first runs the official installer of that version, downloaded from Notepad++'s GitHub and checked for Notepad++'s signature.

### Uninstall

Run `Uninstall-KEX.cmd`. It restores the official `notepad++.exe` and removes the update task. See `KEX-README.txt` in the zip for details.

## FAQ

**What happens when Notepad++ releases a new version?**  
A daily build checks for new Notepad++ releases and publishes a matching KEX build. With automatic updates on, it's installed the next time Notepad++ closes. Notepad++'s own updater is disabled so it can't replace the KEX build. If a release breaks the patch, no build is published and you stay on your current version. Every build is signed, and the update task only installs a build whose signature checks out.

**Do my plugins still work?**  
Yes, in views 1 and 2. In views 3 and 4, a plugin's commands are blocked unless the plugin exports `kexAware()` (see [`include/KexApi.h`](include/KexApi.h)).

**How do I get <kbd>Ctrl</kbd>+<kbd>Y</kbd> (redo) back?**  
*KEX* > *Editing* > *Settings...*, untick *Also the keys Notepad++ uses*. This restores <kbd>Ctrl</kbd>+<kbd>Y</kbd>, <kbd>Ctrl</kbd>+<kbd>W</kbd>, <kbd>Ctrl</kbd>+<kbd>R</kbd>, <kbd>F11</kbd> and the zoom keys. To change a single key instead, open *Settings* > *Shortcut Mapper* > *Plugin commands* and edit the KEX entry.

**Glass doesn't show up.**  
Turn it on under *Settings* > *KEX Windows*, and turn on Windows' *Transparency effects* (*Settings* > *Personalization* > *Colors*). *Acrylic* can lag behind a moving window on Windows 10, so while you drag the window KEX shows plain *Blur* and switches back when you let go.

**Is this an official Notepad++ release?**  
No. It isn't affiliated with the Notepad++ project.

Bug reports and ideas go in [Issues](https://github.com/SirBarnaby/notepadpp-kex/issues).

## Licence

GPL v3, like Notepad++ (see [LICENSE](LICENSE)).
