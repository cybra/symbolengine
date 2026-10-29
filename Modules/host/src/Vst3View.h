#pragma once

namespace symbolengine::vst3View
{

/** Sizes a VST3 plugin's own view in the host's: the first view the plugin put in it, at its top left, and none of the
    others it may keep there, as views some plugins draw off screen in. A plugin that can't be resized may have stopped
    the host's view sizing its views, so the host sizes this one itself. On a Mac; elsewhere it does nothing. */
void sizeOwnView(void* parent, int width, int height);

/** While it lives, the calling thread's DPI awareness is that of the view a plugin's editor is in, as every call that
    may size or move the plugin's windows is made with, since the plugin's windows are measured by it: on Windows;
    elsewhere it does nothing. */
class ParentAwareness
{
public:
    explicit ParentAwareness(void* parent);
    ~ParentAwareness();

    ParentAwareness(const ParentAwareness&) = delete;
    ParentAwareness& operator=(const ParentAwareness&) = delete;

private:
    void* previous = nullptr;
};

}
