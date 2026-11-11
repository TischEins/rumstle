//------------------------------------------------------------------------
// KastleVST3.cpp — see kastle_vst3.h for description and provenance.
//------------------------------------------------------------------------
#include "kastle_vst3.h"
#include "kastle_editor.h"

#include "vstgui/plugin-bindings/vst3editor.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include "base/source/fstreamer.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace Steinberg {
namespace Vst {

namespace {
constexpr int32 kMaxBlock = 8192;

inline float clamp01 (float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline float modSourceValue (int32 src, float lfoV, float runglerV, float lfoSqV)
{
    switch (src) {
        case KastleModSource::kModLFO:       return lfoV;
        case KastleModSource::kModRungler:   return runglerV;
        case KastleModSource::kModLfoSquare: return lfoSqV;
        default:                             return 0.0f;
    }
}

// normalized <-> discrete conversion for the mod-source selectors; they now
// carry four values, so the divisor is no longer 2
constexpr double kModSrcSteps = KastleModSource::kNumModSources - 1; // 3

// Every destination's param IDs are laid out as 4 consecutive IDs starting
// at its "SrcA" id: SrcA, DepthA, SrcB, DepthB. This lets us map any of the
// 12 patch-param tags back to (destination, slot, isDepth) with simple math
// instead of a 12-way switch.
// The three VCO destinations are the ones v1.2 already stored; everything
// after them is appended to the state stream so older sessions still load.
constexpr int32 kNumLegacyDests = 3;

constexpr ParamID kDestBaseId[kNumDests] = {
    kPitchModSrcAId, kTimbreModSrcAId, kWaveshapeModSrcAId,
    kModeModSrcAId, kLfoRateModSrcAId, kLfoRstModSrcAId, kBitInModSrcAId
};
} // anonymous namespace

//------------------------------------------------------------------------
KastleProcessor::KastleProcessor () {}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::initialize (FUnknown* context)
{
    tresult result = SingleComponentEffect::initialize (context);
    if (result != kResultOk)
        return result;

    // No audio input — this is a CV-controlled instrument, not an effect.
    addAudioOutput (STR16 ("Stereo Out"), SpeakerArr::kStereo);

    // Kept for host compatibility (e.g. note-on could later feed pitch);
    // the Kastle itself has no MIDI, so this bus is currently unused.
    addEventInput (STR16 ("Event In"), 1);

    parameters.addParameter (USTRING ("Pitch"), nullptr, 0, 0.5,
                              ParameterInfo::kCanAutomate, kPitchId);
    parameters.addParameter (USTRING ("Timbre"), nullptr, 0, 0.5,
                              ParameterInfo::kCanAutomate, kRateId);
    parameters.addParameter (USTRING ("Waveshape"), nullptr, 0, 0.5,
                              ParameterInfo::kCanAutomate, kWaveshapeId);

    // 3-step discrete parameter: FM / T&H / Noise. StringListParameter, not a
    // plain one with the names stuffed into the units field -- only this gives
    // hosts (and the segment button in the editor) real per-step strings.
    {
        auto* modeParam = new StringListParameter (USTRING ("Mode"), kModeId);
        modeParam->appendString (USTRING ("FM"));
        modeParam->appendString (USTRING ("T&H"));
        modeParam->appendString (USTRING ("Noise"));
        parameters.addParameter (modeParam);
    }

    parameters.addParameter (USTRING ("XY Mode"), nullptr, 1, 0.0,
                              ParameterInfo::kCanAutomate, kXYModeId);

    parameters.addParameter (USTRING ("LFO Rate"), nullptr, 0, 0.5,
                              ParameterInfo::kCanAutomate, kLfoRateId);

    // Hosts expect a flagged bypass control even on an instrument; bypassing a
    // generator means it outputs silence.
    parameters.addParameter (USTRING ("Bypass"), nullptr, 1, 0.0,
                              ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, kBypassId);

    // The hardware has two separate outs carrying different signals; this folds
    // them into one so the plugin can sit in a mono chain.
    parameters.addParameter (USTRING ("Mono"), nullptr, 1, 0.0,
                              ParameterInfo::kCanAutomate, kMonoId);

    // The device's power switch. Off is the default, so a freshly loaded
    // instance is silent until it is switched on.
    parameters.addParameter (USTRING ("Power"), nullptr, 1, 0.0,
                              ParameterInfo::kCanAutomate, kPowerId);

    // Patch layer: two summed slots (A, B) per destination.
    static const char* destNames[kNumDests] = { "Pitch", "Timbre", "Waveshape",
                                                "Mode", "LFO Rate", "LFO Rst", "Bit In" };
    static const char* slotNames[kNumSlots] = { "A", "B" };
    for (int32 d = 0; d < kNumDests; ++d)
    {
        for (int32 s = 0; s < kNumSlots; ++s)
        {
            ParamID srcId   = (ParamID)(kDestBaseId[d] + s*2);
            ParamID depthId = (ParamID)(kDestBaseId[d] + s*2 + 1);
            char srcName[64], depthName[64];
            snprintf (srcName, sizeof(srcName), "%s Mod Src %s", destNames[d], slotNames[s]);
            snprintf (depthName, sizeof(depthName), "%s Mod Depth %s", destNames[d], slotNames[s]);
            auto* srcParam = new StringListParameter (USTRING (srcName), srcId);
            srcParam->appendString (USTRING ("None"));
            srcParam->appendString (USTRING ("LFO"));
            srcParam->appendString (USTRING ("Rungler"));
            srcParam->appendString (USTRING ("LFO Sq"));
            parameters.addParameter (srcParam);
            parameters.addParameter (USTRING (depthName), nullptr, 0, 0.5,
                                      ParameterInfo::kCanAutomate, depthId);
        }
    }

    kastle_vco_init (&vco, 44100.0f);
    kastle_lfo_init (&lfo, 44100.0f);
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::terminate ()
{
    return SingleComponentEffect::terminate ();
}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::setActive (TBool /*state*/)
{
    return kResultOk;
}

//------------------------------------------------------------------------
IPlugView* PLUGIN_API KastleProcessor::createView (const char* name)
{
    if (name && strcmp (name, "editor") == 0)
    {
        // The patch bay is a custom view referenced by name from the .uidesc,
        // so the factory has to know it before the description is parsed.
        registerKastleViews ();
        return new VSTGUI::VST3Editor (this, "view", "kastleport.uidesc");
    }
    return nullptr;
}

//------------------------------------------------------------------------
void KastleProcessor::applyParamFromHost (ParamID tag, ParamValue value)
{
    switch (tag)
    {
        case kPitchId:     pPitch = (float)value; break;
        case kRateId:      pRate = (float)value; break;
        case kWaveshapeId: pWaveshape = (float)value; break;
        case kModeId:      pMode = (int32)(value * 2.0 + 0.5); break; // 0..2
        case kXYModeId:    pXY = value > 0.5; break;
        case kBypassId:    pBypass = value >= 0.5; break;
        case kMonoId:      pMono = value >= 0.5; break;
        case kPowerId:     pPower = value >= 0.5; break;
        case kLfoRateId:   pLfoRate = (float)value; break;
        default:
        {
            // Any of the 12 patch params: figure out (destination, slot, isDepth)
            // from its offset relative to that destination's base id.
            for (int32 d = 0; d < kNumDests; ++d)
            {
                int32 offset = (int32)tag - (int32)kDestBaseId[d];
                if (offset >= 0 && offset < kNumSlots*2)
                {
                    int32 slot = offset / 2;
                    bool isDepth = (offset % 2) == 1;
                    if (isDepth)
                        pModDepth[d][slot] = (float)value;
                    else
                        pModSrc[d][slot] = (int32)(value * kModSrcSteps + 0.5); // 0..3
                    break;
                }
            }
            break;
        }
    }
    kastle_vco_set_mode (&vco, (KastleMode)pMode);
    kastle_vco_set_xy_mode (&vco, pXY);
    kastle_lfo_set_rate (&lfo, pLfoRate);
    // Pitch/Timbre/Waveshape themselves are set per-sample in renderChunk()
    // once modulation is folded in, not here.
}

//------------------------------------------------------------------------
void KastleProcessor::renderChunk (float* outL, float* outR, int32 nframes)
{
    float amt[kNumDests][kNumSlots];
    for (int32 d = 0; d < kNumDests; ++d)
        for (int32 s = 0; s < kNumSlots; ++s)
            amt[d][s] = (pModDepth[d][s] - 0.5f) * 2.0f;

    // Bit In sits at the firmware's default (classic XOR-invert) when unpatched;
    // 155/255 is that value expressed on the 0..1 scale the patch layer uses.
    const float kBitInBase = 155.0f / 255.0f;
    const float base[kNumDests] = { pPitch, pRate, pWaveshape,
                                    pMode / 2.0f, pLfoRate, 0.0f, kBitInBase };

    // Modulation sources are the LFO chip's outputs. The four LFO-side
    // destinations therefore have to be evaluated from the PREVIOUS sample --
    // they decide how this sample's LFO is clocked, so they cannot depend on
    // its result. Patching the LFO to its own rate is a real move on the
    // hardware, and this is the one-sample delay that makes it well-defined.
    float lfoPrev = 0.0f, rngPrev = 0.0f, lfoSqPrev = 0.0f;

    for (int32 i = 0; i < nframes; ++i)
    {
        auto modulate = [&] (int32 d) {
            float sum = base[d];
            for (int32 s = 0; s < kNumSlots; ++s)
                sum += amt[d][s] * modSourceValue (pModSrc[d][s], lfoPrev, rngPrev, lfoSqPrev);
            return sum;
        };

        //---- LFO-side destinations, from the previous sample ----------------
        kastle_lfo_set_rate (&lfo, clamp01 (modulate (kDestLfoRate)));

        lfo.chaos_cv = (uint8_t)(clamp01 (modulate (kDestBitIn)) * 255.0f + 0.5f);

        const bool rstHigh = modulate (kDestLfoRst) > 0.5f;
        if (rstHigh && !lfoRstArmed)
            kastle_lfo_reset (&lfo);
        lfoRstArmed = rstHigh;

        float lfoS = 0.0f, rngS = 0.0f;
        kastle_lfo_process (&lfo, &lfoS, &rngS, 1);
        // the firmware's half-cycle flag is exactly the hardware's square out
        const float lfoSqS = lfo.lfo_flop ? 1.0f : -1.0f;

        lfoPrev = lfoS; rngPrev = rngS; lfoSqPrev = lfoSqS;

        //---- VCO-side destinations, from this sample's sources --------------
        float dest[kNumDests];
        for (int32 d = 0; d < kNumDests; ++d)
        {
            float sum = base[d];
            for (int32 s = 0; s < kNumSlots; ++s)
                sum += amt[d][s] * modSourceValue (pModSrc[d][s], lfoS, rngS, lfoSqS);
            dest[d] = clamp01 (sum);
        }

        kastle_vco_set_pitch (&vco, dest[kDestPitch]);
        kastle_vco_set_rate (&vco, dest[kDestTimbre]);
        kastle_vco_set_waveshape (&vco, dest[kDestWaveshape]);
        kastle_vco_set_mode (&vco, (KastleMode)(int32)(dest[kDestMode] * 2.0f + 0.5f));

        float l, r;
        kastle_vco_process (&vco, &l, &r, 1);

        if (pMono)
        {
            // out1 and out2 are independent signals, not a stereo pair, so the
            // fold-down is a straight sum at -6 dB rather than a downmix
            const float m = (l + r) * 0.5f;
            l = r = m;
        }

        outL[i] = l;
        if (outR) outR[i] = r;
    }
    lfoLedOn.store (lfo.lfo_flop != 0, std::memory_order_relaxed);
}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::process (ProcessData& data)
{
    //---1) Read parameter changes-----------
    if (IParameterChanges* paramChanges = data.inputParameterChanges)
    {
        int32 numParamsChanged = paramChanges->getParameterCount ();
        for (int32 i = 0; i < numParamsChanged; i++)
        {
            if (IParamValueQueue* q = paramChanges->getParameterData (i))
            {
                ParamValue value;
                int32 sampleOffset;
                int32 numPoints = q->getPointCount ();
                if (numPoints <= 0) continue;
                if (q->getPoint (numPoints - 1, sampleOffset, value) != kResultTrue)
                    continue;
                applyParamFromHost (q->getParameterId (), value);
            }
        }
    }

    //---2) No audio input to read; this is a generator-----------

    if (data.numOutputs == 0)
        return kResultOk;

    int32 numChannels = data.outputs[0].numChannels;
    if (numChannels < 1 || data.numSamples <= 0)
        return kResultOk;

    void** out = getChannelBuffersPointer (processSetup, data.outputs[0]);
    data.outputs[0].silenceFlags = 0;

    if (pBypass || !pPower)
    {
        lfoLedOn.store (false, std::memory_order_relaxed);
        const size_t sampleBytes = data.symbolicSampleSize == kSample32 ? sizeof (Sample32)
                                                                       : sizeof (Sample64);
        for (int32 c = 0; c < numChannels; ++c)
            memset (out[c], 0, (size_t)data.numSamples * sampleBytes);
        data.outputs[0].silenceFlags = ((uint64)1 << numChannels) - 1;
        return kResultOk;
    }

    int32 n = data.numSamples;
    int32 done = 0;
    float bufL[kMaxBlock];
    float bufR[kMaxBlock];

    if (data.symbolicSampleSize == kSample32)
    {
        auto* l32 = (Sample32*)out[0];
        auto* r32 = numChannels > 1 ? (Sample32*)out[1] : nullptr;
        while (done < n)
        {
            int32 chunk = (n - done) > kMaxBlock ? kMaxBlock : (n - done);
            renderChunk (bufL, r32 ? bufR : nullptr, chunk);
            for (int32 i = 0; i < chunk; ++i)
            {
                l32[done + i] = bufL[i];
                if (r32) r32[done + i] = bufR[i];
            }
            done += chunk;
        }
    }
    else
    {
        // 64-bit path: render to the same stack buffers, then widen.
        auto* l64 = (Sample64*)out[0];
        auto* r64 = numChannels > 1 ? (Sample64*)out[1] : nullptr;
        while (done < n)
        {
            int32 chunk = (n - done) > kMaxBlock ? kMaxBlock : (n - done);
            renderChunk (bufL, r64 ? bufR : nullptr, chunk);
            for (int32 i = 0; i < chunk; ++i)
            {
                l64[done + i] = (Sample64)bufL[i];
                if (r64) r64[done + i] = (Sample64)bufR[i];
            }
            done += chunk;
        }
    }

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::setState (IBStream* state)
{
    IBStreamer streamer (state, kLittleEndian);
    float p, r, w, lr;
    int32 m, xy;
    int32 srcTmp[kNumDests][kNumSlots];
    float depthTmp[kNumDests][kNumSlots];

    if (!streamer.readFloat (p) || !streamer.readFloat (r) ||
        !streamer.readFloat (w) || !streamer.readInt32 (m) ||
        !streamer.readInt32 (xy) || !streamer.readFloat (lr))
        return kResultFalse;

    for (int32 d = 0; d < kNumLegacyDests; ++d)
        for (int32 s = 0; s < kNumSlots; ++s)
            if (!streamer.readInt32 (srcTmp[d][s]) || !streamer.readFloat (depthTmp[d][s]))
                return kResultFalse;

    // written after the 1.2 payload, so states saved by older versions simply
    // stop here and keep the default
    int32 byp = 0;
    if (!streamer.readInt32 (byp))
        byp = 0;
    int32 mono = 0;
    if (!streamer.readInt32 (mono))
        mono = 0;

    // The LFO-side destinations were added last, so a shorter stream simply
    // means an older session: leave them unpatched.
    for (int32 d = kNumLegacyDests; d < kNumDests; ++d)
        for (int32 s = 0; s < kNumSlots; ++s)
        {
            if (!streamer.readInt32 (srcTmp[d][s]))
                srcTmp[d][s] = kModNone;
            if (!streamer.readFloat (depthTmp[d][s]))
                depthTmp[d][s] = 0.5f;
        }

    // appended by v1.8. States from before the power switch existed were
    // always sounding, so they come back switched on.
    int32 power = 1;
    if (!streamer.readInt32 (power))
        power = 1;
    pPower = power != 0;

    pPitch = p; pRate = r; pWaveshape = w; pMode = m; pXY = xy != 0;
    pLfoRate = lr;
    pBypass = byp != 0;
    pMono = mono != 0;
    for (int32 d = 0; d < kNumDests; ++d)
        for (int32 s = 0; s < kNumSlots; ++s)
        { pModSrc[d][s] = srcTmp[d][s]; pModDepth[d][s] = depthTmp[d][s]; }

    kastle_vco_set_mode (&vco, (KastleMode)pMode);
    kastle_vco_set_xy_mode (&vco, pXY);
    kastle_lfo_set_rate (&lfo, pLfoRate);

    setParamNormalized (kPitchId, pPitch);
    setParamNormalized (kRateId, pRate);
    setParamNormalized (kWaveshapeId, pWaveshape);
    setParamNormalized (kModeId, pMode / 2.0);
    setParamNormalized (kXYModeId, pXY ? 1.0 : 0.0);
    setParamNormalized (kLfoRateId, pLfoRate);
    setParamNormalized (kBypassId, pBypass ? 1.0 : 0.0);
    setParamNormalized (kMonoId, pMono ? 1.0 : 0.0);
    setParamNormalized (kPowerId, pPower ? 1.0 : 0.0);
    for (int32 d = 0; d < kNumDests; ++d)
        for (int32 s = 0; s < kNumSlots; ++s)
        {
            // divisor must match applyParamFromHost, or a reloaded session
            // comes back with every cable moved to the next source
            setParamNormalized ((ParamID)(kDestBaseId[d] + s*2), pModSrc[d][s] / kModSrcSteps);
            setParamNormalized ((ParamID)(kDestBaseId[d] + s*2 + 1), pModDepth[d][s]);
        }
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::getState (IBStream* state)
{
    IBStreamer streamer (state, kLittleEndian);
    streamer.writeFloat (pPitch);
    streamer.writeFloat (pRate);
    streamer.writeFloat (pWaveshape);
    streamer.writeInt32 (pMode);
    streamer.writeInt32 (pXY ? 1 : 0);
    streamer.writeFloat (pLfoRate);
    // --- v1.2 payload, byte-for-byte unchanged ---
    for (int32 d = 0; d < kNumLegacyDests; ++d)
        for (int32 s = 0; s < kNumSlots; ++s)
        {
            streamer.writeInt32 (pModSrc[d][s]);
            streamer.writeFloat (pModDepth[d][s]);
        }
    // --- appended by v1.4 ---
    streamer.writeInt32 (pBypass ? 1 : 0);
    streamer.writeInt32 (pMono ? 1 : 0);
    // --- appended by v1.5: the LFO-side patch points ---
    for (int32 d = kNumLegacyDests; d < kNumDests; ++d)
        for (int32 s = 0; s < kNumSlots; ++s)
        {
            streamer.writeInt32 (pModSrc[d][s]);
            streamer.writeFloat (pModDepth[d][s]);
        }
    // --- appended by v1.8 ---
    streamer.writeInt32 (pPower ? 1 : 0);
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::setupProcessing (ProcessSetup& newSetup)
{
    kastle_vco_set_sample_rate (&vco, (float)newSetup.sampleRate);
    kastle_lfo_set_sample_rate (&lfo, (float)newSetup.sampleRate);
    return SingleComponentEffect::setupProcessing (newSetup);
}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::canProcessSampleSize (int32 symbolicSampleSize)
{
    return (symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64)
        ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API KastleProcessor::setParamNormalized (ParamID tag, ParamValue value)
{
    applyParamFromHost (tag, value);
    return SingleComponentEffect::setParamNormalized (tag, value);
}

//------------------------------------------------------------------------
} // namespace Vst
} // namespace Steinberg

//------------------------------------------------------------------------
BEGIN_FACTORY_DEF ("TischEins", "https://tischeins.org", "mailto:info@tischeins.org")

    DEF_CLASS2 (INLINE_UID (0xFE197112, 0xC3AF4CFB, 0xA9565558, 0x8E341B23), // random, generated once — do not reuse for other plugins
                PClassInfo::kManyInstances,
                kVstAudioEffectClass,
                "RUMSTLE",                         // plug-in name shown in the host
                0,                                  // single component effects cannot be distributed
                "Instrument|Synth",                 // subcategory
                "1.12.0",                           // version — 1.12.0: first release build (static runtime, contact)
                kVstVersionString,
                Steinberg::Vst::KastleProcessor::createInstance)

END_FACTORY
