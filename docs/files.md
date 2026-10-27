# Files (draft)

Sending files between Puddles, in a DM or a group: any kind of file, several to a message with an optional caption.
Photos open in the app, videos play in it, and anything else can be saved or opened with the system's own app. This is
the design the code will follow (branch `feat/multimedia`). The endpoints go in `api/openapi.yaml` (the owner's app)
and `api/federation.yaml` (Puddle to Puddle) as each step is built, and the published copy in the `doco` repository is
updated at the same time.

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
| Kinds of file | Anything. Photos and videos open in the app; programs and scripts are saved, never opened, without a warning first. |
| Before download | A tiny preview the sending app makes (at most 1 KiB, inside the signed message), plus the name, type and size. |

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
  above the message box, with a preview and a remove button, and go when the message is sent; each shows its upload.
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

Each step leaves the build and every test passing, and updates `api/*.yaml` and the `doco` repository as it goes.

1. **This design**, and `doco` set up: the current specs copied in (`scripts/sync-doco.sh`), an overview, and this page.
2. **Serving and taking bytes:** the server sends a file from disk as it reads it (with `Range`), and takes a raw
   `application/octet-stream` body; the client fetches one chunk at a time. Tests for both.
3. **The file store** (`core/files`): chunking, ids, partial files, which messages use what; `files.*` in `puddle.yaml`.
4. **Uploading and sending:** the upload routes, `files` in a message event (and its checks), the `File` and `Message`
   objects, the 32 KiB event limit.
5. **Fetching between Puddles:** the two stream routes and who may use them; `core/transfers`; the automatic limit and
   the largest size; trying other group members. An end-to-end test of a DM and of a group whose sender is offline.
6. **Keeping and letting go:** a file goes with the last message using it; unsent uploads go after a day.
7. **The app, sending:** the paperclip, dropping, pasting, previews, uploads.
8. **The app, showing:** the grid, cards, downloading, the photo viewer and the video player; `ui_test` covers them.
9. **Wrap-up:** README requirements (Qt Multimedia), CI, and a last pass over the docs.

## Not in this

Playing a video while it downloads; sending a file to someone you have no chat with; voice notes; a settings screen for
the two limits (they are in `puddle.yaml` for now); fetching through a Levee.
