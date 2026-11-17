// The outgoing half of the stream API: drains core's outbox and delivers to peers.
//
// objects/ queues a signed event with core::Outbox (Conversation::send does), and this sends it as a Delivery
// over http/'s client, retrying until the peer answers. So objects never need to know the federation format.

#include "core/outbox.hpp"
#include "core/streams.hpp"
#include "federation/delivery.hpp"
#include "http/client.hpp"

namespace puddle::federation {

// Sender::run(): for each peer with something waiting, POST /stream/v1/events with up to 100 events for one chat,
// sign the request, read the DeliveryResult, and mark accepted and duplicate ones done in the outbox. Back off on
// failure; hold everything for a peer whose certificate changed until the owner trusts it.

} // namespace puddle::federation
