#pragma once

// The running Puddle as objects see it: the engine's parts, handed to every object so it can read and act.
// Built once in service/main.cpp. Objects reach data only through core (chats(), blocks(), ...), never through
// storage, so they don't know or care how anything is kept.

#include "core/engine.hpp"

namespace puddle {

class Puddle {
public:
    explicit Puddle(core::Engine &engine) : engine_(engine) {}

    core::Config &config() { return engine_.config(); }
    core::Identity &identity() { return engine_.identity(); }
    core::Chats &chats() { return engine_.chats(); }
    core::People &people() { return engine_.people(); }
    core::Conversations &conversations() { return engine_.conversations(); }
    core::Blocks &blocks() { return engine_.blocks(); }
    core::Streams &streams() { return engine_.streams(); }
    core::Groups &groups() { return engine_.groups(); }
    core::Outbox &outbox() { return engine_.outbox(); }
    core::Media &media() { return engine_.media(); }
    int64_t now() { return engine_.now(); }

private:
    core::Engine &engine_;
};

} // namespace puddle
