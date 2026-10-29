#pragma once

void InputInit();
void InputShutdown();
void InputAdvanceFrame();
// A second fixed tick in one frame (FixedTickRate): this frame's presses and releases, and its mouse movement, already
// seen by its first tick -- not seen again. No devices read, the cursor not recentred.
void InputAdvanceTick();
void InputPostUpdate();
