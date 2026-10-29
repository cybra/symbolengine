#pragma once

#include "pluginterfaces/base/funknown.h"

// The editor test plugin, made by the test module's factory.
Steinberg::FUnknown* makeTestEditor(void*);

// What the editor plugin's view does in the system's own views, an NSView on a Mac. Main thread only.
namespace testEditor
{

// Whether views of this kind are put in the host's on this system.
bool supported(const char* platformType);

// The view's own native view, made at its size and put in the host's parent, with an off-screen view of its own
// beside it, as some plugins keep, and the parent's sizes watched from then on; null where it can't be.
void* attach(void* parent, int width, int height);

// Sizes the view's own native view.
void resize(void* view, int width, int height);

// Takes the view's own native views out of the parent and lets go of them.
void detach(void* view);

// Whether the parent is still whole and holds the view: in its window, neither of them hidden.
bool stillWhole(void* parent, void* view);

// Whether the parent is in its window, laid out as a plugin's view expects: not flipped, and clipping to its bounds.
bool laidOut(void* parent);

// Whether the parent has the size given.
bool sized(void* parent, int width, int height);

// The size of the view's own native view.
void ownSize(void* view, int& width, int& height);

// Whether the host has moved or resized the off-screen view.
bool offscreenMoved(void* view);

// How many times the parent was made a point larger each way and then put back.
int nudges(void* view);

}
