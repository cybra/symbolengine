#include "symbolengine/devices/Microphone.h"

#import <AVFoundation/AVFoundation.h>

#include <memory>

namespace symbolengine
{

MicrophoneAccess microphoneAccess()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized:
            return MicrophoneAccess::allowed;
        case AVAuthorizationStatusNotDetermined:
            return MicrophoneAccess::notAsked;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:
            break;
    }
    return MicrophoneAccess::denied;
}

void askForMicrophone(std::function<void(bool allowed)> answered)
{
    if (const auto now = microphoneAccess(); now != MicrophoneAccess::notAsked)
    {
        if (answered)
            answered(now == MicrophoneAccess::allowed);
        return;
    }

    auto held = std::make_shared<std::function<void(bool)>>(std::move(answered));
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
                               if (*held)
                                   (*held)(granted == YES);
                             }];
}

}
