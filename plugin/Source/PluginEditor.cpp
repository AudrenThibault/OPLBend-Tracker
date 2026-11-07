// SPDX-License-Identifier: GPL-3.0-only
#include "PluginEditor.h"

namespace
{
    constexpr int noteBase = 14;                       // control ID of the lowest key (hal/fm1_input.h FM1_KEYMAP)
    constexpr int numKeys = 27;
    constexpr int designWidth = 904, designHeight = 614;   // the panel's layout; the window opens at 3/4 of it
    constexpr int songMaxBytes = 1 << 20;              // (native/hal/fm1_host_hal.h FM1_HOST_MAC_MAX)

    const juce::Colour background { 0xff17181c }, panel { 0xff26282e }, line { 0xff3a3d45 },
                       text { 0xffd8dade }, accent { 0xffff4f58 };

    // What each key does in D-PAD mode (compat/fm1_touches.h).
    juce::String dpadLabel (int note)
    {
        switch (note)
        {
            case 0:  return "L";
            case 1:  return "U";
            case 2:  return "D";
            case 4:  return "R";
            case 6: case 7: case 21: return "SEL";
            case 22: return "ST";
            case 23: case 24: return "B";
            case 25: return "A";
            default: return {};
        }
    }

    // A file's name as the tracker can show it and name its project with: plain ASCII, no folders.
    juce::String trackerName (const juce::File& file)
    {
        juce::String name;
        for (const auto c : file.getFileNameWithoutExtension().substring (0, 30))
            name += juce::CharacterFunctions::isLetterOrDigit (c) && c < 128 ? juce::String::charToString (c) : "_";
        return (name.isEmpty() ? "SONG" : name) + file.getFileExtension().substring (0, 5);
    }

    // The Mac folders PROJECT > LOAD and SAVE last went to, each its own, kept for every project
    // (~/Library/Application Support/OPLBend/OPLBend.settings).
    juce::PropertiesFile& macFolders()
    {
        static juce::PropertiesFile file ([]
        {
            juce::PropertiesFile::Options options;
            options.applicationName = "OPLBend";
            options.folderName = "OPLBend";
            options.filenameSuffix = ".settings";
            options.osxLibrarySubFolder = "Application Support";
            return options;
        }());
        return file;
    }

    juce::File lastFolder (const char* key)
    {
        const juce::File folder (macFolders().getValue (key));
        return folder.isDirectory() ? folder : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    }

    void rememberFolder (const char* key, const juce::File& folder)
    {
        macFolders().setValue (key, folder.getFullPathName());
        macFolders().saveIfNeeded();
    }
}

//==============================================================================
/** The FM-1's screen, three pixels a dot: on a Retina display, sharp at the size the window opens at. */
class OPLBendEditor::Screen : public juce::Component
{
public:
    void show (const juce::uint8* rgba)
    {
        {
            juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < Machine::lcdSize; ++y)
            {
                const auto* row = rgba + y * Machine::lcdSize * 4;
                for (int x = 0; x < Machine::lcdSize * dot; ++x)
                {
                    const auto* p = row + (x / dot) * 4;
                    pixels.setPixelColour (x, y * dot, juce::Colour (p[0], p[1], p[2]));
                }
                for (int k = 1; k < dot; ++k)
                    std::memcpy (pixels.getLinePointer (y * dot + k), pixels.getLinePointer (y * dot), (size_t) (pixels.width * pixels.pixelStride));
            }
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (image, getLocalBounds().reduced (1).toFloat());
        g.setColour (line);
        g.drawRect (getLocalBounds());
    }

private:
    static constexpr int dot = 3;
    juce::Image image { juce::Image::RGB, Machine::lcdSize * dot, Machine::lcdSize * dot, true };
};

//==============================================================================
// A knob as on the FM-1: a dark dial, a white mark.
void drawDial (juce::Graphics& g, juce::Rectangle<float> area, float angle)
{
    const auto dial = area.withSizeKeepingCentre (52.0f, 52.0f);
    g.setColour (panel);
    g.fillEllipse (dial);
    g.setColour (line);
    g.drawEllipse (dial, 2.0f);
    const auto centre = dial.getCentre();
    g.setColour (text);
    g.drawLine ({ centre.getPointOnCircumference (10.0f, angle), centre.getPointOnCircumference (22.0f, angle) }, 2.5f);
}

// MASTER, the plugin's parameter, drawn as the other knobs.
struct DialLook : juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider&) override
    {
        drawDial (g, juce::Rectangle<int> (x, y, width, height).toFloat(), startAngle + position * (endAngle - startAngle));
    }
};

//==============================================================================
/** A rotary encoder: drag or wheel gives detents. */
class OPLBendEditor::Knob : public juce::Component
{
public:
    Knob (Machine& e, int encoderIndex, const juce::String& name) : machine (e), index (encoderIndex), label (name) {}

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();
        g.setColour (text);
        g.setFont (11.0f);
        g.drawText (label, area.removeFromTop (16), juce::Justification::centred);   // (printed above, as on the FM-1)

        drawDial (g, area.toFloat(), juce::degreesToRadians (detents * 15.0f));
    }

    void mouseDown (const juce::MouseEvent&) override { dragged = 0; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto wanted = -e.getDistanceFromDragStartY() / 6;
        if (wanted != dragged)
        {
            turn (wanted - dragged);
            dragged = wanted;
        }
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        wheelRest += wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f) * 16.0f;
        const auto steps = (int) wheelRest;
        if (steps != 0)
        {
            turn (steps);
            wheelRest -= (float) steps;
        }
    }

private:
    void turn (int steps)
    {
        machine.encoder (index, steps);
        detents += steps;
        repaint();
    }

    Machine& machine;
    int index;
    juce::String label;
    int detents = 0, dragged = 0;
    float wheelRest = 0.0f;
};

//==============================================================================
class OPLBendEditor::Button : public juce::Component
{
public:
    Button (Machine& e, int controlId, const juce::String& name) : machine (e), id (controlId), label (name) {}

    void setLed (juce::uint8 level)
    {
        if (level != led)
        {
            led = level;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (held ? line : panel);
        g.fillRoundedRectangle (area, 5.0f);
        g.setColour (accent.withAlpha ((float) led / 255.0f));
        g.fillRoundedRectangle (area, 5.0f);
        g.setColour (line);
        g.drawRoundedRectangle (area, 5.0f, 1.0f);
        g.setColour (text);
        g.setFont (10.5f);
        g.drawFittedText (label, getLocalBounds().reduced (2), juce::Justification::centred, 2);
    }

    void mouseDown (const juce::MouseEvent&) override { press (true); }
    void mouseUp (const juce::MouseEvent&) override { press (false); }

private:
    void press (bool down)
    {
        held = down;
        machine.key (id, down);
        repaint();
    }

    Machine& machine;
    int id;
    juce::String label;
    juce::uint8 led = 0;
    bool held = false;
};

//==============================================================================
/** The 27 keys, F3 to G5. */
class OPLBendEditor::Keyboard : public juce::Component
{
public:
    explicit Keyboard (Machine& e) : machine (e) {}

    ~Keyboard() override
    {
        if (held >= 0)
            machine.key (noteBase + held, false);
    }

    void setLeds (const juce::uint8* levels)
    {
        if (std::memcmp (leds.data(), levels, leds.size()) != 0)
        {
            std::memcpy (leds.data(), levels, leds.size());
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        for (const auto black : { false, true })
        {
            for (int note = 0; note < numKeys; ++note)
            {
                if (isBlack (note) != black)
                    continue;
                // A pad, its light a bar down its middle, as on the FM-1; its D-PAD function at its foot.
                const auto area = keyArea (note).toFloat();
                g.setColour (note == held ? line : panel);
                g.fillRoundedRectangle (area, area.getWidth() * 0.45f);
                g.setColour (line);
                g.drawRoundedRectangle (area, area.getWidth() * 0.45f, 1.0f);
                const auto bar = area.withSizeKeepingCentre (4.0f, area.getHeight() * 0.36f).translated (0.0f, -area.getHeight() * 0.08f);
                g.setColour (line.brighter (0.2f));
                g.fillRoundedRectangle (bar, 2.0f);
                if (const auto level = leds[(size_t) note]; level > 0)
                {
                    g.setColour (accent.withAlpha ((float) level / 255.0f));
                    g.fillRoundedRectangle (bar, 2.0f);
                }
                g.setColour (text.withAlpha (0.7f));
                g.setFont (9.0f);
                g.drawText (dpadLabel (note), area.withTrimmedBottom (3.0f), juce::Justification::centredBottom);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        held = noteAt (e.getPosition());
        if (held >= 0)
            machine.key (noteBase + held, true);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (held >= 0)
            machine.key (noteBase + held, false);
        held = -1;
        repaint();
    }

private:
    static bool isBlack (int note)
    {
        const auto pitch = note % 12;                  // 0 = F
        return pitch == 1 || pitch == 3 || pitch == 5 || pitch == 8 || pitch == 10;
    }

    // Two rows of pads, as on the FM-1: the 16 white keys below, the 11 black ones above, each between two whites.
    juce::Rectangle<int> keyArea (int note) const
    {
        constexpr int numWhite = 16;
        auto white = 0;
        for (int n = 0; n < note; ++n)
            if (! isBlack (n))
                ++white;

        // (low pads, as on the FM-1: the upper row, then the lower row, a little taller)
        const auto pitch = (float) getWidth() / (float) numWhite;
        const auto padWidth = pitch * 0.78f, upperHeight = (float) getHeight() * 0.44f, lowerTop = upperHeight + 8.0f;
        if (! isBlack (note))
            return juce::Rectangle<float> ((float) white * pitch + (pitch - padWidth) / 2.0f, lowerTop, padWidth, (float) getHeight() - lowerTop).toNearestInt();
        return juce::Rectangle<float> ((float) white * pitch - padWidth / 2.0f, 0.0f, padWidth, upperHeight).toNearestInt();
    }

    int noteAt (juce::Point<int> p) const
    {
        for (const auto black : { true, false })
            for (int note = 0; note < numKeys; ++note)
                if (isBlack (note) == black && keyArea (note).contains (p))
                    return note;
        return -1;
    }

    Machine& machine;
    std::array<juce::uint8, numKeys> leds {};
    int held = -1;
};

//==============================================================================
/** The dark plate the twelve buttons sit in. */
class OPLBendEditor::Plate : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xff111215));
        g.fillRoundedRectangle (area, 10.0f);
        g.setColour (line);
        g.drawRoundedRectangle (area, 10.0f, 1.0f);
    }
};

//==============================================================================
/** A choice parameter as segments: the OPL3 the firmware plays with (the FM-1's own, or Nuked OPL3), and who
    leads the tempo (Live, or the tracker through Link). */
class OPLBendEditor::ChoiceSwitch : public juce::Component
{
public:
    ChoiceSwitch (juce::AudioParameterChoice& p, const juce::String& title, const juce::StringArray& segments, int titleWidth, int segmentWidth)
        : parameter (p), attachment (p, [this] (float) { repaint(); }), label (title), names (segments),
          labelWidth (titleWidth), width (segmentWidth)
    {
        attachment.sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();
        g.setColour (text);
        g.setFont (11.0f);
        g.drawText (label, area.removeFromLeft (labelWidth), juce::Justification::centredLeft);
        for (int i = 0; i < names.size(); ++i)
        {
            const auto segment = segmentArea (i).toFloat().reduced (1.0f);
            const auto chosen = parameter.getIndex() == i;
            g.setColour (chosen ? accent : panel);
            g.fillRoundedRectangle (segment, 4.0f);
            g.setColour (line);
            g.drawRoundedRectangle (segment, 4.0f, 1.0f);
            g.setColour (chosen ? background : text);
            g.drawText (names[i], segment, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < names.size(); ++i)
            if (segmentArea (i).contains (e.getPosition()) && parameter.getIndex() != i)
                attachment.setValueAsCompleteGesture ((float) i);
    }

private:
    juce::Rectangle<int> segmentArea (int i) const { return { labelWidth + i * width, 0, width, getHeight() }; }

    juce::AudioParameterChoice& parameter;
    juce::ParameterAttachment attachment;
    const juce::String label;
    const juce::StringArray names;
    const int labelWidth, width;
};

//==============================================================================
OPLBendEditor::OPLBendEditor (OPLBendProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), machine (p.machine)
{
    frame.allocate ((size_t) Machine::lcdSize * Machine::lcdSize * 4, true);
    addAndMakeVisible (content);

    screen = std::make_unique<Screen>();
    content.addAndMakeVisible (*screen);

    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, accent);
    content.addAndMakeVisible (statusLabel);

    masterKnob.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    masterKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    dialLook = std::make_unique<DialLook>();
    masterKnob.setLookAndFeel (dialLook.get());
    content.addAndMakeVisible (masterKnob);
    masterAttachment = std::make_unique<juce::SliderParameterAttachment> (*processor.master, masterKnob);

    masterLabel.setText ("MASTER", juce::dontSendNotification);
    masterLabel.setFont (juce::FontOptions (11.0f));
    masterLabel.setJustificationType (juce::Justification::centred);
    masterLabel.setColour (juce::Label::textColourId, text);
    content.addAndMakeVisible (masterLabel);

    // In fm1n_encoder's order (native/fm1_native.h).
    int index = 0;
    for (const auto* name : { "SELECT", "PRESETS", "ALGORITHM", "KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4" })
        content.addAndMakeVisible (knobs.add (new Knob (machine, index++, name)));

    buttonPlate = std::make_unique<Plate>();
    content.addAndMakeVisible (*buttonPlate);          // (under the buttons, as on the FM-1)
    int id = 0;
    for (const auto* name : { "OCT-", "OCT+", "FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY\nSTOP", "REC" })
        content.addAndMakeVisible (buttons.add (new Button (machine, id++, name)));

    keyboard = std::make_unique<Keyboard> (machine);
    content.addAndMakeVisible (*keyboard);

    engineSwitch = std::make_unique<ChoiceSwitch> (*processor.engine, "OPL3", juce::StringArray { "FM-1", "NUKED" }, 36, 56);
    content.addAndMakeVisible (*engineSwitch);
    tempoSwitch = std::make_unique<ChoiceSwitch> (*processor.tempoMaster, "TEMPO", juce::StringArray { "LIVE", "TRACKER" }, 44, 64);
    content.addAndMakeVisible (*tempoSwitch);
    layOut();

    macKeys = std::make_unique<MacKeys> (*this, machine);
    setWantsKeyboardFocus (true);
    const auto width = processor.editorWidth > 0 ? processor.editorWidth : designWidth * 3 / 4;   // (before the limits resize it)
    setResizable (true, true);
    setResizeLimits (designWidth / 2, designHeight / 2, designWidth * 3 / 2, designHeight * 3 / 2);
    getConstrainer()->setFixedAspectRatio ((double) designWidth / designHeight);
    setSize (width, juce::roundToInt (width * (double) designHeight / designWidth));
    startTimerHz (30);
}

OPLBendEditor::~OPLBendEditor()
{
    stopTimer();
    masterKnob.setLookAndFeel (nullptr);
}

void OPLBendEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
}

void OPLBendEditor::resized()
{
    content.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));
    processor.editorWidth = getWidth();
}

// As on the FM-1: the knobs two by two on the left, OCT- and OCT+ under them; the screen; KNOB1 to KNOB4 in a row
// on the right, the twelve buttons under them; the two rows of keys below. The plugin's own switches at the foot.
void OPLBendEditor::layOut()
{
    content.setBounds (0, 0, designWidth, designHeight);
    auto area = content.getLocalBounds().reduced (16);
    auto foot = area.removeFromBottom (26);
    area.removeFromBottom (10);
    auto top = area.removeFromTop (398);
    area.removeFromTop (18);
    keyboard->setBounds (area);

    engineSwitch->setBounds (foot.removeFromLeft (36 + 2 * 56));
    foot.removeFromLeft (16);
    tempoSwitch->setBounds (foot.removeFromLeft (44 + 2 * 64));
    statusLabel.setBounds (foot);

    // Left: MASTER, SELECT / PRESETS, ALGORITHM / OCT-, OCT+.
    auto left = top.removeFromLeft (150);
    const auto columnWidth = left.getWidth() / 2;
    auto row = [&] (int height) { return left.removeFromTop (height); };
    left.removeFromTop (40);
    auto first = row (100), second = row (100);
    left.removeFromTop (30);
    auto octaves = row (34);
    auto masterCell = first.removeFromLeft (columnWidth).reduced (4);
    masterLabel.setBounds (masterCell.removeFromTop (16));
    masterKnob.setBounds (masterCell);
    knobs[0]->setBounds (first.reduced (4));                       // SELECT
    knobs[1]->setBounds (second.removeFromLeft (columnWidth).reduced (4));   // PRESETS
    knobs[2]->setBounds (second.reduced (4));                      // ALGORITHM
    // (small buttons, a little wider than high, as on the FM-1)
    buttons[0]->setBounds (octaves.removeFromLeft (columnWidth).withSizeKeepingCentre (54, 30));   // OCT-
    buttons[1]->setBounds (octaves.withSizeKeepingCentre (54, 30));                                 // OCT+

    top.removeFromLeft (16);
    // (the one liberty taken with the FM-1: a screen much bigger than its own, to be read on a computer)
    screen->setBounds (top.removeFromLeft (398).withHeight (398).expanded (1));
    top.removeFromLeft (16);

    // Right: KNOB1 to KNOB4, then FX SEL ENV LFO EDIT GLO / HOME SAVE ARP SEQ PLAY REC.
    top.removeFromTop (40);
    auto knobRow = top.removeFromTop (100);
    const auto knobWidth = knobRow.getWidth() / 4;
    for (int i = 0; i < 4; ++i)
        knobs[3 + i]->setBounds (knobRow.removeFromLeft (knobWidth).reduced (4));
    top.removeFromTop (30);
    const auto plate = top.removeFromTop (96);
    buttonPlate->setBounds (plate);
    const auto grid = plate.reduced (8);
    const auto cellWidth = grid.getWidth() / 6, cellHeight = grid.getHeight() / 2;
    for (int i = 2; i < buttons.size(); ++i)
        buttons[i]->setBounds (juce::Rectangle<int> (grid.getX() + ((i - 2) % 6) * cellWidth, grid.getY() + ((i - 2) / 6) * cellHeight,
                                                     cellWidth, cellHeight).withSizeKeepingCentre (44, 36));
}

void OPLBendEditor::timerCallback()
{
    if (machine.copyScreen (frame, leds.data(), seenVersion))
    {
        screen->show (frame);
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setLed (leds[(size_t) i]);
        keyboard->setLeds (leds.data() + noteBase);
    }

    auto wanted = machine.getError();
    if (wanted.isEmpty() && machine.getUnderruns() > 0)
        wanted = "Dropouts: " + juce::String (machine.getUnderruns());
    if (wanted.isEmpty() && machine.getLinkPeers() > 0)
        wanted = "LINK";
    else if (wanted.isEmpty() && processor.tempoMaster->getIndex() == (int) Machine::TempoMaster::tracker)
        wanted = "LINK off in Live";
    statusLabel.setText (wanted, juce::dontSendNotification);


    if (choosing)
        return;
    juce::String name;
    juce::MemoryBlock contents;
    if (machine.takeLoadRequest())
        chooseSong();
    else if (machine.takeSavedSong (name, contents))
        saveSong (name, contents);
}

// PROJECT > SAVE: the song, saved in the project, is offered to the Mac too (Cancel: in the project only).
void OPLBendEditor::saveSong (const juce::String& name, const juce::MemoryBlock& contents)
{
    choosing = true;
    chooser = std::make_unique<juce::FileChooser> ("Save the song", lastFolder ("saveFolder").getChildFile (name),
                                                   "*" + juce::File (name).getFileExtension());
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, contents] (const juce::FileChooser& c)
    {
        choosing = false;
        const auto file = c.getResult();
        if (file != juce::File() && file.replaceWithData (contents.getData(), contents.getSize()))
            rememberFolder ("saveFolder", file.getParentDirectory());
        grabKeyboardFocus();
    });
}

void OPLBendEditor::chooseSong()
{
    choosing = true;
    chooser = std::make_unique<juce::FileChooser> ("Load a song", lastFolder ("loadFolder"), "*.obt;*.xt;*.xmm;*.a2m");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& c)
    {
        choosing = false;
        const auto file = c.getResult();
        juce::MemoryBlock contents;
        if (file.getSize() > 0 && file.getSize() <= songMaxBytes && file.loadFileAsData (contents))
        {
            rememberFolder ("loadFolder", file.getParentDirectory());
            machine.loadSong (trackerName (file), contents);
        }
        else
        {
            machine.loadSong ({}, {});
        }
        grabKeyboardFocus();
    });
}

//==============================================================================
void OPLBendEditor::focusLost (FocusChangeType)
{
    macKeys->releaseAll();
}
