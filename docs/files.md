# Files

Sending files between Puddles, in a DM or a group: any kind of file, several to a message with an optional caption.
Photos open in the app, videos play in it, and anything else can be saved or opened with the system's own app. Built on
the branch `feat/multimedia`, in the order below. The endpoints are in `api/openapi.yaml` (the owner's app) and
`api/federation.yaml` (Puddle to Puddle); `scripts/sync-doco.sh` publishes both, and this page, to the `doco` repository.

Code: `storage/files` (the bytes, by id), `core/transfers` (fetching), `events/message` (files in a message),
`objects/file` and `api/files.cpp` (the owner's side), `federation/files` (between Puddles), and in the app
`app/composer` (sending) and `qml/chat/MessageFiles.qml`, `MediaViewer.qml` (showing).

## What was decided

| Question | Decision |
|---|---|
| Largest file | The owner's choice: `files.max_size` in `puddle.yaml`, **100 MiB** by default. A Puddle refuses to send or fetch anything larger. |
| Arriving | Fetched by itself up to `files.auto_download` (**100 MiB** by default); anything larger waits for the owner to ask. |
| Groups | Fetched from the sender; if the sender cannot be reached, from any other member who has it. |
| Video | Played once fully downloaded, from the local file (Qt Multimedia). |
| Kept for | As long as a message refers to it. When the last message using a file is deleted, the file goes. |
| A message | Up to **10 files** and an optional caption. Photos are shown as a grid. |
| Choosing files | A paperclip button (the system's file picker), dropping files on the chat, and pasting. |
| Kinds of file | Anything. Photos and videos open in the app; anything else is a card, opened or saved by the owner. |
| Before download | A tiny preview the sending app makes (at most 1 KiB, inside the signed message), plus the name, type and size. |

### Second round: security and bandwidth

Decided after the first round, from a security review (the notes are not kept in the repository):

| Question | Decision |
|---|---|
| A file the owner sends from this computer | **Cloned** into the data folder (APFS `clonefile`, Btrfs/XFS reflink), which takes no space until one side changes; **copied** where the file system cannot clone. Never a symlink or a reference to the original: the Puddle acts only inside its data folder, and moving, editing or deleting the original changes nothing that was sent. |
| How the app hands it over | The app the Puddle runs inside passes the open file (`puddle_server_take_file`). No route takes a path, so nothing on the network can make the Puddle read a file outside its data folder. A phone client uploads, as before. |
| Disk and bandwidth | Disk is the owner's to spend; bandwidth is the owner's to control. Photos and videos under the receiver's automatic limit still arrive by themselves. |
| Asking again | The first time a Puddle delivers a file whole to a peer, for a message, it does so by itself. After that, every new request for it needs the holder's approval, one approval per delivery. A download picking up after an interruption is not a new request (an hour's grace after delivery). |
| Groups | A request goes to every member who can be reached; any who has the file can accept, and their Puddle sends it. Each member's first delivery to a peer is by itself, as above. |
| Too large | A receiver that finds a file over its `files.max_size` does not fetch it, tells the sender, and both chats say so: "Bob accepts files up to 50 MB". |
| Showing | The client's call, not the protocol's. The desktop app embeds photos and videos only; every other file is a card (name, type, size) the owner opens, with no warning. |

## The shape of a file

A file is split into **chunks of 1 MiB** (the last one shorter). Each chunk has a SHA-256, and the file's **id** is the
SHA-256 of all the chunk hashes, one after another, in order (as raw bytes). So:

- one id names the file, wherever it is kept, and two people sending the same file send the same id;
- a receiver first gets the list of chunk hashes, checks that it hashes to the id, and from then on checks every chunk
  as it arrives. A chunk can come from anyone who has it, and a bad one is thrown away alone, not the whole file;
- a download that stops picks up where it left off.

1 MiB keeps every request well under the server's 4 MiB body limit and the client's timeouts, even on a slow home
upload.

## In a message

A message event gains an optional `files` list, signed with the rest of the event:

```json
{"type":"message","chat":"…","sender":"…","ts":1793100000000,"body":"from the beach",
 "files":[{"id":"<64 hex>","name":"IMG_2041.jpg","mime":"image/jpeg","size":3145728,
           "w":4032,"h":3024,"preview":"<base64 of a tiny webp, at most 1 KiB>"},
          {"id":"<64 hex>","name":"swim.mov","mime":"video/quicktime","size":48234496,
           "w":1920,"h":1080,"duration_ms":14200,"preview":"…"}]}
```

- `body` becomes optional when there are files. `name` is a file name only (no folders), at most 255 bytes;
  `mime` is what the sender says it is, and the receiver checks the bytes before showing anything inline.
- `w`, `h` and `duration_ms` are for laying out the message before the file arrives. Previews are optional.
- An event may now be up to **32 KiB** (from 16 KiB), to fit ten previews and a full caption.
- Edits change the caption only; the files of a message never change. Deleting the message deletes its files for
  everyone who no longer has another message using them.

## Between Puddles

Two new routes, for a peer with an open stream, signed like every other stream request:

| Route | Answers |
|---|---|
| `GET /stream/v1/files/{file}/chunks?chat=&message=` | the chunk hashes, 32 bytes each, as bytes (the size is in the message) |
| `GET /stream/v1/files/{file}/chunks/{n}?chat=&message=` | chunk `n`, as bytes (`application/octet-stream`) |

The request names the message that carries the file and its chat, so no index is needed: a Puddle serves a file only to
a peer in that chat (the DM with them over an open stream, or a group both are in), only if that message carries it,
and only the chunks it has. Anything else is a `404`, like everything else that is refused.

**Fetching** is a queue of its own (`core/transfers`), not the outbox: one download at a time per peer, chunks in order,
progress saved as it goes, retried with the outbox's back-off when the other side is away. Where the chunks come from is
a hook `federation/` fills in, as the outbox's sending is. In a group, the sender is asked first, then the other members
the receiver has a stream with, in turn.

## The owner's API

Uploading in chunks, so the app and a phone client never send more than 1 MiB at once:

| Route | Does |
|---|---|
| `POST /api/v1/files/uploads/create` `{size}` | starts an upload: `{upload, chunk_size, chunks}` (name and type come with the message) |
| `POST /api/v1/files/uploads/{upload}/chunks/{n}` (bytes) | one chunk |
| `POST /api/v1/files/uploads/{upload}/finish` | checks it is whole: the `File` |
| `POST /api/v1/files/uploads/{upload}/cancel` | throws it away |
| `POST /api/v1/conversations/{id}/messages/send` | gains `files: [{id, name, mime, preview?, w?, h?, duration_ms?}]`; `body` may then be left out |
| `GET /api/v1/files/{file}` | the `File`: size, state (`available`, `downloading`, `waiting`, `too_large`, `failed`), bytes so far |
| `GET /api/v1/files/{file}/content` | the bytes, read from disk as they are sent, with `Range` |
| `POST /api/v1/conversations/{id}/messages/{message}/files/{file}/download` and `/cancel` | fetch one that waits (over the automatic limit) or failed, or stop and throw away what came |

A `Message` gains `files: [File]`. Uploaded files no message uses are thrown away after a day.

## Kept where

```
<data_dir>/files/<id>              the file, once whole
<data_dir>/files/<id>.chunks       its chunk hashes
<data_dir>/files/partial/<id>      a download or upload in progress, and which chunks it has
<data_dir>/files/refs/<id>            which messages carry it, and when it was first seen
<data_dir>/state/transfers/<id>.json  a download: its chat and message, who to ask, how it is going
```

Profile pictures stay in `media/` as they are.

A message adds itself to its files' `refs` when it is sent or arrives (fetched or not), and a deletion by its author
takes it off; when the last goes, the file goes, and so does any download of it. Once an hour the downloads worker
sweeps away what no message has carried for a day (an upload never sent, or a file whose message never arrived whole)
and uploads left unfinished for a day. `GET /api/v1/files/usage` says how many files are kept and the room they take.

## In the desktop app

- **Sending:** a paperclip beside the message box, dropping files on the chat, and pasting. Chosen files wait in a row
  above the message box, with a preview and a remove button, and go when the message is sent. The desktop app hands
  each to its Puddle to clone (above); a phone client uploads it in chunks.
  The app makes the previews: photos with Qt's image reading, videos from their first frame (Qt Multimedia), along with
  the width, height and length.
- **In the chat:** photos as a grid (the preview, blurred, until the photo is here), videos as a tile with a play
  button and the length, anything else as a card with its name, size and a progress bar. Over the automatic limit:
  a Download button.
- **Opening:** a photo fills the window (zoom, and the other photos of the same message); a video plays there with
  play, pause, seeking and volume. Anything else: Save as… and Open (the system's app), with a warning for programs and
  scripts. Escape closes.
- The app downloads each file to its own cache from `/api/v1/files/{file}/content` once the Puddle has it.

Qt Multimedia joins the app's requirements (on Linux it needs a media backend, such as `qt6-multimedia` with FFmpeg or
GStreamer). `puddled` itself needs nothing new.

## Order of work

Each step left the build and every test passing, and updated `api/*.yaml` and the `doco` repository as it went.

1. [x] **This design**, and `doco` set up: the specs copied in (`scripts/sync-doco.sh`), an overview, and this page.
2. [x] **Serving bytes:** the server sends a file from disk as it reads it, with `Range` (`puddle_answer_file`).
3. [x] **The file store** (`storage/files`): chunking, ids, partial files, uploads; `files.*` in `puddle.yaml`.
4. [x] **Uploading and sending:** the upload routes, `files` in a message event (and its checks), the `File` object,
   the 32 KiB event limit.
5. [x] **Fetching between Puddles:** the two stream routes and who may use them; `core/transfers`; the automatic limit
   and the largest size; other group members as sources. Tested end to end in a DM (`e2e`) and in a group whose sender
   is offline (`group_e2e`), and with a fake network (`transfers`).
6. [x] **Keeping and letting go:** a file goes with the last message carrying it; unsent uploads go after a day.
7. [x] **The app, sending:** the paperclip, dropping, pasting, previews, uploads.
8. [x] **The app, showing:** the grid, cards, downloading, the photo viewer and the video player (`ui_test`; the video
   part runs only on a real platform: `PUDDLE_TEST_VIDEO=<clip> ./build/ui_test`).
9. [x] **Wrap-up:** requirements (Qt Multimedia), and a last pass over the docs.

Second round:

10. [x] **Clone, don't upload:** `puddle_fd_clone_to` (platform), `puddle_files_upload_adopt` (store),
    `puddle_server_take_file` (service), and the app hands picked files over instead of uploading them (`files` test).
11. [ ] **Too large, said openly:** the refusal goes back to the sender; both chats show it.
12. [ ] **Asking again:** delivered-once records, requests, approvals, grants; the stream and owner routes; the app's
    prompt.
13. [ ] **Plain file cards:** no warning before opening a file that is not a photo or video.

## Not in this

Playing a video while it downloads; sending a file to someone you have no chat with; voice notes; a settings screen for
the two limits (they are in `puddle.yaml` for now); fetching through a Levee.

Known gaps, for later:

- A peer asking for a file through a message that was since deleted is still served if another message in the same chat
  carries the file (it is in that chat either way; the deletion is not looked up when serving).
- The app sees a download's progress only as often as it reads the conversation (every two seconds), until
  `/api/v1/updates` exists.
- A message's files never change, and an edit cannot add or take one away.
- Disk use is shown by `GET /api/v1/files/usage`, but there is no cap on it yet beyond `files.max_size` per file.
