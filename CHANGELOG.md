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
- Nickel loads into any copy of Minecraft 1.26.60 to 1.26.69, from any launcher: when the game is not a build Nickel
  was tested on (a launcher's own copy, a game patched by another program, a small hotfix), Nickel finds every game
  function it uses by itself and checks each one before hooking it. If one is missing or changed, the menu, HUD and
  scripts still work, game modules stay off, and the launcher says which function differs.
- A refused game now says why ("Load failed" with the reason).

### Nickel AI

- Nickel AI, an assistant inside the menu: a bubble beside the menu opens a chat window. Ask about modules, settings
  and scripts; it can look them up, turn modules on or off, change settings, write scripts and undo its changes.
- Every change waits on a card that shows the real current and new values; nothing happens until you press Allow.
  Scripts it writes are saved held and run only when you start them.
- Answers show their formatting (bold, lists, headings, code).
- Up to 25 messages, one more every 30 minutes; the chat shows how many are left and when the next one comes back.
  No account and no sign-in: Nickel AI uses the anonymous key of this install.
- It sees the game: the server you are on (named, not just its address), what the server sent since you joined
  (custom items, commands, scoreboards), the players and entities around you, and it can capture packets while you
  play. It searches the web and Mojang's protocol documentation, and checks every script against Nickel's API
  before saving it.
- It keeps working while you close the chat and play, and a notice tells you when it is done or needs you.
- Long chats are summarised instead of filling up.
- Nickel AI memory (Settings > Connection, on by default): short notes about you are kept for your next chats.
  Turn it off and Nickel AI keeps nothing.
- The chat window can be resized from its corner, long messages wrap, and right click on a message copies it or
  its code.
- Nickel AI is on by default; turn it off in Settings > Connection > Nickel AI (assistant). While it is off, Nickel
  sends nothing.

### PUBLIC

- The online community (PUBLIC) is now on by default; turn it off in Settings > Connection > Online community
  (PUBLIC). A player who already turned it off keeps it off.

### Menu

- The menu fits the game window: it scales down on smaller windows instead of going past the edges.
- The Nickel AI bubble can be dragged; double-click its window's header to put it back.

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
