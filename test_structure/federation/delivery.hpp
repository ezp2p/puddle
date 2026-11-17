#pragma once

// Delivery and DeliveryResult: how events are carried to a peer, and its answer (model/federation/federation.yaml).
// A protocol message, owned by the federation API and versioned with it.

#include "events/event.hpp"

#include <optional>
#include <string>
#include <vector>

namespace puddle::federation {

struct Delivery {
    std::string chat;
    std::vector<events::SignedEvent> events; // at most 100

    std::string toJson() const;
    static std::optional<Delivery> parse(const std::string &json);
};

class DeliveryResult {
public:
    void accept(const std::string &id);
    void duplicate(const std::string &id);
    void reject(const std::string &id, const std::string &reason);
    std::string toJson() const;
    static std::optional<DeliveryResult> parse(const std::string &json);

private:
    std::vector<std::string> accepted_, duplicate_;
    std::vector<std::pair<std::string, std::string>> rejected_;
};

} // namespace puddle::federation
