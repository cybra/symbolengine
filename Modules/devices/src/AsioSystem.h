#pragma once

#include <symbolengine/devices/AudioSystem.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct IASIO;

// ASIO behind the devices module's system layer, on Windows, built with the developer's copy of Steinberg's SDK.
namespace symbolengine::asio::detail
{

/** A driver as it is installed, read without loading it; one that can't be loaded is listed with why. */
struct Installed
{
    std::string id{};
    std::string name{};
    std::string problem{};
};

/** Where the ASIO layer finds drivers and makes them: the registry and COM, or a test's own. */
struct Host
{
    std::function<std::vector<Installed>()> drivers{};

    /** Makes a driver, on the thread that makes every call to it, or gives nothing with the problem said. */
    std::function<IASIO*(const std::string& id, std::string& problem)> make{};

    /** How long a call to a driver may take before the driver is taken as not answering. */
    std::chrono::milliseconds callTimeout{4000};
};

/** The drivers the registry lists and COM makes. */
Host installedDrivers();

std::shared_ptr<AudioSystem> makeSystem(Host host = installedDrivers());

}
