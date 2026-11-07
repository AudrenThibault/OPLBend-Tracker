// SPDX-License-Identifier: GPL-3.0-only
// The Mac's keyboard as the FM-1's D-PAD, by key position, whatever the layout (AZERTY, QWERTY, QWERTZ...):
//   arrows      the cross                    either Shift   SELECT
//   the two keys right of the left Shift     B, then A (ISO: < and W on AZERTY; ANSI and JIS: Z and X)
//   Return, Space  START (PLAY)              Backspace, Delete   delete (as A+B)
//   G           GLO
// They work in every GLO mode. Keys with Command go to the host.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Machine.h"

class MacKeys
{
public:
    MacKeys (juce::Component& owner, Machine& machine);
    ~MacKeys();

    void releaseAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
