//------------------------------------------------------------------------
// KastleVST3 — minimal VST3 instrument wrapping the portable Kastle VCO
// and LFO/Rungler cores (kastle_vco.c, kastle_lfo.c), plus a patch layer:
// each destination (Pitch, Timbre, Waveshape) now has TWO independent
// modulation slots (A and B) that sum together — true cable-summing,
// not just one-cable-per-destination anymore.
// No custom editor yet: the host's generic parameter UI is used.
//
// Derivative of bastl-instruments/kastle firmware (CC BY-SA, Vaclav
// Pelousek). Built against the Steinberg VST3 SDK's SingleComponentEffect
// helper (public.sdk/source/vst/vstsinglecomponenteffect.h).
//------------------------------------------------------------------------
#pragma once

// must always come first
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <atomic>

extern "C" {
#include "kastle_vco.h"
#include "kastle_lfo.h"
}

namespace Steinberg {
namespace Vst {

// modulation source selector, shared by every slot: 0=None, 1=LFO, 2=Rungler
enum KastleModSource : int32 {
    kModNone = 0, kModLFO = 1, kModRungler = 2, kModLfoSquare = 3, kNumModSources = 4
};

// destination index, used to address pModSrc[dest][slot] / pModDepth[dest][slot]
// Mirrors the hardware's modulation inputs. Pitch/Timbre/Waveshape feed the
// VCO, the remaining four feed the LFO/Rungler chip and are evaluated one
// sample earlier, since they decide how the LFO itself is clocked.
enum KastleDest : int32 {
    kDestPitch = 0, kDestTimbre = 1, kDestWaveshape = 2,
    kDestMode = 3, kDestLfoRate = 4, kDestLfoRst = 5, kDestBitIn = 6,
    kNumDests = 7
};
enum { kNumSlots = 2 }; // A and B, summed together

enum KastleParamId : ParamID
{
    kPitchId = 1000,
    kRateId,
    kWaveshapeId,
    kModeId,             // discrete: 0=FM, 1=T&H, 2=Noise
    kXYModeId,            // toggle

    kLfoRateId,           // LFO/Rungler shared clock rate

    kPitchModSrcAId,      // discrete: None|LFO|Rungler
    kPitchModDepthAId,    // attenuverter, 0.5 = no modulation
    kPitchModSrcBId,
    kPitchModDepthBId,

    kTimbreModSrcAId,
    kTimbreModDepthAId,
    kTimbreModSrcBId,
    kTimbreModDepthBId,

    kWaveshapeModSrcAId,
    kWaveshapeModDepthAId,
    kWaveshapeModSrcBId,
    kWaveshapeModDepthBId,

    kBypassId,            // appended last: existing ids must keep their values
    kMonoId,              // sums the Kastle's two outputs onto both channels

    // Stage 1 patch points, appended so every earlier id keeps its value.
    // Each destination stays a block of 4: SrcA, DepthA, SrcB, DepthB.
    kModeModSrcAId,
    kModeModDepthAId,
    kModeModSrcBId,
    kModeModDepthBId,

    kLfoRateModSrcAId,
    kLfoRateModDepthAId,
    kLfoRateModSrcBId,
    kLfoRateModDepthBId,

    kLfoRstModSrcAId,
    kLfoRstModDepthAId,
    kLfoRstModSrcBId,
    kLfoRstModDepthBId,

    kBitInModSrcAId,
    kBitInModDepthAId,
    kBitInModSrcBId,
    kBitInModDepthBId,

    kPowerId,             // the panel's toggle switch; a new instance starts switched off
};

//------------------------------------------------------------------------
class KastleProcessor : public SingleComponentEffect
{
public:
    KastleProcessor ();

    static FUnknown* createInstance (void* /*context*/)
    {
        return (IAudioProcessor*)new KastleProcessor;
    }

    //---from IComponent-----------------------
    tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API terminate () SMTG_OVERRIDE;
    tresult PLUGIN_API setActive (TBool state) SMTG_OVERRIDE;
    tresult PLUGIN_API process (ProcessData& data) SMTG_OVERRIDE;
    tresult PLUGIN_API canProcessSampleSize (int32 symbolicSampleSize) SMTG_OVERRIDE;
    tresult PLUGIN_API setState (IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API getState (IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API setupProcessing (ProcessSetup& newSetup) SMTG_OVERRIDE;

    //---from IEditController-------
    IPlugView* PLUGIN_API createView (const char* /*name*/) SMTG_OVERRIDE;
    tresult PLUGIN_API setParamNormalized (ParamID tag, ParamValue value) SMTG_OVERRIDE;

    // The LFO's current half cycle, for the rate LED on the panel. Written by
    // the audio thread once per block, read by the editor's timer.
    std::atomic<bool> lfoLedOn {false};

    //---Interface---------
    OBJ_METHODS (KastleProcessor, SingleComponentEffect)
    REFCOUNT_METHODS (SingleComponentEffect)

private:
    void applyParamFromHost (ParamID tag, ParamValue value);
    // renders nframes into (possibly mono, possibly stereo) float buffers,
    // computing per-sample modulated Pitch/Timbre/Waveshape along the way
    void renderChunk (float* outL, float* outR, int32 nframes);

    KastleVCO vco {};
    KastleLFO lfo {};

    float pPitch = 0.5f, pRate = 0.5f, pWaveshape = 0.5f;
    int32 pMode = 0;
    bool  pXY = false;
    float pLfoRate = 0.5f;
    bool  pBypass = false;
    bool  pMono = false;
    bool  pPower = false;

    // edge detector for the LFO RST patch point
    bool  lfoRstArmed = false;

    // [destination][slot], slot A and B summed together per destination
    int32 pModSrc[kNumDests][kNumSlots]   = {};   // kModNone
    // 0.5 = no modulation. Every row must be listed: a missing one is zero,
    // which is full inverted depth, not "off".
    float pModDepth[kNumDests][kNumSlots] = {{0.5f,0.5f},{0.5f,0.5f},{0.5f,0.5f},{0.5f,0.5f},
                                             {0.5f,0.5f},{0.5f,0.5f},{0.5f,0.5f}};
    static_assert (kNumDests == 7, "add a row to pModDepth for the new destination");
};

//------------------------------------------------------------------------
} // namespace Vst
} // namespace Steinberg
