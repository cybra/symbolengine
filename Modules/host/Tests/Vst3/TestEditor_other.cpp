// The editor test plugin's view where no view of its kind is put in a host's yet.

#include "TestEditor.h"

namespace testEditor
{

bool supported(const char*)
{
    return false;
}

void* attach(void*, int, int)
{
    return nullptr;
}

void resize(void*, int, int) {}

void detach(void*) {}

bool stillWhole(void*, void*)
{
    return false;
}

bool laidOut(void*)
{
    return false;
}

bool sized(void*, int, int)
{
    return false;
}

void ownSize(void*, int& width, int& height)
{
    width = height = 0;
}

bool awareAsParent(void*)
{
    return true;
}

bool takesDrops(void*)
{
    return true;
}

void holdLater(void*, int) {}

int keysSent()
{
    return 0;
}

bool offscreenMoved(void*)
{
    return false;
}

int nudges(void*)
{
    return 0;
}

}
