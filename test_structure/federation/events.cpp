// Stream API: POST /stream/v1/events. A peer delivers signed events for one chat (model/federation/federation.yaml,
// Delivery and DeliveryResult).
//
// This speaks in events, not objects. It checks each event, stores its exact bytes, and answers which were
// accepted. The owner hears about it through api/updates.cpp, which listens to core::Chats::onAppend, builds the Message object
// and sends it to the app. Federation never includes anything from api/ or objects/.

#include "http/router.hpp"
#include "core/chats.hpp"
#include "core/streams.hpp"
#include "events/chat.hpp"
#include "events/group.hpp"
#include "federation/delivery.hpp"

namespace puddle::federation {

namespace {

http::Response deliver(http::Call &call)
{
    auto delivery = Delivery::parse(call.body());
    if (!delivery)
        return http::Response::badRequest();

    auto key = call.engine().streams().signingKey(call.sender()); // the router already checked the request's signature
    DeliveryResult result;
    for (const auto &signed_ : delivery->events) {
        if (!signed_.verify(key)) {
            result.reject(signed_.id(), "bad_signature");
            continue;
        }
        std::string why;
        // Accept only events this sender may send into this chat, by the rules in events/.
        if (!events::acceptable(call.engine(), call.sender(), delivery->chat, signed_, &why)) {
            result.reject(signed_.id(), why);
            continue;
        }
        if (call.engine().chats().append(delivery->chat, signed_.id(), signed_.ts(), signed_.toJson()))
            result.accept(signed_.id());
        else
            result.duplicate(signed_.id());
    }
    return http::Response::ok(result.toJson());
}

} // namespace

void addEventRoutes(http::Router &r)
{
    r.post("/stream/v1/events", http::Auth::Peer, deliver, 256 * 1024);
}

} // namespace puddle::federation
