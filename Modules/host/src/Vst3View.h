#pragma once

namespace symbolengine::vst3View
{

/** Sizes a VST3 plugin's own view in the host's: the first view the plugin put in it, at its top left, and none of the
    others it may keep there, as views some plugins draw off screen in. A plugin that can't be resized may have stopped
    the host's view sizing its views, so the host sizes this one itself. On a Mac; elsewhere it does nothing. */
void sizeOwnView(void* parent, int width, int height);

}
