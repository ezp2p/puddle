# puddle

Easiest self hosted chat application conceivable.

## Building

Two programs, built from one CMake project:

- `puddled` — headless service (C, OpenSSL)
- `Puddle` — the desktop app (C core + Qt 6 Quick/QML). It embeds the same server
  as `puddled`, so desktop users only need this one program.

### Requirements

CMake 3.21+, a C11/C++17 compiler, OpenSSL 3.0+, libyaml, and (for the GUI) Qt 6 with Quick, Quick Controls 2 and Svg (the icons are SVG files).

macOS (Homebrew):

    brew install cmake openssl@3 libyaml qt

Debian/Ubuntu:

    sudo apt install build-essential cmake libssl-dev libyaml-dev qt6-base-dev qt6-declarative-dev qt6-svg-dev qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-templates qml6-module-qtquick-window qml6-module-qt-labs-platform

Fedora:

    sudo dnf install gcc gcc-c++ cmake openssl-devel libyaml-devel qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel

### Build and test

    scripts/build.sh              # configure, build, test (--clean, --no-gui, --release)

Or by hand:

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build

Headless host with no Qt: add `-DPUDDLE_BUILD_GUI=OFF` to the first command.
If CMake picks the wrong OpenSSL (macOS ships LibreSSL), pass
`-DOPENSSL_ROOT_DIR=/path/to/openssl@3`. If Qt isn't found, add its prefix with
`-DCMAKE_PREFIX_PATH=/path/to/qt`.

Outputs: `build/puddled` and `build/Puddle` (`build/Puddle.app` on macOS).

### Run

    ./build/puddled                  # or ./build/Puddle

On first run `~/.config/puddle/puddle.yaml` is created (Windows: `%APPDATA%\puddle\`)
with `address: 127.0.0.1` and `port: 8443`, a starter `profiles.yaml` next to it, and a
self-signed `cert.pem` / `key.pem` in the same folder. Edit the config and restart to change the
listener (`address: "::"` listens on all IPv4 and IPv6 interfaces; put IPv6 addresses in quotes). Set `advertise: host:port`
to the address friends should use if it isn't the one you listen on; it is the address you give to friends.

Your profiles (display name, picture, blurb, colors, ...) live in `profiles.yaml`, and the
`profiles:` setting names the file or files. See [docs/profiles.md](docs/profiles.md).
Your identity is made from a **recovery phrase**: eight groups of four characters, such as
`7QK4-M2XB-9TDW-HC5R-ZP3E-A8VN-4YJF-X2K7`. It is shown once, when the identity is made, and is not stored
anywhere. **Write it down** (on paper, or in a password manager): it is the only way back to your
identity if this computer is lost, and anyone who has it can take your identity over.

- Run `puddled` from a terminal the first time and it shows the phrase, waits for you to write it
  down, then carries on. Started as a service with no terminal it refuses, so the phrase can't land in a
  log; run `puddled --init` once from a terminal first.
- The desktop app does the same on first launch with a short setup screen: it shows the phrase in
  groups, with a copy button, and only starts the Puddle once you tick "I have written my recovery
  phrase down".
- If you quit before confirming, that identity is thrown away on the next start and a fresh one is made,
  so you can never be left with an identity whose phrase you didn't write down.

**If this computer is lost**, put Puddle on another one and run
`puddled --restore --chain chain.jsonl` from a terminal. It asks for your **latest** recovery phrase (the one
from your most recent key change, or from the start if you never changed them) and needs a copy of your chain,
which is `identity/chain.jsonl` in your data folder. The chain is not secret, so it is worth keeping a copy
somewhere. Your id comes back unchanged. An older phrase is refused.

**Changing your keys** is optional and never happens by itself. Stop the Puddle and run `puddled --roll`
from a terminal: it asks for your *current* phrase (not echoed), gives you a **new** phrase, and only
once you confirm the new one is written down does it change anything. From then on the new phrase is the
one that counts and the old one stops working. Your user id stays the same. Quitting before you confirm
changes nothing.

Chats are stored under the data directory (`data_dir` in the config; default
`~/Library/Application Support/Puddle` on macOS, `~/.local/share/puddle` on Linux).
Only one Puddle can use a data directory at a time.

Nothing on the port is public. Even `/health` needs your owner token, which is made on first run in
`<data_dir>/state/owner-token` (mode 0600). Without it the answer is a plain `404`, the same as for
any path that doesn't exist; the Puddle's log says why.

    curl --cacert ~/.config/puddle/cert.pem \
         -H "Authorization: Bearer $(cat ~/Library/Application\ Support/Puddle/state/owner-token)" \
         https://localhost:8443/health

(On Linux the token is at `~/.local/share/puddle/state/owner-token`.)

The app opens its window on launch. Closing the window hides it to the system tray
(menu bar on macOS, where the Dock icon also goes away until the window is reopened) and the server keeps running; use the tray icon to reopen it or
choose **Quit Puddle** to stop. On desktops without a tray (e.g. stock GNOME) closing
the window quits instead, so the app can't become unreachable.

### Run as a systemd user service

    install -Dm755 build/puddled ~/.local/bin/puddled
    install -Dm644 dist/puddled.service ~/.config/systemd/user/puddled.service
    systemctl --user daemon-reload
    systemctl --user enable --now puddled
    journalctl --user -u puddled -f

To keep it running when you're logged out: `loginctl enable-linger $USER`.

The server code is POSIX-only for now; Windows builds are not yet supported.

## Releases

Pushing a tag like `0.0.0` (no `v`) builds Linux amd64/arm64 (`puddled`) and
macOS arm64/amd64 (`Puddle.app` only) and publishes them, with `SHA256SUMS`,
to the GitHub release page. A tag containing `-` (e.g. `0.1.0-rc1`) is marked a pre-release.

    git tag 0.0.0 && git push origin 0.0.0

Pushes to `main` and pull requests run the same builds without publishing.
The macOS app is not notarized; after downloading, clear the quarantine flag
with `xattr -dr com.apple.quarantine Puddle.app`.
