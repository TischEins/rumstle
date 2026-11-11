//------------------------------------------------------------------------
// kastle_editor.cpp — see kastle_editor.h.
//------------------------------------------------------------------------
#include "kastle_editor.h"

#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/cgraphicspath.h"
#include "vstgui/lib/controls/cknob.h"
#include "vstgui/lib/cfont.h"

#include "vstgui/uidescription/iviewcreator.h"
#include "vstgui/uidescription/uiviewfactory.h"
#include "vstgui/uidescription/detail/uiviewcreatorattributes.h"

#include "public.sdk/source/vst/vstguieditor.h"

#include <cmath>

namespace Steinberg {
namespace Vst {

using namespace VSTGUI;

namespace {

// ---- layout, panel-local coordinates (canvas 760x650). The plate bitmap
// carries the header (logo, licence, artwork); everything below is placed on
// it: MOD knobs down the left edge, OSC/LFO knobs down the right, WAVESHAPE
// in the middle, patch bay below it with inputs left and outputs right.
constexpr CCoord kPanelW = 760, kPanelH = 650;
const CRect kSwitchRect (328, 5, 408, 64);         // OFF | ON buttons, one bitmap per state
const CPoint kPowerLed (426, 35);
const CPoint kLfoLed (722, 446);                  // beside LFO RATE, blinks with the LFO
constexpr CCoord kLedR = 12;
constexpr CCoord kSlotGap = 40;
constexpr CCoord kJackRadius = 11;
constexpr CCoord kBigR = 34, kSmallR = 20;       // half the drawn bitmap; the knob body is ~70% of it
constexpr double kPi = 3.14159265358979323846;

struct DestLayout { CCoord x; CCoord y; const char* label; };
constexpr DestLayout kDestLayout[kNumDests] = {
    { 215, 305, "PITCH MOD" },
    { 215, 367, "TIMBRE MOD" },
    { 360, 305, "WAVESHAPE" },
    { 215, 491, "MODE" },
    { 215, 429, "RATE MOD" },
    { 360, 367, "LFO RST" },
    { 360, 429, "BIT IN" },
};

struct SourceLayout { int32 id; CCoord x; CCoord y; const char* label; };
constexpr SourceLayout kSourceLayout[] = {
    { kModLFO,       532, 305, "LFO TRI" },
    { kModLfoSquare, 532, 367, "LFO SQUARE" },
    { kModRungler,   532, 429, "STEPPED" },
};

// SrcA id per destination; DepthA/SrcB/DepthB follow at +1/+2/+3
constexpr ParamID kDestSrcABase[kNumDests] = {
    kPitchModSrcAId, kTimbreModSrcAId, kWaveshapeModSrcAId,
    kModeModSrcAId, kLfoRateModSrcAId, kLfoRstModSrcAId, kBitInModSrcAId
};

// One knob per hardware knob. A depth knob drives both sockets (A and B) of
// its input, like the hardware's single MOD knob behind several pin holes.
// The four inputs without a hardware knob get a small trimmer by the socket.
struct KnobDef { ParamID tag; ParamID tag2; CCoord x, y, r; const char* label; };
constexpr KnobDef kKnobs[] = {
    { kPitchId,     0, 672, 170, kBigR, "OSC PITCH" },
    { kRateId,      0, 672, 308, kBigR, "OSC TIMBRE" },
    { kLfoRateId,   0, 672, 446, kBigR, "LFO RATE" },
    { kWaveshapeId, 0, 380, 150, kBigR, "WAVESHAPE" },
    { kPitchModDepthAId,     kPitchModDepthBId,      86, 170, kBigR, "PITCH MOD" },
    { kTimbreModDepthAId,    kTimbreModDepthBId,     86, 308, kBigR, "TIMBRE MOD" },
    { kLfoRateModDepthAId,   kLfoRateModDepthBId,    86, 446, kBigR, "RATE MOD" },
    { kWaveshapeModDepthAId, kWaveshapeModDepthBId, 446, 305, kSmallR, nullptr },
    { kModeModDepthAId,      kModeModDepthBId,      301, 491, kSmallR, nullptr },
    { kLfoRstModDepthAId,    kLfoRstModDepthBId,    446, 367, kSmallR, nullptr },
    { kBitInModDepthAId,     kBitInModDepthBId,     446, 429, kSmallR, nullptr },
};

constexpr double kModSrcSteps = KastleModSource::kNumModSources - 1; // 3

// measured from the plate bitmap; shown where the bitmap is missing and
// behind socket labels. Lettering is #F3DEC6.
const CColor kPlate (51, 48, 49, 255);
const CColor kInk (243, 222, 198, 255);

void drawCentered (CDrawContext* ctx, CBitmap* bmp, const CPoint& c)
{
    if (!bmp) return;
    const CCoord w = bmp->getWidth (), h = bmp->getHeight ();
    bmp->draw (ctx, CRect (c.x - w * 0.5, c.y - h * 0.5, c.x + w * 0.5, c.y + h * 0.5));
}

bool isModSrcParam (ParamID tag)
{
    for (int32 d = 0; d < kNumDests; ++d)
        if (tag == kDestSrcABase[d] || tag == (ParamID)(kDestSrcABase[d] + 2))
            return true;
    return false;
}

CPoint onCircle (const CPoint& c, double angle, CCoord radius)
{
    return CPoint (c.x + std::cos (angle) * radius, c.y + std::sin (angle) * radius);
}

//------------------------------------------------------------------------
// Two-layer knob: a fixed body (shadow and lighting stay where they are) and
// on top of it the pointer, drawn from pre-rotated frames (make_art.ps1: 1
// degree per frame, laid out in a grid; rotating the bitmap at draw time did
// not render under Direct2D). kKnobFrames / kKnobCols must match the script.
//
// Mouse: drag up or right to raise, down or left to lower. A full sweep is
// kDragRange pixels, with Shift held ten times that for fine adjustment.
// Double-click returns to the centre. This replaces VSTGUI's default, where
// the pointer jumps to the angle of the cursor - too coarse on a small knob.
constexpr int kKnobFrames = 271, kKnobCols = 16;
constexpr CCoord kDragRange = 300, kFineFactor = 10;

class KastleKnob : public CKnob
{
public:
    KastleKnob (const CRect& r, IControlListener* l, int32_t tag, CBitmap* bodyBmp, CBitmap* frames)
    : CKnob (r, l, tag, nullptr, nullptr), body (bodyBmp), art (frames)
    {
        setWheelInc (0.01f);
    }

    void draw (CDrawContext* ctx) override
    {
        if (body)
            body->draw (ctx, getViewSize ());
        if (art)
        {
            const CCoord size = art->getWidth () / kKnobCols;
            const int frame = (int)(getValueNormalized () * (kKnobFrames - 1) + 0.5f);
            art->draw (ctx, getViewSize (), CPoint ((frame % kKnobCols) * size, (frame / kKnobCols) * size));
        }
        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (!buttons.isLeftButton ())
            return kMouseEventNotHandled;
        beginEdit ();
        if (buttons.isDoubleClick ())
        {
            apply (getDefaultValue ());
            endEdit ();
            return kMouseDownEventHandledButDontNeedMovedOrUpEvents;
        }
        last = where;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState& buttons) override
    {
        if (!isEditing () || !buttons.isLeftButton ())
            return kMouseEventNotHandled;
        // incremental, so pressing or releasing Shift mid-drag does not jump
        const CCoord delta = (last.y - where.y) + (where.x - last.x);
        last = where;
        const CCoord range = (buttons & kShift) ? kDragRange * kFineFactor : kDragRange;
        apply (getValueNormalized () + (float)(delta / range));
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint&, const CButtonState&) override
    {
        if (isEditing ())
            endEdit ();
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        if (isEditing ())
            endEdit ();
        return kMouseEventHandled;
    }

private:
    void apply (float v)
    {
        v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        if (v == getValueNormalized ())
            return;
        setValueNormalized (v);
        valueChanged ();
        invalid ();
    }

    SharedPointer<CBitmap> body, art;
    CPoint last;
};

void strokeCable (CDrawContext* ctx, const CPoint& from, const CPoint& to, const CColor& col)
{
    ctx->setFrameColor (col);
    ctx->setLineWidth (4.0);
    if (CGraphicsPath* path = ctx->createGraphicsPath ())
    {
        // both control points hang below the sockets, so the cable sags
        path->beginSubpath (from);
        path->addBezierCurve (CPoint (from.x, from.y + 70), CPoint (to.x, to.y + 70), to);
        ctx->drawGraphicsPath (path, CDrawContext::kPathStroked);
        path->forget ();
    }
}

} // anonymous namespace

//------------------------------------------------------------------------
void PLUGIN_API KastleBayListener::update (FUnknown* changedUnknown, int32 message)
{
    if (!view || message != IDependent::kChanged)
        return;
    if (auto* param = FCast<Parameter> (changedUnknown))
        view->onParamChangedFromHost (param->getInfo ().id, param->getNormalized ());
}

//------------------------------------------------------------------------
KastlePatchBayView::KastlePatchBayView (const CRect& size, const KastleArt& a)
: CViewContainer (size), art (a)
{
    setBackgroundColor (kPlate);

    for (const auto& s : kSourceLayout)
        jacks.push_back ({ true, s.id, 0, CPoint (s.x, s.y), s.label });

    for (int32 d = 0; d < kNumDests; ++d)
        for (int32 s = 0; s < kNumSlots; ++s)
            jacks.push_back ({ false, 0, (ParamID)(kDestSrcABase[d] + s*2),
                               CPoint (kDestLayout[d].x + s * kSlotGap, kDestLayout[d].y),
                               s == 0 ? kDestLayout[d].label : nullptr });

    for (const auto& k : kKnobs)
    {
        auto* knob = new KastleKnob (CRect (k.x - k.r, k.y - k.r, k.x + k.r, k.y + k.r), this, k.tag,
                                     k.r == kBigR ? art.knobBody : art.trimBody,
                                     k.r == kBigR ? art.knob : art.trim);
        knob->setDefaultValue (0.5f);
        knob->setValueNormalized (0.5f);
        addView (knob);
        knobs.push_back ({ k.tag, k.tag2, knob });
    }
}

//------------------------------------------------------------------------
template <typename Fn>
void KastlePatchBayView::forEachWatchedParam (Fn fn)
{
    if (!controller) return;
    for (int32 d = 0; d < kNumDests; ++d)
        for (int32 i = 0; i < kNumSlots*2; ++i)
            if (auto* p = controller->getParameterObject ((ParamID)(kDestSrcABase[d] + i)))
                fn (p);
    for (ParamID id : { (ParamID)kPitchId, (ParamID)kRateId, (ParamID)kWaveshapeId, (ParamID)kLfoRateId,
                        (ParamID)kPowerId })
        if (auto* p = controller->getParameterObject (id))
            fn (p);
}

//------------------------------------------------------------------------
bool KastlePatchBayView::attached (CView* parent)
{
    if (!CViewContainer::attached (parent))
        return false;

    // The view is built by the UI description factory, which knows nothing
    // about the controller, so it is resolved from the editor on attach.
    if (!controller)
    {
        if (auto* f = getFrame ())
            if (auto* editor = dynamic_cast<VSTGUIEditor*> (f->getEditor ()))
                controller = editor->getController ();
    }
    if (!controller)
        return true;

    listener = new KastleBayListener (this);
    forEachWatchedParam ([this] (Parameter* p) { p->addDependent (listener); });

    for (const auto& k : knobs)
        k.control->setValueNormalized ((float)controller->getParamNormalized (k.tag));

    // The rate LED follows the LFO's half cycle. Processor and controller are
    // one object here, so the flag is read directly.
    // ponytail: polled at ~30 Hz, so above roughly 15 Hz the LED flickers
    // instead of resolving every cycle - as a real LED does to the eye.
    ledTimer = makeOwned<CVSTGUITimer> ([this] (CVSTGUITimer*) {
        auto* proc = dynamic_cast<KastleProcessor*> (controller);
        const bool on = proc && proc->lfoLedOn.load (std::memory_order_relaxed);
        if (on != lfoLedShown)
        {
            lfoLedShown = on;
            invalidRect (CRect (kLfoLed.x - kLedR, kLfoLed.y - kLedR, kLfoLed.x + kLedR, kLfoLed.y + kLedR));
        }
    }, 33);
    invalid ();
    return true;
}

//------------------------------------------------------------------------
bool KastlePatchBayView::removed (CView* parent)
{
    ledTimer = nullptr;
    if (listener)
    {
        forEachWatchedParam ([this] (Parameter* p) { p->removeDependent (listener); });
        listener->detach ();
        listener->release ();
        listener = nullptr;
    }
    return CViewContainer::removed (parent);
}

//------------------------------------------------------------------------
void KastlePatchBayView::onParamChangedFromHost (ParamID tag, ParamValue value)
{
    for (const auto& k : knobs)
    {
        if (k.tag != tag)
            continue;
        // skip the echo of an edit made in this view
        if (std::fabs (k.control->getValueNormalized () - (float)value) > 1e-6f)
        {
            k.control->setValueNormalized ((float)value);
            k.control->invalid ();
        }
        break;
    }
    if (isModSrcParam (tag) || tag == kPowerId)
        invalid (); // cables, switch and LED are drawn straight from the params
}

//------------------------------------------------------------------------
void KastlePatchBayView::edit (ParamID tag, ParamValue value)
{
    if (!controller) return;
    controller->beginEdit (tag);
    controller->setParamNormalized (tag, value);
    controller->performEdit (tag, controller->getParamNormalized (tag));
    controller->endEdit (tag);
}

//------------------------------------------------------------------------
void KastlePatchBayView::valueChanged (CControl* control)
{
    for (const auto& k : knobs)
    {
        if (k.control != control)
            continue;
        edit (k.tag, control->getValueNormalized ());
        if (k.tag2)
            edit (k.tag2, control->getValueNormalized ());
        break;
    }
}

//------------------------------------------------------------------------
CColor KastlePatchBayView::colorForSource (int32 sourceId) const
{
    if (sourceId == kModLFO)       return CColor (255, 140, 40, 255);  // orange
    if (sourceId == kModRungler)   return CColor (60, 210, 190, 255);  // teal
    if (sourceId == kModLfoSquare) return CColor (245, 220, 70, 255);  // yellow
    return CColor (120, 120, 120, 255);
}

//------------------------------------------------------------------------
CPoint KastlePatchBayView::toLocal (const CPoint& framePoint) const
{
    CPoint o = getViewSize ().getTopLeft ();
    return CPoint (framePoint.x - o.x, framePoint.y - o.y);
}

//------------------------------------------------------------------------
int KastlePatchBayView::findJackAt (const CPoint& p) const
{
    for (size_t i = 0; i < jacks.size (); ++i)
    {
        CPoint d = p; d.x -= jacks[i].pos.x; d.y -= jacks[i].pos.y;
        if (std::sqrt (d.x*d.x + d.y*d.y) <= kJackRadius + 5)
            return (int)i;
    }
    return -1;
}

//------------------------------------------------------------------------
int32 KastlePatchBayView::sourceOf (const PatchJack& destJack) const
{
    ParamValue norm = controller ? controller->getParamNormalized (destJack.destParamId) : 0.0;
    return (int32)(norm * kModSrcSteps + 0.5);
}

//------------------------------------------------------------------------
// CViewContainer::drawRect already translated the context to this view
// origin, so everything here is drawn in panel-local coordinates.
void KastlePatchBayView::drawBackgroundRect (CDrawContext* ctx, const CRect& updateRect)
{
    CViewContainer::drawBackgroundRect (ctx, updateRect);
    ctx->setDrawMode (kAntiAliasing);

    if (art.plate)
        art.plate->draw (ctx, CRect (0, 0, kPanelW, kPanelH));

    // power buttons and LED; second LED blinks with the LFO
    const bool on = controller && controller->getParamNormalized (kPowerId) > 0.5;
    if (CBitmap* sw = on ? art.switchOn : art.switchOff)
        sw->draw (ctx, kSwitchRect);
    drawCentered (ctx, on ? art.ledOn : art.ledOff, kPowerLed);
    drawCentered (ctx, lfoLedShown ? art.ledOn : art.ledOff, kLfoLed);

    ctx->setFontColor (kInk);
    ctx->setFrameColor (kInk);
    ctx->setFont (labelFont);
    for (const auto& k : kKnobs)
        if (k.label)
            ctx->drawString (k.label, CRect (k.x - 70, k.y + 36, k.x + 70, k.y + 54), kCenterText);

    // patch bay frames: inputs left, outputs right
    ctx->setLineWidth (1.5);
    ctx->drawRect (CRect (170, 262, 468, 522), kDrawStroked);
    ctx->drawRect (CRect (476, 262, 588, 460), kDrawStroked);
    ctx->drawString ("IN", CRect (170, 242, 300, 260), kLeftText);
    ctx->drawString ("OUT", CRect (476, 242, 588, 260), kCenterText);

    ctx->setFont (hintFont);
    ctx->drawString ("Klick auf einen Ausgang (farbig), dann auf einen Eingang = Kabel stecken   |   "
                     "Klick auf belegten Eingang = Kabel ziehen",
                     CRect (20, 556, kPanelW - 20, 576), kCenterText);
    ctx->setFont (labelFont);

    // cables, read straight from the mod-source params
    for (const auto& j : jacks)
    {
        if (j.isSource) continue;
        const int32 srcId = sourceOf (j);
        for (const auto& sj : jacks)
            if (sj.isSource && sj.sourceId == srcId)
                strokeCable (ctx, sj.pos, j.pos, colorForSource (srcId));
    }

    // sockets on top of the cables
    for (const auto& j : jacks)
    {
        CRect jr (j.pos.x - kJackRadius, j.pos.y - kJackRadius,
                  j.pos.x + kJackRadius, j.pos.y + kJackRadius);
        const int32 plugged = j.isSource ? j.sourceId : sourceOf (j);

        if (j.isSource ? armedSource == j.sourceId : armedSource != kModNone)
        {
            // armed output, and every input it could go to
            CRect halo (jr); halo.extend (5, 5);
            ctx->setFrameColor (colorForSource (armedSource));
            ctx->setLineWidth (j.isSource ? 3.0 : 1.5);
            ctx->drawEllipse (halo, kDrawStroked);
        }

        ctx->setFillColor (CColor (0, 0, 0, 255));
        ctx->setFrameColor (j.isSource ? colorForSource (j.sourceId) : kInk);
        ctx->setLineWidth (3.0);
        ctx->drawEllipse (jr, kDrawFilledAndStroked);
        if (plugged != kModNone)
        {
            CRect plug (jr); plug.inset (5, 5);
            ctx->setFillColor (colorForSource (plugged));
            ctx->drawEllipse (plug, kDrawFilled);
        }

        if (j.label)
        {
            // an input's label is centred over its socket pair
            const CCoord cx = j.pos.x + (j.isSource ? 0 : kSlotGap * 0.5);
            // black backing, so a cable passing behind never strikes through the text
            const CCoord half = ctx->getStringWidth (j.label) * 0.5 + 3;
            ctx->setFillColor (kPlate);
            ctx->drawRect (CRect (cx - half, j.pos.y - 34, cx + half, j.pos.y - 16), kDrawFilled);
            ctx->drawString (j.label, CRect (cx - 60, j.pos.y - 34, cx + 60, j.pos.y - 16), kCenterText);
        }
    }
}

//------------------------------------------------------------------------
// Click patching: output first, then input. No dragging involved.
CMouseEventResult KastlePatchBayView::onMouseDown (CPoint& wherePt, const CButtonState& buttons)
{
    // VSTGUI asks the container first and only offers the click to the child
    // views (the knobs) when this returns "not handled". So everything that is
    // not the switch or a socket MUST fall through, or the knobs go dead.
    if (!buttons.isLeftButton ()) return kMouseEventNotHandled;

    const CPoint where = toLocal (wherePt);
    if (kSwitchRect.pointInside (where))
    {
        // left button is OFF, right button is ON
        edit (kPowerId, where.x > kSwitchRect.getCenter ().x ? 1.0 : 0.0);
        invalid ();
        return kMouseDownEventHandledButDontNeedMovedOrUpEvents;
    }

    const int idx = findJackAt (where);
    if (idx < 0)
    {
        if (armedSource != kModNone)                  // click elsewhere: put the cable down
        {
            armedSource = kModNone;
            invalid ();
        }
        return kMouseEventNotHandled;
    }
    if (jacks[idx].isSource)
        armedSource = armedSource == jacks[idx].sourceId ? kModNone : jacks[idx].sourceId;
    else if (armedSource != kModNone)
    {
        edit (jacks[idx].destParamId, armedSource / kModSrcSteps);
        armedSource = kModNone;
    }
    else if (sourceOf (jacks[idx]) != kModNone)
        edit (jacks[idx].destParamId, 0.0);           // unplug

    invalid ();
    return kMouseDownEventHandledButDontNeedMovedOrUpEvents;
}

//------------------------------------------------------------------------
namespace {

class KastlePatchBayCreator : public ViewCreatorAdapter
{
public:
    IdStringPtr getViewName () const override { return "KastlePatchBay"; }
    IdStringPtr getBaseViewName () const override { return UIViewCreator::kCViewContainer; }
    UTF8StringPtr getDisplayName () const override { return "Kastle Patch Bay"; }

    CView* create (const UIAttributes& /*attributes*/,
                   const IUIDescription* description) const override
    {
        KastleArt art;
        if (description)
        {
            art.plate     = description->getBitmap ("plate");
            art.knob      = description->getBitmap ("knob");
            art.trim      = description->getBitmap ("trim");
            art.knobBody  = description->getBitmap ("knob_body");
            art.trimBody  = description->getBitmap ("trim_body");
            art.switchOn  = description->getBitmap ("switch_on");
            art.switchOff = description->getBitmap ("switch_off");
            art.ledOn     = description->getBitmap ("led_on");
            art.ledOff    = description->getBitmap ("led_off");
        }
        return new KastlePatchBayView (CRect (0, 0, kPanelW, kPanelH), art);
    }
};

} // anonymous namespace

//------------------------------------------------------------------------
void registerKastleViews ()
{
    static KastlePatchBayCreator creator;
    static bool registered = false;
    if (!registered)
    {
        UIViewFactory::registerViewCreator (creator);
        registered = true;
    }
}

//------------------------------------------------------------------------
} // namespace Vst
} // namespace Steinberg
