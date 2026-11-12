// puddled: the headless Puddle. Builds the layers bottom to top and hands the server to platform to run.
//
//   puddled          run (as a service, or in the foreground from a terminal or the app)
//   puddled init     first run: make the keys and print the recovery phrase on stdout, for the app to show

#include "api/api.hpp"
#include "core/engine.hpp"
#include "federation/federation.hpp"
#include "http/server.hpp"
#include "objects/puddle.hpp"
#include "platform/paths.hpp"
#include "platform/service.hpp"
#include "storage/storage.hpp"

#include <string_view>

int main(int argc, char **argv)
{
    using namespace puddle;

    auto config = core::Config::load(platform::configFile());
    if (argc > 1 && std::string_view(argv[1]) == "init")
        return core::initIdentity(config);

    auto storage = storage::open({.driver = config.storage(), .dataDir = config.dataDir()}); // files, unless puddle.yaml says otherwise
    core::Engine engine(config, *storage);  // identity, chats, blocks, streams, groups, outbox: each deposits with storage
    Puddle puddle(engine);        // what objects see

    http::Server server(config);  // one listener, one router, both APIs on it
    api::addRoutes(server.router(), puddle);          // /api/v1/...    objects, for the owner's apps
    federation::addRoutes(server.router(), engine);   // /stream/v1/... events, for other Puddles
    federation::Sender sender(engine);                // delivers what the outbox holds

    return platform::runService(
        [&] { sender.start(); return server.run(); },
        [&] { server.stop(); sender.stop(); });
}
