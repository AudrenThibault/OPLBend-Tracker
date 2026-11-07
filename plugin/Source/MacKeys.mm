// SPDX-License-Identifier: GPL-3.0-only
#include "MacKeys.h"
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

namespace
{
    constexpr int gloButton = 7;

    // What a key does: a D-PAD function, or a panel button (GLO).
    struct Action { int dpad = -1, button = -1; };

    Action actionFor (unsigned short keyCode)
    {
        // The two keys right of the left Shift: on an ISO keyboard the extra key (kVK_ISO_Section) comes first.
        static const bool iso = KBGetLayoutType (LMGetKbdType()) == kKeyboardISO;
        switch (keyCode)
        {
            case kVK_LeftArrow:     return { FM1N_DPAD_LEFT };
            case kVK_RightArrow:    return { FM1N_DPAD_RIGHT };
            case kVK_UpArrow:       return { FM1N_DPAD_UP };
            case kVK_DownArrow:     return { FM1N_DPAD_DOWN };
            case kVK_ISO_Section:   return iso ? Action { FM1N_DPAD_B } : Action {};
            case kVK_ANSI_Z:        return { iso ? FM1N_DPAD_A : FM1N_DPAD_B };
            case kVK_ANSI_X:        return iso ? Action {} : Action { FM1N_DPAD_A };
            case kVK_Return:
            case kVK_ANSI_KeypadEnter:
            case kVK_Space:         return { FM1N_DPAD_START };
            case kVK_Delete:
            case kVK_ForwardDelete: return { FM1N_DPAD_DELETE };
            case kVK_ANSI_G:        return { -1, gloButton };
            default:                return {};
        }
    }
}

struct MacKeys::Impl
{
    Impl (juce::Component& c, Machine& m) : owner (c), machine (m)
    {
        monitor = [NSEvent addLocalMonitorForEventsMatchingMask: NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged
                                                       handler: ^NSEvent* (NSEvent* e) { return handle (e) ? nil : e; }];
    }

    ~Impl()
    {
        [NSEvent removeMonitor: monitor];
        releaseAll();
    }

    // true: the event was ours (the host does not get it)
    bool handle (NSEvent* e)
    {
        auto* peer = owner.getPeer();
        auto* view = peer != nullptr ? (NSView*) peer->getNativeHandle() : nil;
        if (view == nil || [e window] != [view window])
            return false;

        if ([e type] == NSEventTypeFlagsChanged)
        {
            set (FM1N_DPAD_SELECT, -1, ([e modifierFlags] & NSEventModifierFlagShift) != 0);   // (either Shift)
            return false;                               // (modifiers are the host's too)
        }
        if (([e modifierFlags] & NSEventModifierFlagCommand) != 0)
            return false;

        const auto action = actionFor ([e keyCode]);
        if (action.dpad < 0 && action.button < 0)
            return false;
        if (! [e isARepeat])
            set (action.dpad, action.button, [e type] == NSEventTypeKeyDown);
        return true;
    }

    void set (int dpad, int button, bool down)
    {
        if (dpad >= 0 && dpadHeld[(size_t) dpad] != down)
        {
            dpadHeld[(size_t) dpad] = down;
            machine.dpad (dpad, down);
        }
        if (button >= 0 && buttonHeld[(size_t) button] != down)
        {
            buttonHeld[(size_t) button] = down;
            machine.key (button, down);
        }
    }

    void releaseAll()
    {
        for (int i = 0; i < (int) dpadHeld.size(); ++i)
            set (i, -1, false);
        for (int i = 0; i < (int) buttonHeld.size(); ++i)
            set (-1, i, false);
    }

    juce::Component& owner;
    Machine& machine;
    id monitor = nil;
    std::array<bool, 12> dpadHeld {};
    std::array<bool, Machine::numControls> buttonHeld {};
};

MacKeys::MacKeys (juce::Component& owner, Machine& machine) : impl (std::make_unique<Impl> (owner, machine)) {}
MacKeys::~MacKeys() = default;
void MacKeys::releaseAll() { impl->releaseAll(); }
