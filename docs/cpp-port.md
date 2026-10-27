# C++ port: working log

The overnight port of Puddle from C to C++, with the owner API reshaped around objects (`docs/objects.md`).
Everything is uncommitted on `master`. Each step below says what changed, and whether the build and tests passed.

The plan, in order:

1. Baseline: build and run every test before touching anything.
2. Compile `src/core` and `puddled` as C++ with no change in behaviour, so the existing tests prove nothing moved.
3. The router learns path parameters (`/api/v1/users/{user}`).
4. Objects in C++ (`src/core/objects/`), each written as JSON by one function.
5. The owner API moved to the object routes and shapes in `docs/objects.md`.
6. The desktop app (`src/gui/app`) moved onto the new routes.
7. Platform and storage layers, if time allows.

## Log

### 1. Baseline

`scripts/build.sh` on the untouched tree: everything built, 37 of 37 tests passed.

### 2. The core compiles as C++

- `src/core/*.c` became `.cpp`, and `src/service/main.c` became `main.cpp`. CMake now builds them as C++20
  (`CMAKE_CXX_STANDARD` went from 17 to 20). The headers keep their `extern "C"` guards, so the C tests in
  `tests/` still compile as C and call the same functions.
- What C++ refused, and how each was fixed without changing behaviour:
  - 163 conversions from `void *` (mostly `malloc` results and `qsort`/callback `void *user`): a C-style cast to
    the target type. These are placeholders, to become `static_cast` or go away as each module is modernised.
  - 46 `goto`s that jumped over an initialised variable: the declaration and the first assignment were split
    (`int x; x = ...;`), which is what C did anyway.
  - Three ternaries where the cast had to cover the whole `cond ? malloc(..) : NULL`; `EVP_DigestSign` and
    `EVP_DigestVerify` wanting `const unsigned char *`; one `static const` array needing an initialiser.
- Result: everything builds, including the Qt app and its tests. **37 of 37 tests pass.**

### 3. Path parameters in the router

- A route's path can have typed segments: `{user:id}` (a user or group id), `{message:event}` (64 hex) and
  `{token:token}`. A segment only matches a value of its type, so a malformed id is answered exactly like a path that
  does not exist (404), before any handler runs. Literal segments win where both could apply (`users/me`).
- `puddle_param(call, "user")` gives a handler the value. The 405 answer for the owner works with patterns too.
- Handlers may now throw `puddle::Error` (`src/core/error.hpp`). The router answers `{"error": code, "message"?}` with
  its status, so an object can refuse from anywhere without unwinding by hand.
- `tests/router_test.c` covers matching, types, the literal-wins rule, 404 for bad ids and 405.

### 4. Objects

`src/core/objects/`: `User` (and `PartialUsers`, which looks each person up once per answer), `Stream`, `Group`,
`Conversation` and `Message`. Each is got by id, does things through methods that return the object afterwards
(`stream.accept()`, `user.block()`, `message.edit(...)`, `conversation.send(...)`), and is written as JSON by its own
`write()`. The methods reuse the logic of the old handlers almost unchanged; only where the ids come from and what
comes back changed.

### 5. The owner API moved to object routes

`src/core/api/` holds the new route tables (`users.cpp`, `streams.cpp`, `conversations.cpp`, `groups.cpp`), with
`request.hpp` for reading bodies and paging. The old owner handlers are gone: `owner_streams.cpp` entirely, and the
owner halves of `messages.cpp` and `groupapi.cpp` (their federation halves stay). Identity and media routes keep their
paths; `/api/v1/profile` is now `/api/v1/profiles` and `/api/v1/me` is `/api/v1/users/me`.

| Before | After |
|---|---|
| `GET stream`, `POST stream/request` | `GET streams`, `POST streams/request` |
| `POST stream/{accept,decline,delete,trust,profile}` + `{"stream":id}` | `POST streams/{id}/{accept,decline,delete,trust,profile}` |
| `GET/POST stream/policy` | `GET streams/policy`, `POST streams/policy/set` |
| `GET/POST stream/invite`, `POST stream/invite/revoke` | `GET invites`, `POST invites/create`, `POST invites/{token}/revoke` |
| `GET/POST user/block`, `POST user/unblock` | `GET blocks`, `POST users/{id}/block`, `POST users/{id}/unblock`, `GET users/{id}` |
| `GET conversations` | `GET conversations`, `GET conversations/{id}` |
| `GET dm/messages`, `GET gm/messages` | `GET conversations/{id}/messages` |
| `POST dm/send`, `POST gm/send` | `POST conversations/{id}/messages/send` |
| `POST {dm,gm}/edit`, `POST {dm,gm}/delete` | `POST conversations/{id}/messages/{message}/{edit,delete}` |
| `POST dm/read`, `POST {dm,gm}/retry` | `POST conversations/{id}/read`, `POST conversations/{id}/retry` |
| `GET gm`, `POST gm/create` | `GET groups`, `POST groups/create`, `GET groups/{id}` |
| `POST gm/{invite,accept,decline,leave,rename}` + `{"group":id}` | `POST groups/{id}/{invite,accept,decline,leave,rename}` |

What changed in the answers:

- Every list is `{"items": [...], "next": offset|null}`.
- Every POST answers with the object it changed (200; 201 where it made one). The old 204s (decline, delete,
  unblock, read, retry, group invite/decline/leave, message delete) now carry the object.
- A person is a `User` (`{id, profile, stream, blocked}`) in a stream, and a `PartialUser`
  (`{id, display_name, avatar, color}`) wherever else one appears: a message's `author`, a group's `creator` and
  members, a block, a conversation's `recipient` and `last_message.author`.
- A message has `conversation` (the other person's id for a DM, not the internal chat hash), `type`, `author`, and
  `edited_at` without the old `edited` flag. A group's notices are messages in the same list, with a `type` such as
  `group_join`, instead of a separate `notices` list.
- A conversation has `id`, `name` (a DM's is the other person's display name), `last_message` and, for a group, a
  summary of the group (`member_count`, no member list: the conversation list has to fit in one answer).
- `POST groups/create` answers `{"group": Group, "not_invited": [...]}`.
- An id of the wrong form in a path is a 404, where it used to be a 400 from the body.

### 6. The desktop app on the new routes

`src/gui/app/chats.cpp` calls the new paths. Where an answer's shape changed, it is turned back into what the window's
models already read, in one place each (`messagesOf`, `noticesOf`, `asMessage`, `asNotice`, and the conversation and
group mappings). So no QML changed. A message's `author.id` becomes the model's `sender`, a page's `items` are split
back into messages and notices, and a member's `user.id` becomes `id`. The app still keeps its own name, avatar and
colour tables. Moving it onto the objects themselves (a client library with one instance per id, as in
`test_structure/client/`) is the next step for the app, and isn't done.

One bug turned up here and is fixed. The first version of the new message page estimated each message's size. With
the author now inside each message, the estimate could run short, and the page then silently left out its newest
messages while its `next` still counted them. `tests/ui_test.cpp` caught it: its scroll anchoring jumped. The page now
writes each message once on its own to measure it, so a page never holds less than its `next` says.

**All 37 tests pass**, including the Qt ones (`backend`, `chats`, `ui`).

### 7. The API spec

`api/openapi.yaml` now describes the routes above. Each object is its own file in `api/objects/`, the shapes shared
with the stream API are in `api/wire/`, and the list shape is `api/page.yaml`. They are the model from
`test_structure/model/`, without its notes on where each object came from in the old code, and with what the code
actually answers: a conversation's group as a summary, `groups/create` answering `{group, not_invited}`, and invites
as a page. Two routes are listed but marked `x-status: not built yet`, as the old spec had them unbuilt too:
`/api/v1/updates` (the live stream, renamed from `/api/v1/events`) and `/api/v1/profiles/reload`.
`api/federation.yaml` is unchanged.

### 8. Platform: clocks and directories

- `src/core/clock.h`: `puddle_wall_ms()` and `puddle_mono_ms()`, on `std::chrono`. They replace the six copies of
  `clock_gettime(...)` arithmetic in `client`, `identity`, `outbox`, `reqsig`, `router` and `store`.
- `fsutil.h` gains `puddle_dir_open/next/close`, on `std::filesystem`. They replace `opendir`/`readdir` in `group`,
  `identity`, `outbox`, `store` and `streams`. No `dirent.h` is left in the core.
- Both are standard C++, so these parts now build on Windows with nothing more to write.
- Warnings that came with compiling as C++ are fixed too (designated-initialiser order, `sprintf` becoming
  `snprintf`, a string literal passed as `char *`, `{ 0 }` initialisers). The build has no warnings left in `src/`.

A test that fails some of the time, found while doing this: under `ctest -j6`, `tests/e2e_test.c` sometimes fails
"nothing left waiting anywhere" (now line 391, line 372 before tonight). This isn't from tonight's work: the original
code from git, built and run the same way, fails that check in 3 of 4 runs. The check doesn't wait for the outbox to
empty, and under load it sometimes hasn't yet. It passes when the test runs alone. I've left the test as it was.

### 9. Threads and locks: the C++ standard library

No `pthread` is left in `src/core`.

- **Mutexes:** every `pthread_mutex_t` is a `std::mutex`, in `group`, `identity`, `message`, `outbox`, `profile`,
  `ratelimit`, `reqsig`, `routes`, `server`, `store` and `streams`. A struct holding one is now made with
  `new (std::nothrow) T()` and freed with `delete`, where it used `calloc` and `free`. `T()` still zero-fills every
  field, as `calloc` did, and also constructs the mutex.
- **Threads:** the outbox's worker is a `std::thread` that is joined on close. Its per-peer deliveries and the
  server's connections are detached `std::thread`s. A thread that can't be started is handled as a failed
  `pthread_create` was.
- **Waiting:** condition variables are `std::condition_variable`. The worker's timed wait is `wait_for` a number of
  milliseconds, where it used to build an absolute `timespec` by hand.
- The locking itself is unchanged: `mu.lock()`/`mu.unlock()` where `pthread_mutex_lock`/`unlock` were. Moving to
  `std::lock_guard` can come module by module, as each is touched for other reasons.

Checked with: all 37 tests, twice, plus a ThreadSanitizer build (`-fsanitize=thread`) running `outbox`, `serve`,
`e2e`, `group_e2e`, `messages`, `streams` and `store`. Everything passes, and no races are reported in the core. The
races TSan does report are in test code only, where `tests/outbox_test.c` and `tests/tlsserver.h` read counters
another thread writes without taking the lock. Those files are unchanged tonight.

## What is left

- **Sockets and files (the rest of the platform layer):** sockets, `poll` and `getaddrinfo` (`client`, `server`,
  `groupflow`), `flock` (`store`), `fsync`/`rename`/`open`/`unlink` (`fsutil`, `identity`, `store`, `outbox`,
  `streams`, `tls`, ...), `signal(SIGPIPE)` (`server`), and `getpid`/`getuid` (`fsutil`, `identity`, `config`). These
  need a per-OS implementation behind one header (`test_structure/platform/`). The Windows half can't be built or
  tested on this Mac, so I left it rather than write it blind.
- **Storage layer:** `test_structure/storage/` (one interface, a files driver, a memory driver for tests) and the
  records it would hold (`test_structure/model/records/`, people and conversations) aren't built. The stores still
  write their own files.
- **The client library:** the app still keeps its own tables of names and avatars. The next step for it is
  `src/client/`, one Qt object per id, as in `test_structure/client/`.
- **Live updates:** `/api/v1/updates` (Server-Sent Events) is in the spec, marked not built; the app still polls
  every two seconds.
- **C-style casts:** the 163 casts added in step 2 are still C-style. They go as each module is modernised.
- `docs/objects.md` was the draft for this, and its route table has been built as written. Its "Order of work"
  section is now partly done: see the steps above.

## Next: the layout from test_structure

The code is C++ but still sits in `src/core/`. These steps move it into the layers from `test_structure/README.md`,
one at a time, each leaving the build and all tests passing. Tick a step off when it is done.

Where today's files go:

| Layer | Files |
|---|---|
| `platform/` | parts of `fsutil` and `clock`; the socket and lock code in `client`, `server`, `store` |
| `core/` | `config`, `yamltree`, `identity`, `phrase`, `hash`, `encoding`, `json`, `log`, `profile`, `streams`, `group` (records), `outbox` (the queue), `ratelimit`, `link` |
| `storage/` | `store`, and the file writing in `streams`, `group`, `outbox`, `profile` |
| `wire/` | `wireprofile`, the id checks |
| `events/` | the event halves of `message` and `group` |
| `objects/` | `src/core/objects/` |
| `http/` | `http`, `tls`, `serve`, `server`, `router`, `client`, `reqsig` |
| `api/` | `src/core/api/`, and the owner routes in `routes` and `profiles_api` |
| `federation/` | `inbound`, `access`, `flow`, `keysync`, `groupflow`, `groupapi`, `messages`, `media`, the peer half of `profiles_api` |
| `service/`, `app/` | `src/service/`, `src/gui/` |
| `client/` | new |

- [x] **L1. Move the files.** Each file goes to its layer's folder unchanged; includes name the folder
  (`"core/identity.h"`). No behaviour changes. *Done:* `src/core/` was split into `platform/`, `core/`, `storage/`,
  `wire/`, `events/`, `http/`, `objects/`, `api/`, `federation/` and `service/`. Three changes to the table above:
  `server` went to `service/` (it builds the whole running Puddle), `media` to `storage/` (images kept by their hash),
  and `error.hpp` to `core/` (both `http/` and the objects use it). One library target still holds them all; that
  splits in L2. 37 of 37 tests pass.
- [x] **L2. One CMake target per layer**, linking only the layers below it. *Done:* `puddle_platform`, `_core`,
  `_storage`, `_wire`, `_events`, `_objects`, `_http`, `_api`, `_federation` and `_service`. `puddled`, the app and the
  tests link `puddle_service`. To get there, every include that went up a layer was moved or turned around:
  - `puddle_ctx` moved to `core/context.h`. It names the store, streams, outbox and the rest without including them,
    so the objects no longer include `http/router.h`. (Both APIs still share the one context: splitting it was in the
    plan, but turned out unnecessary once it stopped pulling in the layers above.)
  - **The courier** (`puddle_courier` in `core/context.h`): the owner's actions that other Puddles must hear about
    (asking for a stream, answering, group news, profile pushes, rolled keys) are a table of functions the context
    carries. `federation/courier.cpp` fills it (`puddle_federation_courier`) and `service/server.cpp` sets it, so the
    objects and the identity routes call it without knowing about federation. Tests that build a context by hand
    set it too.
  - The list of every route table moved to `service/routes.cpp`. `/stream/v1/access`, which sat in the owner's
    identity table, moved to a federation table of its own.
  - Small ones: `puddle_clean_text` to `core/text`, `puddle_my_address` and `puddle_make_link` to `core/link`,
    and `PUDDLE_PATH_MAX` to `platform/fsutil.h`.
- [x] **L3. Split the mixed modules.** *Done:*
  - `message` turned out to be only about events (signing, building, parsing, folding a chat) and stays whole in
    `events/`.
  - `group`: the ids, the five events and the rules that fold them into who is in it are now
    `events/group_events.{h,cpp}`. The record, its helpers (including `puddle_group_add_member`, now public) and its
    store stay in `core/group`.
  - `profiles_api`: the owner's routes (`/api/v1/profiles`, `/api/v1/media`) are now `api/profiles.cpp`. What peers
    call, and pushing our profile out, stay in `federation/`. Both read `?hash=` and answer with an image through
    two shared helpers in `http/router` (`puddle_query_media_hash`, `puddle_answer_media`).
  - Also fixed: the `e2e` test that failed some of the time (see step 8) now waits up to 10 seconds for both
    outboxes to empty instead of checking at once. It still fails if anything stays stuck. Three runs under
    `ctest -j6` are clean.
- [x] **L4. The outbox only queues.** *Done, ahead of L3, because it was the last cycle in L2:* `core/outbox`
  queues, saves, schedules, retries and runs a thread per peer, but no longer knows HTTP. How an item travels is its
  `send` hook, which `federation/sender.cpp` provides (`puddle_federation_send`, the old `attempt`). What came of a
  delivery (`puddle_net_reply`) is plain data in `core/delivery.h`. The `done` callbacks already lived in
  `federation/` and are set by `service/`. The timeouts stay in `hooks.net`, under the same field names.
- [x] **L5. `tests/layers.cmake`:** a `ctest` test (`layers`) that reads every `#include` in `src/` and fails on
  one a layer may not use. It catches what linking can't (a header-only include). Checked by planting two wrong
  includes in a scratch copy: it named both and failed. `src/gui/` isn't checked: the app embeds the whole Puddle and
  links `puddle_service`.
- [x] **L6. Platform.** *Done on POSIX. The Windows half is written but not yet built or tested* (no Windows machine
  or cross-compiler here; each Windows file says so at the top). Nothing outside `src/platform/` includes a system
  header for files, sockets, signals or the terminal, or checks the OS. Each platform file ends in `_posix` or
  `_windows`, and `CMakeLists.txt` is the one place that picks:
  - `fsutil.h`: plain descriptors (`puddle_fd_open/read/write/close/end/truncate/size/lock`), paths
    (`exists`/`is_dir`/`is_file`/`size`/`remove`/`rename`), `mkdir -p`, the atomic write, directory listing. On
    Windows: the C runtime's `_wsopen_s` family on UTF-16 paths, `LockFileEx`, `MoveFileExW`. Used by `store`,
    `identity`, `profile`, `media`, `outbox`, `streams` and `tls`.
  - `net.h`: resolve to address bytes, dial (each address the caller's `allowed` rule accepts, with a timeout),
    listen, accept, poll, finish (half-close and drain), close, and a waker that any thread or a stop signal can
    trip (a socket pair, so Windows' `WSAPoll` can wait on it). The rule for which addresses may be dialled stays
    in `http/client.cpp`. The server's stop pipe is now a waker.
  - `process.h`: process id, "is this a terminal", echo off while the recovery phrase is typed, the stop signal
    (`sigaction` / `SetConsoleCtrlHandler`), ignoring `SIGPIPE`.
  - `paths.h`: the per-user config and data folders, the login name, resolving a path, the separator. Moved from
    `core/config.cpp`, which has no `#ifdef _WIN32` left.
  - Also changed: the TLS key and certificate are written through a memory BIO and `puddle_fd_open(..., EXCL)`.
    That replaces `fdopen` and avoids passing a `FILE *` into OpenSSL, which crashes on Windows when OpenSSL was
    built with a different C runtime. `strncasecmp` became `puddle_ascii_ncasecmp` (`core/text`), and an `off_t`
    became `int64_t`.
  - Fixed a bug that predates tonight: the atomic write named its temporary file `.tmp-<pid>`, the same for every
    thread. `streams` and the profile assignments both write into `state/` under different locks, so two writes
    at once could clobber each other's temporary file. The name now adds a counter.
  - Checked: 38 of 38 tests, twice under `-j6`, and ThreadSanitizer on `outbox`, `serve`, `client`, `e2e`,
    `group_e2e`, `messages`, `streams` and `store`. No races in the core; the two in test code are as before.
  - **What a Windows build still needs**, beyond checking these files:
    - CMake finding OpenSSL 3 and libyaml (vcpkg is the usual way).
    - The tests use POSIX directly (`mkdtemp`, `nftw`, `unistd.h`), so they won't build there until they go
      through `platform/` too.
    - The app's `dock_mac.mm` is already macOS-only.
    - Running `puddled` as a Windows service (as opposed to a console program) is still to write
      (`test_structure/platform/windows/service.cpp`).
- [x] **L7. Storage:** the stores write through `storage/` (a files driver, keeping today's on-disk layout, and a
  memory driver for tests). Then the `people` and `conversations` records from `test_structure/model/records/`.
  - `storage/storage.h`: records in collections, plus secrets; a files driver (same paths as before) and a memory
    driver. auth, profile assignments, groups, the outbox and streams take `*_open_with(storage)`.
  - `core/people`: the profile a peer's Puddle sent, one record per person (`state/people/<id>.json`). It is no
    longer part of the stream record. At startup, `puddle_streams_take_profiles` moves any profile still in an old
    stream record into its person record and rewrites the stream record without it (runs once; logged).
  - `core/conversations`: a summary per conversation (`state/conversations/<id>.json`) plus the owner's `read_at`.
    The chat log store reports each append (`puddle_store_on_append`). A summary built before its chat's latest
    append is stale and is rebuilt from the log on the next read. After a start, every summary is stale once.
    Read markers now live here; `read.tsv` is read only for a conversation with no record yet.
  - Still on their own interfaces: chat logs and media (files only). Identity and TLS use `platform/` files directly.
  - Checked: 41 of 41 tests (new: `storage`, `people`, `conversations`), a clean `scripts/build.sh --clean`, and
    ThreadSanitizer on streams, people, conversations, messages, e2e, group_e2e and storage with no races.
  - Not tried yet: starting against a real data folder from before this change (the profile move is covered by
    `people_test` only).
- [ ] **L8. The client library:** `client/`, one Qt object per id, and the app moved onto it.

L1–L5 move code without changing what it does, and the tests cover them. L6–L8 change behaviour.
