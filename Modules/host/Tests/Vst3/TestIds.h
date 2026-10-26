#pragma once

// The class IDs of the test module's plugins, as four 32-bit words for the SDK and as the 32 hexadecimal digits the
// host knows a VST3 plugin by.
#define SYMBOLENGINE_TEST_ID(n) 0x53796D62, 0x54657374, 0x56535433, (n)
// The SDK's own macro, given the four words as one argument, which they become only once expanded.
#define SYMBOLENGINE_INLINE_UID(...) INLINE_UID(__VA_ARGS__)
#define SYMBOLENGINE_TEST_HEX(n) "53796D625465737456535433000000" n

namespace testIds
{
constexpr const char* gain = SYMBOLENGINE_TEST_HEX("01");
constexpr const char* gainController = SYMBOLENGINE_TEST_HEX("02");
constexpr const char* arranged = SYMBOLENGINE_TEST_HEX("03");
constexpr const char* mono = SYMBOLENGINE_TEST_HEX("04");
constexpr const char* noOutput = SYMBOLENGINE_TEST_HEX("05");
constexpr const char* latency = SYMBOLENGINE_TEST_HEX("06");
constexpr const char* throwing = SYMBOLENGINE_TEST_HEX("08");
}
