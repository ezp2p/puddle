// Before the window opens: find the owner's Puddle, starting puddled if it is not running, and hand its
// address, token and certificate to the client.

#include "client/client.hpp"
#include "platform/paths.hpp"
#include "platform/service.hpp"

namespace puddle::app {

bool connect(client::Client &client)
{
    auto dataDir = platform::dataDir();
    if (!platform::runningPuddle(dataDir)) {
        if (platform::needsInit(dataDir))
            return false; // the window shows first-run setup, which runs `puddled init`
        platform::startDetached({ .executable = platform::bundledPuddled() });
    }
    // Wait for /health, then attach the way any client would: address, owner token, pinned certificate.
    return client.attach(platform::listenUrl(dataDir), dataDir / "state/owner-token", dataDir / "tls/cert.pem");
}

} // namespace puddle::app
