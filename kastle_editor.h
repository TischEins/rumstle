//------------------------------------------------------------------------
// kastle_editor.h — custom VSTGUI views for KastlePort.
//
// The plugin's chrome (knobs, labels, toggles, background) lives in
// kastleport.uidesc and is edited graphically: build with live editing
// enabled, right-click in the running plugin, rearrange, save.
//
// Only the patch bay stays code: it is not a stock control but a canvas of
// jacks with drag-to-connect cables, plus one attenuverter knob per
// destination slot. It registers itself with VSTGUI's view factory, so it
// shows up in the UI editor's view list and can be placed and resized like
// any built-in view.
//------------------------------------------------------------------------
#pragma once

#include <vector>

#include "base/source/fobject.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include "vstgui/lib/vstguifwd.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/cbitmap.h"
#include "vstgui/lib/cvstguitimer.h"
#include "vstgui/lib/controls/ccontrol.h"
#include "vstgui/lib/controls/icontrollistener.h"
#include "vstgui/lib/cviewcontainer.h"

#include "kastle_vst3.h"

namespace Steinberg {
namespace Vst {

//------------------------------------------------------------------------
// One jack on the patch bay: either a fixed modulation SOURCE (LFO, LFO
// square, Rungler — engine outputs, not parameters) or a modulation
// DESTINATION slot, which IS backed by a real ModSrc param, so dropping a
// cable on it changes that param.
struct PatchJack
{
    bool           isSource;
    int32          sourceId;    // valid when isSource
    ParamID        destParamId; // valid when !isSource: the SrcA/SrcB param
    VSTGUI::CPoint pos;         // bay-local
    const char*    label;
};

class KastlePatchBayView;

// The faceplate graphics, loaded from the bundle through the .uidesc. Any of
// them may be null (file missing); that element is then simply not drawn.
struct KastleArt
{
    VSTGUI::SharedPointer<VSTGUI::CBitmap> plate, knob, knobBody, trim, trimBody, switchOn, switchOff, ledOn, ledOff;
};

//------------------------------------------------------------------------
// Dependent on the patch params, so host-side changes reach the bay.
class KastleBayListener : public FObject
{
public:
    explicit KastleBayListener (KastlePatchBayView* v) : view (v) {}

    void PLUGIN_API update (FUnknown* changedUnknown, int32 message) SMTG_OVERRIDE;
    void detach () { view = nullptr; }

    OBJ_METHODS (KastleBayListener, FObject)

private:
    KastlePatchBayView* view;
};

//------------------------------------------------------------------------
class KastlePatchBayView : public VSTGUI::CViewContainer, public VSTGUI::IControlListener
{
public:
    KastlePatchBayView (const VSTGUI::CRect& size, const KastleArt& art);

    // CViewContainer
    void drawBackgroundRect (VSTGUI::CDrawContext* context, const VSTGUI::CRect& updateRect) override;
    bool attached (VSTGUI::CView* parent) override;
    bool removed (VSTGUI::CView* parent) override;

    VSTGUI::CMouseEventResult onMouseDown (VSTGUI::CPoint& where, const VSTGUI::CButtonState& buttons) override;

    // IControlListener — the knobs report here
    void valueChanged (VSTGUI::CControl* control) override;

    // called by KastleBayListener
    void onParamChangedFromHost (ParamID tag, ParamValue value);

private:
    VSTGUI::CPoint toLocal (const VSTGUI::CPoint& framePoint) const;
    int findJackAt (const VSTGUI::CPoint& p) const;
    int32 sourceOf (const PatchJack& destJack) const;
    void edit (ParamID tag, ParamValue value);
    VSTGUI::CColor colorForSource (int32 sourceId) const;
    template <typename Fn> void forEachWatchedParam (Fn fn);

    EditController* controller = nullptr;
    std::vector<PatchJack> jacks;
    // tag2 != 0: a depth knob that drives both sockets of its input
    struct TaggedControl { ParamID tag; ParamID tag2; VSTGUI::CControl* control; };
    std::vector<TaggedControl> knobs;   // owned by this container
    KastleBayListener* listener = nullptr;

    int32 armedSource = kModNone;   // output clicked, waiting for an input

    // members, not statics: a static would outlive VSTGUI's own shutdown
    KastleArt art;
    VSTGUI::SharedPointer<VSTGUI::CVSTGUITimer> ledTimer;   // polls the LFO for the rate LED
    bool lfoLedShown = false;
    VSTGUI::SharedPointer<VSTGUI::CFontDesc> labelFont = VSTGUI::makeOwned<VSTGUI::CFontDesc> ("Arial", 14, VSTGUI::kBoldFace);
    VSTGUI::SharedPointer<VSTGUI::CFontDesc> hintFont  = VSTGUI::makeOwned<VSTGUI::CFontDesc> ("Arial", 13);
};

//------------------------------------------------------------------------
// Must be called once before the first editor is created, so the patch bay
// is known to the UI description factory. Explicit rather than a file-scope
// static, which the linker is free to drop from a static library.
void registerKastleViews ();

//------------------------------------------------------------------------
} // namespace Vst
} // namespace Steinberg
