#pragma once

#include <functional>

namespace symbolengine
{

/** Whether this program may record from the computer's inputs. On a Mac the user allows each program once, in System
    Settings under Privacy & Security, and Core Audio gives a program it hasn't allowed silence rather than an error;
    a program run from a terminal is allowed or not as the terminal is. Elsewhere it reads allowed, and the system
    says itself when an input is refused. */
enum class MicrophoneAccess
{
    allowed,
    denied,
    notAsked
};

MicrophoneAccess microphoneAccess();

/** Asks the user, with the system's own question, if they haven't been asked, and calls back with the answer on a
    thread of the system's; asked before, it calls back at once with the answer given then. A program asks when
    the user first chooses to record, where the question is expected; an input opened before it is asked asks then. */
void askForMicrophone(std::function<void(bool allowed)> answered);

}
