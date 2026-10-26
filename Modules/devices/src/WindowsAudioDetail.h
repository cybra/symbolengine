#pragma once

#include "symbolengine/devices/AudioDevice.h"

#include <algorithm>
#include <cctype>
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

}
