# jamcli 

A terminal chat, voice, and video client for the [Jami](https://jami.net)
(libjami / GNU Ring) peer-to-peer messaging network. 

[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/tknv/jamcli)   

`jamcli` is a slash-command driven, single-binary UI on top of `libjami`.
It lets you log in to (or create) a Jami account, add and message
contacts over swarm conversations, send files and inline images, place
audio/video calls, and record push-to-talk voice or video messages —
all from a terminal.

## Features

- Slash-command driven chat over Jami's **swarm** conversations
  (end-to-end distributed, no central server required).
- Contact management: add by hash, accept/list invitations, short
  numeric aliases (`@01`, `@02`, ...) for quick reference.
- Audio and video calling, with a pluggable video renderer (e.g. `mpv`,
  or a `raw` mode that streams frames to a Unix socket for a remote
  viewer over SSH).
- File and inline image transfer, with locally logged message/file IDs
  you can reopen later (`/open<ID>`).
- Push-to-talk voice and video messages recorded via `ffmpeg`, with a
  live preview while recording.
- Message replies (both to the underlying Jami swarm commit and to a
  local jamcli message ID) and message deletion.
- Local display name and Jami public username (name-service)
  registration.
- Presence-aware contact list (online/offline, confirmed/pending).
- An escape hatch to run arbitrary shell commands (`/shell`) without
  leaving the client.
- Simple line editor: UTF-8 aware cursor movement, backspace, and
  Alt+Enter for multi-line messages, plus paste-friendly bulk input
  handling.

## No features  

jamcli will not show previous messages. But from `/shell` can see it. 

### First find conversation ID  
```bash
grep -RIl --binary-files=without-match -F "<your companion hash>" "$HOME/.local/share/jami/<yout account id>" 2>/dev/null | sed -n 's#^.*/conversation_data/\([^/]*\)/.*#\1#p' | sort -u
```

### Second get previous messages
```bash
git -C "$HOME/.local/share/jami/<yout account id>/conversations/<the conversation id>" -c i18n.logOutputEncoding=UTF-8 log --all -10 --date=iso-strict --pretty='%H%x09%ad%x09%s%n%b'
```
It will show 10 latest messages.  Then you can reply to by `/#<message id> your reply message` 

## Requirements

- **libjami** — the Jami/GNU Ring core library. `jamcli` talks to it
  through its C interfaces for calls, account/configuration management,
  swarm conversations, file transfer, presence, and video.
- **ffmpeg** — required to record push-to-talk voice/video messages.
- **ffplay** or **mpv** — used to preview incoming media, received
  files/images, and in-progress video-message recordings.
- **timg** - required to show image inline. E.g. show avatar picuture. 
- A POSIX environment (the client uses `fork`/`exec`, named pipes,
  `poll`, and raw terminal mode).

If `ffmpeg` or `ffplay`/`mpv` are missing, `jamcli` still runs, but the
corresponding commands (`/am`, `/vm`, media previews) print an error
and are skipped.

## Installation

`jamcli` is built as part of a larger C++ project that links against
`libjami`. Please refer to the project's build system (e.g. CMake/Makefile) 

## Quick start

```sh
jamcli
```

On first run, if no Jami account exists yet, `jamcli` walks you through
creating one interactively (including, optionally, registering a public
Jami username). On subsequent runs, it logs the existing account back
in automatically.

Once logged in:

```
/list                 # see your contacts (aliases @01, @02, ...)
/add <peer-hash>       # send a contact request
/list all              # also see pending requests
/accept <peer-hash>    # accept a request someone sent you
/chat @01              # start chatting with contact @01
hello there             # plain text is sent to the current chat
/audio-call             # start an audio call with them
/e                       # leave the chat, back to the contact list
/quit                    # exit jamcli
```

## Command reference

All commands are typed at the prompt and are case-insensitive. Any line
that doesn't start with `/` is sent as a plain chat message to the
current companion.

| Command | Aliases | Description |
|---|---|---|
| `/login` | `/me` | Log in the current/first account, or start account setup if none exists. |
| `/logout` | | Log out, optionally exporting an encrypted backup archive first. |
| `/list [all]` | | List contacts (alias, display name, hash, registered name, status). `all` also shows pending invitations. |
| `/add <hash>` | | Send a contact request to a Jami hash. |
| `/accept <user>` | `/ac` | Accept a pending invitation from `user`. |
| `/chat @<ID>` | | Switch the active chat to the contact at alias `@ID`. |
| `/add-img <file>` | | Set your avatar image. |
| `/give-displayName <name>` | | Set your local, unregistered display name. |
| `/give-registerName <name>` | | Register a public Jami username (one-time network op). |
| `/audio-call` | `/acal` | Start an audio call with the current chat companion. |
| `/video-call` | `/vcal` | Start a video call with the current chat companion. |
| `/receive` | `/r` | Accept an incoming call, or an incoming file if there's no call. |
| `/cancel` | `/c` | Cancel the active outgoing call / file transfer. |
| `/hangup` | `/h` | Hang up the active call. |
| `/mute` / `/unmute` | | Mute/unmute your outgoing video. |
| `/send <file>` | | Send a file to the current chat companion. |
| `/am` | | Start/stop a push-to-talk **voice** message recording. |
| `/vm` | | Start/stop a push-to-talk **video** message recording. |
| `/e` | `/end` | Leave the current chat, back to the contact list. |
| `/quit` | `/q` | Exit jamcli. |
| `/#<commit-id> <text>` | | Reply to a specific Jami swarm commit. |
| `/@<local-id> <text>` | | Reply to a locally logged message by its short jamcli ID. |
| `/del@<local-id>` | | Delete a message locally, and notify the peer. |
| `/open<ID>` | `/o<ID>` | Reopen a previously logged file/image (2-digit hex ID). |
| `/shell <command>` | | Run a shell command and print its output. |
| `/help` | | Print the full command list. |

## Key bindings

| Key | Effect |
|---|---|
| `Enter` | Send the current line. |
| `Alt+Enter` | Insert a newline in the input, for multi-line messages. |
| `←` / `→` | Move the cursor (UTF-8 aware). |
| `Backspace` | Delete the previous character (UTF-8 aware). |
| `Ctrl-D` (on empty line) | Quit. |


## Video rendering

`jamcli` supports pluggable video renderers, selected when the app is
started. Observed in the source:

- **`mpv`** — opens video in an external `mpv` window, when `mpv` is
  installed.
- **`raw`** — instead of rendering locally, `jamcli` writes raw RGB0
  video frames to a local Unix socket and prints a ready-to-run command
  to pull and view that stream remotely, for example:

  ```sh
  ssh <host> jamcli --raw-renderer local <socket-path> \
    | ffplay -f rawvideo -pixel_format rgb0 -video_size <W>x<H> -i -
  ```

  This is handy when running `jamcli` on a headless/remote machine and
  viewing the call on your local desktop over SSH.

## Configuration and data files

`jamcli` reads a small amount of local state on top of the account data
managed by `libjami` itself:

- Per-account config, checked in this order:
  `~/.config/jami/<account-id>/config.yml`,
  `~/.config/jami/<account-id>/config.yaml`,
  `~/.local/share/jami/<account-id>/config.yaml`,
  `~/.local/share/jami/<account-id>/config.yml`
  (used to enrich status output such as display name, network
  interface, and device name).
- `~/.local/share/jami/<account-id>/profile.vcf` — the account's own
  vCard, used to preview your avatar on login.
- Temporary files under `/tmp/jamcli-*` for push-to-talk recordings and
  live video previews (voice: `.m4a`, video: `.mp4`, preview pipe:
  `.nut`, ffmpeg log: `.log`); these are process-scoped (named with the
  PID) and safe to ignore/clean up between runs.

Network- and account-level settings (STUN/TURN, UPnP, local peer
discovery, DHT proxy/bootstrap, account manager/JAMS server, network
interface) are read from `jamcli`'s own configuration object and pushed
into the `libjami` account details at login time. The exact
configuration file/format for these settings lives outside `app.cpp`
and isn't covered here. 

Copyright (C) [2026]  
This work incorporates AI-assisted.

## This program is free software:

you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see [https://www.gnu.org/licenses/](https://www.gnu.org/licenses/).
