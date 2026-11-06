# Agent preferences for testing in Waydroid

These are the instructions the author of this branch gives AI coding agents for checking web pages and apps in Waydroid. They rely on the `waydroid-app-displays` command that the install adds.

To use them, copy the section below into your own `AGENTS.md`, `CLAUDE.md`, or the rules file of your agent. Change the browser, the display size, or the density to the ones you test with. The section is short because the launch prints everything else the agent needs: screenshots, touch, a host server, light mode, and DevTools.

```markdown
## Testing web pages in Waydroid

Use Waydroid for any phone check of a web page, next to headless Chrome at phone width: only a real Android browser shows touch behaviour, browser toolbars and forced dark mode. Undo each setup change when the test is done.

1. Use Cromite (Chromium), installed as `org.cromite.cromite`. Waydroid has no browser of its own. If Cromite is missing, install `x64_ChromePublic.apk` from the latest `uazo/cromite` GitHub release.
2. Launch it: `waydroid-app-displays launch org.cromite.cromite --hidden --size 720x1280 --dpi 320`.
   - `--hidden` runs it on a display with no window. Leave it out to open a floating window of that size, for a test the user watches.
   - `--size WxH` sets the display in pixels and `--dpi N` its density. 720x1280 at 320 dpi is a 360 dp wide phone.
3. Follow the commands and notes the launch prints: screenshots, touch, a host server, light mode and DevTools. `waydroid-app-displays info org.cromite.cromite` prints them again.
```

To install Cromite, download `x64_ChromePublic.apk` from the latest [uazo/cromite release](https://github.com/uazo/cromite/releases/latest), then install it while Waydroid runs:

```sh
waydroid app install x64_ChromePublic.apk
```
