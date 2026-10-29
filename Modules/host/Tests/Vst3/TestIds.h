#pragma once

// The class IDs of the test module's plugins, as four 32-bit words for the SDK and as the 32 hexadecimal digits the
// host knows a VST3 plugin by.
#define SYMBOLENGINE_TEST_ID(n) 0x53796D62, 0x54657374, 0x56535433, (n)
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
constexpr const char* skipsEmpty = SYMBOLENGINE_TEST_HEX("09");
constexpr const char* many = SYMBOLENGINE_TEST_HEX("0A");
constexpr const char* timed = SYMBOLENGINE_TEST_HEX("0C");

// The gain plugins' parameters, listed in this order, which isn't their IDs' order.
constexpr unsigned gainParameter = 100, hiddenParameter = 7, meterParameter = 3000000, bypassParameter = 1,
                   programParameter = 2;

// More of the gain plugins' parameters, set by the host, at which their controller misbehaves as plugins do: an edit of
// the gain in a gesture of its own, the value it shows left as it was, with an edit of a parameter it never listed; a
// burst of saying its state changed; and the value given edited back to the host, as it was and rounded to a float.
constexpr unsigned editParameter = 200, dirtyParameter = 201, echoParameter = 300, roundedEchoParameter = 301,
                   unlistedParameter = 999;

// Not a parameter: the gain plugins' text for it says what their controller has, as
// "gain=0.500000 meter=0.000000 echo=0 rounded=0", where echo and rounded count the values it was given for those; the
// timed plugin's says what it was last given, as
// "position=0 playing=0 tempo=120.0 signature=4/4 music=0.000 bar=0.000 stops=0", stops counting its processing stopped.
constexpr unsigned queryParameter = 4000;

// The many plugin's parameters, 1 to this.
constexpr unsigned manyParameters = 1000;
}
