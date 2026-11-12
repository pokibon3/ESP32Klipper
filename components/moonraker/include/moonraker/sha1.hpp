// SHA-1 and Base64 for the WebSocket handshake.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <string>

namespace kl {

void sha1(const void *data, size_t len, uint8_t out[20]);
std::string base64_encode(const uint8_t *data, size_t len);
// Sec-WebSocket-Accept for a Sec-WebSocket-Key
std::string websocket_accept(const std::string &key);

} // namespace kl
