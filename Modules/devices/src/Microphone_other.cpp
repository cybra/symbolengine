#include "symbolengine/devices/Microphone.h"

namespace symbolengine
{

MicrophoneAccess microphoneAccess()
{
    return MicrophoneAccess::allowed;
}

void askForMicrophone(std::function<void(bool allowed)> answered)
{
    if (answered)
        answered(true);
}

}
