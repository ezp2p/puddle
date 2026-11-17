#pragma once

// Profile, Color and MediaRef as JSON (model/wire/). The profile someone shows you is exactly what your Puddle stores
// and what your app receives, so the owner API and the federation API both send it with these functions, and it
// can't differ between them.

#include "core/profile.hpp"
#include "core/json.hpp"

#include <optional>

namespace puddle::wire {

Value profile(const core::Profile &p);
Value color(const core::Color &c);
Value mediaRef(const core::MediaRef &m);

// Strict: unknown fields, over-long text and bad colours are refused, with the reason in `why`.
std::optional<core::Profile> readProfile(const Value &v, std::string *why = nullptr);

} // namespace puddle::wire
