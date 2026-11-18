# Backups, recovering, and moving a Puddle

A Puddle can be written to one file, a **backup**, and brought back from it: on the same computer after a disk dies, or on another
one. Moving a Puddle from this computer to a server is the same thing, with the app set to use it there afterwards.

## What a backup holds

Everything the Puddle keeps: its keys, friends, chats, files, phones (paired devices), and the settings that are yours: the
profiles, `theme.yaml`, and `puddle.yaml` for reference. Not the TLS certificate (it only carries bytes, so a new one is made;
see the protocol's spec/09) and not the owner token (a new one is made where it is restored).

It is **sealed to the Puddle's own encryption key**, which the recovery phrase makes (`docs/identity.md`). So only the phrase opens
it, and the file can be kept anywhere: a USB stick, cloud storage, an email to yourself. After a key change, a backup made before
it still opens, with the phrase from before; the file says which key generation sealed it.

The format is in `src/core/backup.h`: HPKE (the protocol's suite) to the encryption key, then the files in 64 KiB records under a
key exported from it, the last record marked so a file cut short is refused, as is one with any byte changed. Restoring puts
nothing in place until the whole file has opened, and never over an identity that is already there.

## Making one

- **The app:** Settings, This computer, **Back up…**. It is written while the Puddle runs.
- **A server:** `puddled --backup FILE` (in Docker, `docker exec puddle puddled --backup /data/backup.puddle`). It can run
  beside the running Puddle; a file being written at that moment may be caught part way, so stop the Puddle first for one that is
  exactly one moment.

## Bringing it back

- **The app:** on a fresh install, **Recover my Puddle**, then the phrase and the file. (Settings, This computer, Change… gets
  back to that question.)
- **A server:** `puddled --restore --from FILE`, in a terminal: it asks for the phrase. A Puddle waiting to be set up
  (`--wait`, as the container does) starts by itself once it is restored.

The computer's own `puddle.yaml` stays as it is (its address, port and folders are its own); the backup's is put beside it as
`puddle.yaml.from-backup`. If `advertise` is different from before, friends are told the new address when the Puddle starts
(`docs/streams.md`, "Telling friends about a new address").

Without a backup, the phrase and a copy of the chain (`identity/chain.jsonl`, not secret) bring back the identity alone, so
friends still know you, but not the chats: the same **Recover my Puddle**, with the chain as the file, or
`puddled --restore --from chain.jsonl`.

**Only one Puddle may run with an identity.** Restore one only when the old one is gone or stopped for good.

## Moving to a server

For a Puddle that started on this computer and should run on a server (always on, so friends reach you while this computer is off):

1. Start Puddle on the server (`docs/docker.md`), with `PUDDLE_ADVERTISE` set to where friends will reach it, and **don't** set
   it up: no `puddled --init`.
2. In the app: Settings, This computer, **Move to a server…**. Give the address the server is reached at, and where to save the
   file. The Puddle here stops, this app is added to it as a device (already accepted), and it is written to the file. The data
   folder here is put aside as `<data folder>.handed-over`, not deleted, so nothing starts it again by mistake.
3. Copy the file to the server and restore it there, which asks for the phrase:

   ```sh
   docker cp moved.puddle puddle:/data/
   docker exec -it puddle puddled --restore --from /data/moved.puddle
   ```

   or with Kubernetes, `kubectl cp moved.puddle <pod>:/data/` and `kubectl exec -it <pod> -- puddled --restore --from /data/moved.puddle`.
   Delete the copy on the server afterwards.
4. Restart the app. It now uses your Puddle on the server, as a phone does, and your phones carry on as before.

To go the other way (a Puddle on a server, the app connected to it), make a backup on the server, stop that Puddle for good, and
choose **Recover my Puddle** in the app (Settings, This device, Disconnect, then Back).
