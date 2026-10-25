#include "Systems.h"

namespace symbolengine::devices::detail
{

// Linux's systems come after the first release.
std::vector<std::shared_ptr<AudioSystem>> systemsOfThisComputer()
{
    return {};
}

}
