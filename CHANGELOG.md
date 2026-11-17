# Changelog

Nickel's change history comes from its commits and, once published, its releases.

- [Releases](https://github.com/crossthepath/nickel-client/releases) contain the published changelogs.
- [Commits on master](https://github.com/crossthepath/nickel-client/commits/master/) show the work since the latest
  release.

Write a short imperative commit or PR title describing the resulting behavior; release notes are written from that
record. Do not add a handwritten entry here for each change.

## 1.3.0-beta

### Game versions

- Minecraft 1.26.65 (the current Microsoft Store / Xbox app version) is supported, along with 1.26.62 and 1.26.63.
- Nickel loads into any copy of Minecraft 1.26.60 or later, from any launcher: when the game is not a build Nickel
  was tested on (a launcher's own copy, a game patched by another program, a newer update), Nickel finds every game
  function it uses by itself and checks each one before hooking it. If one is missing or changed, the menu, HUD and
  scripts still work, game modules stay off, and the launcher says which function differs.
- A refused game now says why ("Load failed" with the reason).

### Launcher

- When a new version is out, the launcher says so in a pop-up as it opens: Update now downloads it, checks its
  signature, and restarts the launcher on the new version; Later closes the pop-up; What's new opens the release notes.

### Nickel AI (soon)

- Nickel AI, an assistant inside the menu that answers questions about modules and servers and can change settings
  or write scripts for you, is coming. It shows as "Soon" in Settings > Connection and cannot be turned on yet.

### PUBLIC

- The online community (PUBLIC) is now on by default; turn it off in Settings > Connection > Online community
  (PUBLIC). A player who already turned it off keeps it off.

### Menu

- The menu fits the game window: it scales down on smaller windows instead of going past the edges.
- Client keys (module binds, Right Shift, mouse buttons for Auto Clicker, Kill Aura and Aim Assist) no longer act
  while the game is paused, a game screen is open or Minecraft is not the window in front.

## 1.2.0-beta

### PUBLIC

- New PUBLIC pages in the menu: browse the Scripts and Configs shared by the community on nickelclient.com.
- Sort by Popular, Trending or Recent, search, and keep local favourites with Star (listed in the Starred tab;
  starring sends nothing).
- Scripts are always shown as source before install and are installed held: they run only when you press Start.
  Installed items can be updated or uninstalled, and any item can be reported.
- Publish your own scripts and configs, and publish updates to them, from OTHER > Scripting and OTHER > Configs.
  Publishing is final once the site's automatic check passes.
- No account and no sign-in: publishing and reports use an anonymous key created for this install.
- PUBLIC is off by default (Settings > Connection > Online community (PUBLIC)). While it is off, Nickel sends nothing
  to the site.

### Fixes

- A script call past its time limit can no longer be resumed with `pcall`, and callbacks keep their call's limit.
- `client.isKeyDown` and Hold modules read keys only while Minecraft is in the foreground.
- Path Finder no longer reports a false arrival after a finished next-segment search.
- Hidden editor settings are added once, even when the renderer restarts.
- Launcher: an error in one Minecraft check no longer closes the launcher; the error is shown under Load.

### Internal

- Scripts reach the menu, modules and drawing through interfaces (ScriptHost, ScriptCanvas, ScriptLibrary): held
  scripts, Luau compile errors with their line, and a write contract for script files.
- A shared HTTPS and JSON layer (WinHTTP) for PUBLIC and the future Nickel AI; CI builds the release signer and
  checks the update trust chain offline.
