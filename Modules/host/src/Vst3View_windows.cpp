#include "Vst3View.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace symbolengine::vst3View
{

void sizeOwnView(void*, int, int) {}

ParentAwareness::ParentAwareness(void* parent)
{
    if (parent != nullptr)
        previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(static_cast<HWND>(parent)));
}

ParentAwareness::~ParentAwareness()
{
    if (previous != nullptr)
        SetThreadDpiAwarenessContext(static_cast<DPI_AWARENESS_CONTEXT>(previous));
}

}
