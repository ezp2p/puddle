# Builds and releases

`.github/workflows/build.yml` builds every platform, runs the tests, and publishes what can be downloaded. Nothing updates
itself yet: a newer build is downloaded again by hand.

## When

| What happens                      | What is published                                                                    |
|-----------------------------------|--------------------------------------------------------------------------------------|
| a pull request                    | nothing: built and tested only (the downloads are kept on the run for a few days)     |
| a push to `dev` (a PR merged)      | the **dev build**: the `dev-build` prerelease, replaced with this commit's downloads |
| a tag like `0.3.0`                | the release `0.3.0` (a tag with a `-`, such as `0.3.0-rc.1`, is a prerelease)         |
| a push to `main`                  | nothing: built and tested only                                                       |

The dev build is always at the same link, `https://github.com/ezp2p/puddle/releases/tag/dev-build`, which is the one to
give friends trying the newest. Its title and file names carry the commit (`Puddle-dev-51950ed-windows-x64.zip`). Its tag
is `dev-build`, not `dev`, so it never shares a name with the branch.

Releases are tagged by hand (`git tag 0.3.0 && git push origin 0.3.0`), from whichever commit is to be released.

## What

| File                                     | What it is                                                                            |
|------------------------------------------|---------------------------------------------------------------------------------------|
| `Puddle-<v>-windows-x64.zip`              | the desktop app: unzip, run `Puddle.exe` (`puddled.exe` beside it runs without a window) |
| `Puddle-<v>-macos-arm64.zip`, `-intel`    | the desktop app, `Puddle.app`; signed ad hoc only, so right-click → Open the first time   |
| `Puddle-<v>-linux-x86_64.AppImage`        | the desktop app as one file: `chmod +x`, run; `puddled` is inside too                   |
| `puddled-<v>-linux-amd64.tar.gz`, `arm64` | the Puddle without a window, for a server: OpenSSL built in, a systemd `--user` unit     |
| `SHA256SUMS`                             | the checksums of all of the above                                                     |

The container image of `puddled` is published by `docker.yml` on a release tag (`docs/docker.md`).

How each is made:

- **Linux desktop**: Ubuntu 22.04 (so it runs on any glibc from 2.35), Qt from Qt's own builds, libdatachannel built from
  source (Ubuntu has no package), then `scripts/package-appimage.sh` (linuxdeploy and its Qt plugin).
- **macOS**: Homebrew's Qt and libraries, libdatachannel built from source (Homebrew has none), `macdeployqt`, re-signed ad
  hoc.
- **Windows**: MSYS2's UCRT64 (GCC, Qt and the libraries as packages; libdatachannel built from source, as MSYS2 has
  none), then `scripts/package-windows.sh` (`windeployqt`, and every other DLL the programs need). Not tested on CI yet:
  the tests use POSIX headers (`PUDDLE_BUILD_TESTS=OFF`).
- **Linux server**: as before, `puddled` alone with OpenSSL linked in.

Every desktop build stops if CMake did not find libdatachannel, rather than publishing an app without calls.

## Not yet

- Android (an APK to install directly: roadmap 7.4) and iOS (an unsigned IPA, signed by whoever installs it).
- Signing and notarizing for Windows and macOS, so neither warns on first run.
- Updating itself.
