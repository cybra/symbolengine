#pragma once

#include "symbolengine/devices/AudioDevice.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

// Windows Audio's reading of what an endpoint is, apart from Windows itself so the tests check it on any computer.
namespace symbolengine::windowsAudio::detail
{

struct EndpointKind
{
    bool handsFree = false;
    AudioTransport transport = AudioTransport::unknown;
};

/** What an endpoint is, from the bus that enumerated its device, as its properties name it, and from its name. A
    Bluetooth headset's hands-free endpoints are BTHHFENUM's whatever they are called: Windows 11 names AirPods'
    microphone "Headset (AirPods Pro)", with no word of its profile, where Windows 10 put "Hands-Free" in the name. Its
    stereo output is BTHENUM's, and a USB device's endpoints are those of an enumerator naming USB, the class driver's
    or its maker's own. */
inline EndpointKind kindOf(std::string enumerator, const std::string& name)
{
    std::transform(enumerator.begin(), enumerator.end(), enumerator.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    EndpointKind kind;
    kind.handsFree = enumerator == "BTHHFENUM" || name.find("Hands-Free") != std::string::npos;
    if (kind.handsFree || enumerator.rfind("BTH", 0) == 0)
        kind.transport = AudioTransport::bluetooth;
    else if (enumerator.find("USB") != std::string::npos)
        kind.transport = AudioTransport::usb;
    return kind;
}

/** Whether a stream ends for what a call for its buffer answered. A few say the buffer isn't to be had just now, as a stream
    reset is under way, so the next period may do better, which a run of them says it won't; anything else is the end. */
class ErrorRun
{
public:
    static constexpr int mostInARow = 16;

    bool ends(std::int32_t result) noexcept
    {
        if (result >= 0)
        {
            inARow = 0;
            return false;
        }
        const auto code = static_cast<std::uint32_t>(result);
        const bool recoverable = code == 0x88890018u       // AUDCLNT_E_BUFFER_ERROR
                                 || code == 0x8889000Bu    // AUDCLNT_E_BUFFER_OPERATION_PENDING
                                 || code == 0x88890006u;   // AUDCLNT_E_BUFFER_TOO_LARGE
        return !recoverable || ++inARow > mostInARow;
    }

private:
    int inARow = 0;
};

/** DisconnectReasonFormatChanged, as a number. */
constexpr int disconnectReasonFormatChanged = 2;

/** A session the system ended because its format changed, in Sound settings, runs again as it now is; for any other
    reason it is lost. */
inline bool reopensAfterDisconnect(int reason) noexcept
{
    return reason == disconnectReasonFormatChanged;
}
}
