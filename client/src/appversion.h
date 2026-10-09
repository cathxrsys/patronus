#pragma once

#ifndef PATRONUS_VERSION
#define PATRONUS_VERSION "1.0.0"
#endif

#ifndef PATRONUS_PROTOCOL_VERSION
#define PATRONUS_PROTOCOL_VERSION "3"
#endif

namespace appversion {

inline constexpr const char* kApplicationVersion = PATRONUS_VERSION;
inline constexpr const char* kProtocolVersion = PATRONUS_PROTOCOL_VERSION;

}  // namespace appversion