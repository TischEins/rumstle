//------------------------------------------------------------------------
// kastle_standalone.cpp — RUMSTLE.exe: the plugin's own editor in a
// window, with sound on the default Windows output device.
//
// Window handling is the Steinberg SDK's "editorhost" sample, cut down to one
// window; the audio thread (WASAPI, shared mode) is the part added here. The
// exe loads rumstle.vst3 from the folder it sits in, so the two ship
// together and there is one build of the synth, not two. The "Set" menu
// saves and loads the whole panel (knobs and cables) as a .rumstle file.
//------------------------------------------------------------------------
#include "public.sdk/samples/vst-hosting/editorhost/source/platform/appinit.h"
#include "public.sdk/samples/vst-hosting/editorhost/source/platform/iapplication.h"
#include "public.sdk/samples/vst-hosting/editorhost/source/platform/iwindow.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <commdlg.h>

namespace Steinberg {
namespace Vst {
namespace EditorHost {

//------------------------------------------------------------------------
// Renders the plugin to the default output device.
class AudioOut
{
public:
    // returns an empty string on success, else what went wrong
    std::string start (IComponent* comp)
    {
        component = comp;
        processor = FUnknownPtr<IAudioProcessor> (comp);
        if (!processor)
            return "Plugin has no audio processor";

        CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        IMMDeviceEnumerator* devices = nullptr;
        IMMDevice* device = nullptr;
        bool ok = SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                               __uuidof (IMMDeviceEnumerator), (void**)&devices)) &&
                  SUCCEEDED (devices->GetDefaultAudioEndpoint (eRender, eConsole, &device)) &&
                  SUCCEEDED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) &&
                  SUCCEEDED (client->GetMixFormat (&format));
        if (device) device->Release ();
        if (devices) devices->Release ();
        if (!ok)
            return "No audio output device found";
        // ponytail: the shared-mode mix format is 32-bit float on every current
        // Windows; anything else is refused rather than converted.
        if (format->wBitsPerSample != 32)
            return "Audio device mix format is not 32-bit float";

        event = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        ok = SUCCEEDED (client->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                            0, 0, format, nullptr)) &&
             SUCCEEDED (client->SetEventHandle (event)) &&
             SUCCEEDED (client->GetBufferSize (&bufferFrames)) &&
             SUCCEEDED (client->GetService (__uuidof (IAudioRenderClient), (void**)&render));
        if (!ok)
            return "Could not open the audio output device";

        ProcessSetup setup {kRealtime, kSample32, (int32)bufferFrames, (SampleRate)format->nSamplesPerSec};
        processor->setupProcessing (setup);
        component->activateBus (kAudio, kOutput, 0, true);
        component->setActive (true);
        processor->setProcessing (true);

        running = true;
        thread = std::thread ([this] { run (); });
        client->Start ();
        return {};
    }

    void stop ()
    {
        if (running.exchange (false))
        {
            thread.join ();
            client->Stop ();
            processor->setProcessing (false);
            component->setActive (false);
        }
        if (render) { render->Release (); render = nullptr; }
        if (client) { client->Release (); client = nullptr; }
        if (format) { CoTaskMemFree (format); format = nullptr; }
        if (event) { CloseHandle (event); event = nullptr; }
        processor = nullptr;
    }

private:
    void run ()
    {
        // The Kastle is a drone: full scale from the first sample. Leave headroom
        // so starting the program does not hit the speakers at 0 dBFS.
        constexpr float kGain = 0.25f;

        std::vector<float> left (bufferFrames), right (bufferFrames);
        const UINT32 deviceChannels = format->nChannels;
        while (running)
        {
            // ponytail: if the device goes away (headphones unplugged) this just
            // falls silent; restart the program. Upgrade: IMMNotificationClient.
            if (WaitForSingleObject (event, 200) != WAIT_OBJECT_0)
                continue;
            UINT32 padding = 0;
            if (FAILED (client->GetCurrentPadding (&padding)) || padding >= bufferFrames)
                continue;
            const UINT32 frames = bufferFrames - padding;
            BYTE* dst = nullptr;
            if (FAILED (render->GetBuffer (frames, &dst)))
                continue;

            float* channels[2] = { left.data (), right.data () };
            AudioBusBuffers bus;
            bus.numChannels = 2;
            bus.channelBuffers32 = channels;
            ProcessData data;
            data.processMode = kRealtime;
            data.symbolicSampleSize = kSample32;
            data.numSamples = (int32)frames;
            data.numOutputs = 1;
            data.outputs = &bus;
            processor->process (data);

            auto* out = (float*)dst;
            for (UINT32 i = 0; i < frames; ++i)
                for (UINT32 c = 0; c < deviceChannels; ++c)
                    *out++ = c == 0 ? left[i] * kGain : c == 1 ? right[i] * kGain : 0.0f;
            render->ReleaseBuffer (frames, 0);
        }
    }

    IComponent* component = nullptr;
    IPtr<IAudioProcessor> processor;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* format = nullptr;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0;
    std::atomic<bool> running {false};
    std::thread thread;
};

//------------------------------------------------------------------------
// Sizes the window so its client area is exactly w x h. The SDK window's own
// resize() does not know about the menu bar and would cut the panel short.
// ponytail: a DPI change while running still goes through the SDK's resize;
// if the bottom strip gets clipped after moving between monitors, restart.
static void fitClient (HWND hwnd, int w, int h)
{
    RECT win {}, client {};
    GetWindowRect (hwnd, &win);
    GetClientRect (hwnd, &client);
    SetWindowPos (hwnd, nullptr, 0, 0, (win.right - win.left) + w - client.right,
                  (win.bottom - win.top) + h - client.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

//------------------------------------------------------------------------
class WindowController : public IWindowController, public IPlugFrame
{
public:
    WindowController (const IPtr<IPlugView>& plugView) : plugView (plugView) {}

    void onShow (IWindow& w) override
    {
        window = &w;
        auto platformWindow = window->getNativePlatformWindow ();
        plugView->setFrame (this);
        if (plugView->attached (platformWindow.ptr, platformWindow.type) != kResultTrue)
            IPlatform::instance ().kill (-1, "Attaching PlugView failed");
    }
    void onClose (IWindow&) override
    {
        closePlugView ();
        IPlatform::instance ().quit ();
    }
    void onResize (IWindow&, Size newSize) override
    {
        if (!plugView) return;
        ViewRect r {0, 0, newSize.width, newSize.height};
        plugView->onSize (&r);
    }
    Size constrainSize (IWindow&, Size requestedSize) override
    {
        ViewRect r {0, 0, requestedSize.width, requestedSize.height};
        if (plugView && plugView->checkSizeConstraint (&r) != kResultTrue)
            requestedSize = {r.right - r.left, r.bottom - r.top};
        return requestedSize;
    }
    void onContentScaleFactorChanged (IWindow&, float newScaleFactor) override
    {
        if (auto css = U::cast<IPlugViewContentScaleSupport> (plugView))
            css->setContentScaleFactor (newScaleFactor);
    }

    // IPlugFrame
    tresult PLUGIN_API resizeView (IPlugView* view, ViewRect* newSize) override
    {
        if (!newSize || !view || view != plugView || !window)
            return kInvalidArgument;
        fitClient ((HWND)window->getNativePlatformWindow ().ptr, newSize->right - newSize->left,
                   newSize->bottom - newSize->top);
        plugView->onSize (newSize);
        return kResultTrue;
    }

    void closePlugView ()
    {
        if (plugView)
        {
            plugView->setFrame (nullptr);
            plugView->removed ();
            plugView = nullptr;
        }
        window = nullptr;
    }

private:
    tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual (_iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual (_iid, FUnknown::iid))
        {
            *obj = this;
            return kResultTrue;
        }
        if (window)
            return window->queryInterface (_iid, obj);
        return kNoInterface;
    }
    // owned by the App; a plug-in's release must not destroy it
    uint32 PLUGIN_API addRef () override { return 1000; }
    uint32 PLUGIN_API release () override { return 1000; }

    IPtr<IPlugView> plugView;
    IWindow* window {nullptr};
};

//------------------------------------------------------------------------
class App : public IApplication
{
public:
    ~App () noexcept override { terminate (); }

    void init (const std::vector<std::string>& cmdArgs) override
    {
        // default: the bundle next to this exe; a path argument overrides it
        std::string path;
        if (!cmdArgs.empty () && cmdArgs.back ().find (".vst3") != std::string::npos)
            path = cmdArgs.back ();
        else
        {
            char exe[MAX_PATH] {};
            GetModuleFileNameA (nullptr, exe, MAX_PATH);
            path = exe;
            path = path.substr (0, path.find_last_of ("\\/") + 1) + "rumstle.vst3";
        }

        PluginContextFactory::instance ().setPluginContext (&pluginContext);

        std::string error;
        module = VST3::Hosting::Module::create (path, error);
        if (!module)
            IPlatform::instance ().kill (-1, "Could not load " + path + "\n" + error);

        auto factory = module->getFactory ();
        for (auto& classInfo : factory.classInfos ())
        {
            if (classInfo.category () != kVstAudioEffectClass)
                continue;
            plugProvider = owned (new PlugProvider (factory, classInfo, true));
            if (!plugProvider->initialize ())
                plugProvider = nullptr;
            break;
        }
        if (!plugProvider)
            IPlatform::instance ().kill (-1, "No VST3 audio class in " + path);

        component = plugProvider->getComponent ();
        auto controller = plugProvider->getController ();
        component->release (); // plugProvider keeps its own references
        controller->release ();

        auto view = owned (controller->createView (ViewType::kEditor));
        ViewRect size {};
        if (!view || view->getSize (&size) != kResultTrue)
            IPlatform::instance ().kill (-1, "Plugin has no editor");

        windowController = std::make_shared<WindowController> (view);
        window = IPlatform::instance ().createWindow (
            "RUMSTLE", {size.right - size.left, size.bottom - size.top}, false, windowController);
        if (!window)
            IPlatform::instance ().kill (-1, "Could not create window");
        window->show ();
        addSetMenu ();

        // the editor stays usable without sound, so this is a warning, not fatal
        const std::string audioError = audio.start (component);
        if (!audioError.empty ())
            MessageBoxA (nullptr, audioError.c_str (), "RUMSTLE: kein Ton", MB_OK | MB_ICONWARNING);
    }

    void terminate () override
    {
        audio.stop ();
        if (windowController)
            windowController->closePlugView ();
        windowController.reset ();
        plugProvider.reset ();
        module.reset ();
        PluginContextFactory::instance ().setPluginContext (nullptr);
    }

private:
    enum { kMenuOpen = 1, kMenuSave = 2 };
    // file = magic + the plugin's own state stream; the magic keeps an
    // arbitrary file from being fed to setState as knob values
    static constexpr char kMagic[8] = {'R','U','M','S','T','L','E','1'};

    void addSetMenu ()
    {
        hwnd = (HWND)window->getNativePlatformWindow ().ptr;
        RECT client {};
        GetClientRect (hwnd, &client);
        HMENU bar = CreateMenu (), set = CreatePopupMenu ();
        AppendMenuW (set, MF_STRING, kMenuOpen, L"Set \u00F6ffnen...");
        AppendMenuW (set, MF_STRING, kMenuSave, L"Set speichern unter...");
        AppendMenuW (bar, MF_POPUP, (UINT_PTR)set, L"Set");
        SetMenu (hwnd, bar);
        fitClient (hwnd, client.right, client.bottom);

        // the window class belongs to the SDK sample, so WM_COMMAND is caught
        // by subclassing; one window per process, hence the statics
        self = this;
        sdkProc = (WNDPROC)SetWindowLongPtrW (hwnd, GWLP_WNDPROC, (LONG_PTR)menuProc);
    }

    static LRESULT CALLBACK menuProc (HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_COMMAND && HIWORD (wp) == 0 && LOWORD (wp) == kMenuOpen) { self->openSet (); return 0; }
        if (msg == WM_COMMAND && HIWORD (wp) == 0 && LOWORD (wp) == kMenuSave) { self->saveSet (); return 0; }
        return CallWindowProcW (sdkProc, h, msg, wp, lp);
    }

    bool askPath (bool save, wchar_t* path)
    {
        OPENFILENAMEW ofn {};
        ofn.lStructSize = sizeof (ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"RUMSTLE Set (*.rumstle)\0*.rumstle\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"rumstle";
        ofn.Flags = save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST;
        return (save ? GetSaveFileNameW (&ofn) : GetOpenFileNameW (&ofn)) != 0;
    }

    void fail (const wchar_t* text) { MessageBoxW (hwnd, text, L"RUMSTLE", MB_OK | MB_ICONWARNING); }

    void saveSet ()
    {
        wchar_t path[MAX_PATH] = L"";
        if (!askPath (true, path))
            return;
        MemoryStream state;
        FILE* f = nullptr;
        bool ok = component->getState (&state) == kResultOk && _wfopen_s (&f, path, L"wb") == 0 && f;
        if (ok)
        {
            ok = fwrite (kMagic, 1, sizeof (kMagic), f) == sizeof (kMagic) &&
                 fwrite (state.getData (), 1, (size_t)state.getSize (), f) == (size_t)state.getSize ();
            ok = fclose (f) == 0 && ok;
        }
        if (!ok)
            fail (L"Das Set konnte nicht gespeichert werden.");
    }

    void openSet ()
    {
        wchar_t path[MAX_PATH] = L"";
        if (!askPath (false, path))
            return;
        std::vector<char> bytes;
        FILE* f = nullptr;
        if (_wfopen_s (&f, path, L"rb") == 0 && f)
        {
            char buf[4096];
            for (size_t n; (n = fread (buf, 1, sizeof (buf), f)) > 0;)
                bytes.insert (bytes.end (), buf, buf + n);
            fclose (f);
        }
        if (bytes.size () <= sizeof (kMagic) || memcmp (bytes.data (), kMagic, sizeof (kMagic)) != 0)
            return fail (L"Das ist keine RUMSTLE-Set-Datei.");
        MemoryStream state (bytes.data () + sizeof (kMagic), (TSize)(bytes.size () - sizeof (kMagic)));
        if (component->setState (&state) != kResultOk)
            fail (L"Die Set-Datei ist unvollst\u00E4ndig.");
    }

    static inline App* self = nullptr;
    static inline WNDPROC sdkProc = nullptr;
    HWND hwnd = nullptr;
    IComponent* component = nullptr;   // owned by plugProvider

    VST3::Hosting::Module::Ptr module {nullptr};
    IPtr<PlugProvider> plugProvider {nullptr};
    Vst::HostApplication pluginContext;
    WindowPtr window;
    std::shared_ptr<WindowController> windowController;
    AudioOut audio;
};

static AppInit gInit (std::make_unique<App> ());

//------------------------------------------------------------------------
} // EditorHost
} // Vst
} // Steinberg
