# Profiles

A profile is how the owner appears to someone: name, picture, bios, name colour, links and so on. An owner has
several (work, friends, gaming) and chooses one for each **place**: a direct conversation with a friend, a group, and
later a Pond. Like a Discord server profile, the same person can look different in each place.

**Nothing is shown anywhere the owner has not chosen a profile for.** Choosing a profile for a place is the owner's
consent for it to go there, and only there. Someone the owner has chosen nothing for sees only their user id.

Implemented in `src/core/profile.cpp` (the owner's profiles and their places), `src/wire/wireprofile.cpp` (cleaning
what arrives), `src/core/people.cpp` (what others show us), `src/federation/profiles_api.cpp` (how they travel) and
`src/objects/profile.cpp` (the `Profile` object). Tests: `profile_test`, `wireprofile_test`, `people_test`,
`profiles_test`, and the end-to-end tests.

## Where they live

One YAML file per profile in the data folder: `<data_dir>/profiles/<id>.yaml`. The file's name is the profile's id,
which only the owner sees (`a-z`, `0-9`, `-`, `_`, up to 32). Puddle writes these files when a profile is created or
changed through the owner API (`/api/v1/profiles/...`). They can also be edited by hand while Puddle is not running.

    # A Puddle profile.
    display_name: "Ali"
    blurb: "Healer main"
    about: "Longer description,\nover several lines."
    pronouns: "they/them"
    color:
      gradient: ["#ff7a18", "#af002d"]
      angle: 135
    avatar: 3f5a...e1c4            # the hash of a picture in the media folder, or a path to a file to bring in
    banner: 9b1c...77d0
    background: 0d2e...4a91
    links:
      - text: "Battle.net: Ali#1234"
      - text: "Steam"
        url: "https://steamcommunity.com/id/ali"

| field          | required | notes |
|----------------|----------|-------|
| `display_name` | yes | up to 64 characters |
| `avatar`       | no | the profile picture |
| `banner`       | no | the banner across the top of the profile |
| `background`   | no | a background picture |
| `blurb`        | no | short bio, up to 160 characters |
| `about`        | no | long bio, up to 2000 characters, may span lines |
| `pronouns`     | no | up to 32 characters |
| `color`        | no | the colour of their name: `"#rgb"` / `"#rrggbb"`, or `gradient:` with 2 to 5 colours and an optional `angle` (0-360, default 180) |
| `links`        | no | up to 10 of `text` (up to 64 characters: a game tag, a handle) and, optionally, `url` (http or https, up to 512 bytes) |

Limits count characters, not bytes. Text must be valid UTF-8 without control characters (`about` may hold line
breaks). Unknown settings are errors.

**Pictures** are kept by content in `<data_dir>/media/<sha256>`, and a profile names them by that hash. The app uploads
one with `POST /api/v1/media/upload` (the image's bytes: png, jpg, gif or webp, judged by its first bytes, up to the
placeholder 25 MiB) and uses the hash it gets back. A hand-edited file may give a path instead; it is brought in when
the file is read, and the file is rewritten with the hash next time Puddle saves it.

**A file with a mistake in it** is left out at startup, with a warning naming the file and line; the other profiles
still load, so one bad edit cannot stop Puddle. A picture that is not in the media folder (or a path that cannot be
read) is left out of its profile the same way.

### The first start

Before profiles lived in the data folder they were YAML files named by the `profiles:` setting in `puddle.yaml`. The
first time Puddle starts without a `profiles` folder, it brings those in: each becomes a file of its own (the
`profiles:` setting is not read again). Then the first of them was shown to everyone the owner had chosen nothing for,
so it stays chosen for every stream that is not closed and every group the owner is in: nobody's view of the owner
changes. Anywhere new sees a profile only once one is chosen. Choices made for streams before
(`state/stream-profiles.tsv`) carry over as the direct places.

A new Puddle with nothing to bring in starts with one profile, `default`, named after the OS account, and shown to no one.

## Where each is shown

`<data_dir>/state/profile-places.tsv` maps a place to a profile id:

- `dm:<user id>`: what that person sees of the owner directly (the stream, the direct conversation, and anywhere else
  outside groups). Chosen with `POST /api/v1/streams/{user}/profile`, or with `"profile"` when sending a request
  (`/api/v1/streams/request`) or accepting one (`/api/v1/streams/{user}/accept`).
- `group:<group id>`: what the members of that group see. Chosen with `POST /api/v1/groups/{group}/profile`, or with
  `"profile"` when creating the group (`/api/v1/groups/create`) or joining it (`/api/v1/groups/{group}/accept`).
- `pond:<id>`: later.

`{"profile": null}` takes a choice back: the place sees nothing of the owner there any more, and is told so. Deleting a
profile does the same for every place that showed it. If a chosen profile's file goes missing, its places see nothing.

## How they travel

Over a stream, a peer receives a profile *without* its id: its text, its colour, its links, and its pictures as
`{hash, mime, size}`.

- **Directly:** the `dm` profile rides on a request for a stream and on its answer, if one was chosen for that
  person; if not, there is no `profile` in them at all. Once the stream is open, any change is pushed.
- **In groups:** a group's profile is never put in a request. It is pushed with `POST /stream/v1/profile`
  (`{"profile": ..., "group": "<group id>"}`) to each member the owner has a connection with: when the choice is made
  or the profile edited, when a connection to a member opens, and when someone joins.
- **Taking it back:** the same push with `"profile": null`.

A peer fetches a picture it does not hold from `GET /stream/v1/media?hash=`. It is served only if it is in a profile
shown to that peer in some place (directly, or in a group both are in), and only over an open stream. What arrives is
checked against what the profile said (the hash, the size, the kind of image from the file's own first bytes) and kept
in `<data_dir>/media/<hash>` only if it all matches.

## What others show us

Their Puddle's profiles are kept per place in `<data_dir>/state/people/<user id>.json`: one for `dm` and one for each
group they show us one in. Whichever comes last for a place replaces what we held; a `null` makes us forget it. Being
in a place together is the consent for that place's profile: a friend who is also in a group with someone keeps both,
and nobody is told (docs/discuss-later.md, 10).

**Nothing a peer sends is stored as sent, and nothing in it is refused for being wrong.** The profile is rebuilt from
the fields we understand:

- A field that is wrong is **left out**: a colour that is not `#rrggbb`, a picture reference with a malformed hash or a
  kind of image we do not take, a link that is not http or https (its text is kept), text that is not text.
- Text over its limit is **cut** to it.
- Fields we do not know are skipped without a word, so a newer Puddle can send more.
- Each thing left out or cut gets a short **note**, kept with that profile, written to the log, and shown in the owner
  API (`GET /api/v1/users/{user}/profiles`):

      color: "#12345g" is not a colour like "#3366ff"; left out

Only something that is not a JSON object at all is set aside.

The app shows each person as they are in the place on screen: directly in the friends list and direct conversations,
and as they are in the group in a group's members and messages. In the owner API, `User.profile` is what they show
us directly; a group's members and messages carry each person as they are in that group.

## The owner API

- `GET /api/v1/profiles`, `GET /api/v1/profiles/{id}`: the owner's profiles, each with `places` (where it is shown).
- `POST /api/v1/profiles/create`: `{"id", "display_name", ...}`; shown nowhere yet.
- `POST /api/v1/profiles/{id}/edit`: the fields to change (null clears one); every place showing it hears of it.
- `POST /api/v1/profiles/{id}/delete`: every place showing it is told to forget it.
- `POST /api/v1/media/upload`: a picture for a profile; answers `{hash, mime, size}`.
- `GET /api/v1/media?hash=`: any picture this Puddle holds, ours or a friend's.
- `GET /api/v1/users/{user}/profiles`: what someone shows us, per place, with the notes.

In code: `puddle::Profile` (`src/objects/profile.hpp`). `Profile::get(ctx, "work")`, `profile.edit(...)`,
`Profile::showAt(ctx, place, id)`, and for others `user.profile()` (direct) and `user.profileIn("group:<id>")`.

## Not built yet

The app's screens for making and editing profiles and choosing one when accepting a friend or joining a group (the
request dialog already has "Appear as"); reloading hand-edited files while running and resizing pictures
(docs/discuss-later.md, 1 and 2); and Ponds.
