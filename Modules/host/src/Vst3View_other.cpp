#include "Vst3View.h"

namespace symbolengine::vst3View
{

void sizeOwnView(void*, int, int) {}

// The awareness it would put back is Windows' alone.
ParentAwareness::ParentAwareness(void*)
{
    static_cast<void>(previous);
}

ParentAwareness::~ParentAwareness() = default;

}
