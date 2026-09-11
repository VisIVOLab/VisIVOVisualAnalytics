#include "CubeVRController.h"

#include <QDebug>
#include <QTimer>

#include <algorithm>
#include <cmath>

#ifdef VISIVO_HAS_VR
#  include <vtkActor.h>
#  include <vtkCallbackCommand.h>
#  include <vtkCamera.h>
#  include <vtkCommand.h>
#  include <vtkEventData.h>
#  include <vtkInteractorStyle3D.h>
#  include <vtkMatrix4x4.h>
#  include <vtkOpenXRInteractorStyle.h>
#  include <vtkOpenXRManager.h>
#  include <vtkOpenXRRenderWindow.h>
#  include <vtkOpenXRRenderWindowInteractor.h>
#  include <vtkOpenXRRenderer.h>
#  include <vtkOutlineSource.h>
#  include <vtkPolyDataMapper.h>
#  include <vtkProperty.h>
#  include <vtkRenderer.h>
#  include <vtkSphereSource.h>
#  include <vtkVolume.h>
#  include <vtkNew.h>
#  include <vtkSmartPointer.h>
#endif

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Impl (PIMPL) — only defined when VR is compiled in. The base class methods
// branch on the pointer being non-null, so the VR-less build is a clean stub
// without dragging OpenXR headers into translation units that don't need them.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef VISIVO_HAS_VR
// Tiny subclass that exposes the otherwise-protected event-loop state. Stock
// vtkOpenXRRenderWindowInteractor sets ``Done = true`` in its constructor and
// only flips it to false inside the blocking ``Start() → StartEventLoop()``
// path. We bypass Start() (we pump frames from a QTimer instead), so without
// this helper ``Done`` stays true and the very first ``DoOneEvent`` returns
// immediately without polling actions or rendering — net effect: the headset
// shows nothing forever.
class VisivoOpenXRInteractor : public vtkOpenXRRenderWindowInteractor
{
public:
    static VisivoOpenXRInteractor *New() { return new VisivoOpenXRInteractor; }
    vtkTypeMacro(VisivoOpenXRInteractor, vtkOpenXRRenderWindowInteractor);
    void EnableManualEventLoop()
    {
        this->Done = false;
        this->StartedMessageLoop = 1;
    }
protected:
    VisivoOpenXRInteractor() = default;
    ~VisivoOpenXRInteractor() override = default;
private:
    VisivoOpenXRInteractor(const VisivoOpenXRInteractor &) = delete;
    void operator=(const VisivoOpenXRInteractor &) = delete;
};

// Interactor style without thumbstick locomotion. The stock style maps the
// right thumbstick to either FLY_STYLE (Dolly3D) or GROUNDED_STYLE
// (GroundMovement3D) — both translate ``PhysicalTranslation`` by units
// proportional to ``PhysicalScale``. For a typical data cube
// (physicalScale ~225 in our case) even a slow thumbstick push displaces the
// camera by hundreds of world units per second, putting the user outside the
// volume's renderable region and causing the "screen goes dark" symptom.
// The user can still grab the world / volume with the grip buttons (grab mode
// from the OpenXR system menu), which moves things proportionally to physical
// motion and behaves intuitively. No-op'ing OnViewerMovement3D leaves all
// other interactions (grab, select, menu) untouched.
class VisivoOpenXRInteractorStyle : public vtkOpenXRInteractorStyle
{
public:
    static VisivoOpenXRInteractorStyle *New() { return new VisivoOpenXRInteractorStyle; }
    vtkTypeMacro(VisivoOpenXRInteractorStyle, vtkOpenXRInteractorStyle);
    void OnViewerMovement3D(vtkEventData *) override { /* disabled */ }
    void OnElevation3D(vtkEventData *) override { /* disabled */ }
protected:
    VisivoOpenXRInteractorStyle() = default;
    ~VisivoOpenXRInteractorStyle() override = default;
private:
    VisivoOpenXRInteractorStyle(const VisivoOpenXRInteractorStyle &) = delete;
    void operator=(const VisivoOpenXRInteractorStyle &) = delete;
};

struct CubeVRController::Impl
{
    vtkSmartPointer<vtkOpenXRRenderWindow>            renderWindow;
    vtkSmartPointer<VisivoOpenXRInteractor>           interactor;
    // Must be the OpenXR subclass, not the plain factory default. The render
    // window's AddRenderer() rejects vtkOpenGLRenderer with a stern message
    // ("A vtkOpenXRRenderer is expected") and leaves the scene unattached.
    vtkSmartPointer<vtkOpenXRRenderer>                vrRenderer;
    // Placeholder controller markers — index 0 = left hand, 1 = right hand.
    // Used until proper glTF controller models are wired up via
    // VISIVO_OPENXR_CONTROLLER_MODELS_DIR.
    vtkSmartPointer<vtkActor>                         controllerActors[2];
    // Bounds outline that mirrors the volume's pose. We don't share the
    // volume's prop3D state directly — grab interaction reads/writes the
    // *volume's* transform, and we just stamp the outline with the volume's
    // composed matrix each frame so they translate / rotate / scale together.
    vtkSmartPointer<vtkActor>                         outlineActor;
    vtkVolume                                        *volume{ nullptr };  // not owning
    // Drives one OpenXR frame per Qt event-loop tick. Started after open(),
    // stopped by close() or by the session ending from inside the HMD.
    QTimer                                            frameTimer;
    bool active{ false };
    // Latches true once the runtime actually transitions to RUNNING (a few
    // frames after Initialize() — see vtkOpenXRRenderWindowInteractor's
    // XR_SESSION_STATE_READY → BeginSession path). Used so the per-tick
    // "session went away" check only fires *after* it had gone up, otherwise
    // we'd close ourselves immediately on the first tick.
    bool sessionWasRunning{ false };

    // vtkCallbackCommand-compatible per-frame pose refresh. Static so it can
    // be bound as a C function pointer; receives the Impl* via clientData.
    // Defined here (rather than in an anonymous namespace) because Impl is a
    // private nested type — only a member function can see its layout.
    static void onRendererStart(vtkObject *, unsigned long, void *clientData, void *);
};
#else
struct CubeVRController::Impl
{
    // Empty placeholder for the no-VR build. Keeps `d` pointers semantically
    // valid (== nullptr) without forward-declaration headaches.
};
#endif

#ifdef VISIVO_HAS_VR
// Per-frame pre-render hook. Bound to the VR renderer's StartEvent so it
// fires right after vtkOpenXRRenderWindow::UpdateHMDMatrixPose() but before
// the actual draw. Two responsibilities, both depending on the just-updated
// camera and controller poses:
//
//   1. Position the placeholder controller actors. We stamp each one's
//      UserMatrix with the current device-to-world transform; if the runtime
//      can't see a hand (controller off / not yet tracked) we hide the actor
//      instead of leaving it at the origin.
//
//   2. Re-run vtkVRRenderer::ResetCameraClippingRange(). VTK's VR-aware
//      logic — 0.2 m near, farthest-bound + 3 m slack (min 10 m) far —
//      otherwise only runs on thumbstick locomotion. Physical head/body
//      movement updates the camera each frame but leaves the clipping range
//      frozen at its open()-time value, which becomes wrong as the user
//      walks around and can cause the volume to clip out. Re-resetting per
//      frame keeps the depth window tight around the *current* scene with
//      good depth-buffer precision.
void CubeVRController::Impl::onRendererStart(
    vtkObject *, unsigned long, void *clientData, void *)
{
    auto *impl = static_cast<Impl *>(clientData);
    if (!impl || !impl->renderWindow || !impl->vrRenderer)
        return;

    static constexpr vtkEventDataDevice kHands[2] = {
        vtkEventDataDevice::LeftController,
        vtkEventDataDevice::RightController,
    };
    vtkNew<vtkMatrix4x4> mat;
    for (int i = 0; i < 2; ++i) {
        if (!impl->controllerActors[i])
            continue;
        const bool ok = impl->renderWindow->GetDeviceToWorldMatrixForDevice(kHands[i], mat);
        if (ok) {
            impl->controllerActors[i]->SetUserMatrix(mat);
            impl->controllerActors[i]->SetVisibility(true);
        } else {
            impl->controllerActors[i]->SetVisibility(false);
        }
    }

    // Drag-along outline. The volume's composed model-to-world matrix
    // includes the grab-mode translation / rotation / scale that the VR
    // interactor style stamps onto the prop. Mirroring it onto the outline
    // keeps the two visually locked together regardless of how the user
    // pushes or twists the volume around.
    if (impl->volume && impl->outlineActor) {
        vtkNew<vtkMatrix4x4> volMat;
        impl->volume->GetMatrix(volMat);
        impl->outlineActor->SetUserMatrix(volMat);
    }

    impl->vrRenderer->ResetCameraClippingRange();
}
#endif

// ─────────────────────────────────────────────────────────────────────────────

CubeVRController::CubeVRController(QObject *parent)
    : QObject(parent)
{
}

CubeVRController::~CubeVRController()
{
    close();
}

bool CubeVRController::isCompiledIn()
{
#ifdef VISIVO_HAS_VR
    return true;
#else
    return false;
#endif
}

bool CubeVRController::isRuntimeAvailable()
{
#ifdef VISIVO_HAS_VR
    // We used to probe by creating a vtkOpenXRRenderWindow and calling
    // Initialize/Finalize, then caching the result. In practice this is
    // actively harmful: the Oculus / SteamVR loaders enforce a single
    // xrInstance per process, and their teardown after the probe leaks
    // enough state that a subsequent open() fails with
    // XR_ERROR_LIMIT_REACHED ("Loader does not support simultaneous
    // XrInstances"). The probe also can't tell "no HMD connected" from
    // "runtime not installed" — Initialize succeeds for the OpenGL side
    // either way and the form-factor check only fails later.
    //
    // Cheaper and more honest: assume the runtime is reachable whenever
    // VR support is compiled in. open() will do the real attempt and
    // surface a clean error if there's no HMD or no runtime.
    return true;
#else
    return false;
#endif
}

bool CubeVRController::open(vtkRenderer *desktopRenderer, vtkVolume *cubeVolume)
{
#ifdef VISIVO_HAS_VR
    if (!desktopRenderer || !cubeVolume) {
        m_lastError = u"Internal error: missing renderer or volume."_s;
        return false;
    }
    if (!d) {
        d = new Impl();
    }
    if (d->active) {
        // Already running — re-raising is sufficient.
        return true;
    }

    d->renderWindow = vtkSmartPointer<vtkOpenXRRenderWindow>::New();
    // 3D controller model assets (glTF + JSON manifest) are looked up during
    // Initialize(), so the directory must be set BEFORE the call. Empty path
    // = no controller models rendered (the controllers still track and
    // trigger actions — they're just invisible in the headset).
#ifdef VISIVO_OPENXR_CONTROLLER_MODELS_DIR
    d->renderWindow->SetModelsManifestDirectory(VISIVO_OPENXR_CONTROLLER_MODELS_DIR);
#endif
    d->renderWindow->Initialize();

    // vtkOpenXRRenderWindow::Initialize() does NOT clear `Initialized` when
    // the OpenXR session creation fails (no HMD, runtime missing, form-factor
    // unavailable, etc.) — the OpenGL half is independent and reports success.
    // After Initialize() the XrSession handle is non-null iff CreateSession()
    // succeeded, so it's a reliable "did init reach a usable state" signal.
    // (Note: IsSessionRunning() is NOT usable here — BeginSession() is only
    // called later, by the interactor in response to the XR_SESSION_STATE_READY
    // event, so it's still false on the happy path at this point.)
    //
    // If the session wasn't created we MUST bail before constructing the
    // interactor: interactor->Initialize() calls renderWindow->Initialize()
    // a second time, which trips the loader's single-XrInstance rule
    // (XR_ERROR_LIMIT_REACHED) and any subsequent Render() then blocks
    // against a null XrSession.
    //
    // Finalize() is also required even on the failure path: a partially-built
    // XrInstance is left behind by the failed Initialize(), and only Finalize()
    // releases it so the *next* open() attempt isn't poisoned.
    if (vtkOpenXRManager::GetInstance().GetSession() == XR_NULL_HANDLE) {
        m_lastError = u"Could not start an OpenXR session. Make sure an HMD is "
                      u"connected and the OpenXR runtime (SteamVR / Oculus / "
                      u"WMR) is running, then try again."_s;
        if (d->renderWindow->GetInitialized()) {
            d->renderWindow->Finalize();
        }
        d->renderWindow = nullptr;
        return false;
    }

    // Build the OpenXR-flavoured renderer (not the factory default, which is
    // a plain vtkOpenGLRenderer and is rejected by vtkOpenXRRenderWindow).
    d->vrRenderer = vtkSmartPointer<vtkOpenXRRenderer>::New();
    // Share the volume actor — its mapper is the same one the desktop cube
    // viewer mutates when the user changes the LUT / threshold / opacity TF,
    // so those edits become visible in the HMD on the next frame "for free".
    d->vrRenderer->AddVolume(cubeVolume);
    d->vrRenderer->SetBackground(0.04, 0.05, 0.08);
    d->volume = cubeVolume;

    // Wireframe outline of the volume bounds. Gives the user a visible "edge"
    // in VR so they can tell where the cube ends even when the transfer
    // function leaves a region transparent. Built from the live volume bounds
    // (an explicit vtkOutlineSource instead of vtkOutlineFilter so we don't
    // touch the mapper's input pipeline — the volume's GPU mapper holds the
    // dataset reference and we don't want to extract it here). The outline's
    // UserMatrix is updated each frame from cubeVolume->GetMatrix() so that
    // grab-mode translation/rotation of the volume drags the outline with it.
    // PickableOff so the outline itself is never the picked prop — grab
    // always picks the underlying volume.
    double bounds[6];
    cubeVolume->GetBounds(bounds);
    vtkNew<vtkOutlineSource> outlineSource;
    outlineSource->SetBounds(bounds);
    vtkNew<vtkPolyDataMapper> outlineMapper;
    outlineMapper->SetInputConnection(outlineSource->GetOutputPort());
    d->outlineActor = vtkSmartPointer<vtkActor>::New();
    d->outlineActor->SetMapper(outlineMapper);
    d->outlineActor->GetProperty()->SetColor(0.85, 0.85, 0.95);
    d->outlineActor->GetProperty()->SetLineWidth(2.0);
    d->outlineActor->GetProperty()->SetLighting(false);
    d->outlineActor->PickableOff();
    d->vrRenderer->AddActor(d->outlineActor);

    // Placeholder controller markers. Two coloured spheres — cyan for the left
    // hand, magenta for the right — sized roughly to a Touch controller's grip
    // (3 cm radius). Real glTF models from VTK's controller-model machinery
    // (SetModelsManifestDirectory, configured via VISIVO_OPENXR_CONTROLLER_MODELS_DIR)
    // will replace these once assets are provided. The actors start invisible
    // and are revealed by the per-frame pose callback once the runtime reports
    // a valid pose for each hand.
    for (int i = 0; i < 2; ++i) {
        vtkNew<vtkSphereSource> sphere;
        sphere->SetRadius(0.03);
        sphere->SetPhiResolution(16);
        sphere->SetThetaResolution(16);
        vtkNew<vtkPolyDataMapper> sphereMapper;
        sphereMapper->SetInputConnection(sphere->GetOutputPort());
        d->controllerActors[i] = vtkSmartPointer<vtkActor>::New();
        d->controllerActors[i]->SetMapper(sphereMapper);
        d->controllerActors[i]->GetProperty()->SetColor(
            i == 0 ? 0.2 : 0.9,  // R: left=dim, right=bright
            i == 0 ? 0.8 : 0.2,  // G: left=bright, right=dim
            0.9);                // B: both blueish
        d->controllerActors[i]->SetVisibility(false);
        d->vrRenderer->AddActor(d->controllerActors[i]);
    }
    // Per-frame hook: pose-track the placeholder controllers AND re-run the
    // VR-aware clipping-range reset. Attached to the *renderer's* StartEvent
    // so it fires after UpdateHMDMatrixPose() but before the actual draw —
    // exactly when both the camera and the device poses are current.
    vtkNew<vtkCallbackCommand> startCb;
    startCb->SetClientData(d);
    startCb->SetCallback(&Impl::onRendererStart);
    d->vrRenderer->AddObserver(vtkCommand::StartEvent, startCb);

    d->renderWindow->AddRenderer(d->vrRenderer);

    d->interactor = vtkSmartPointer<VisivoOpenXRInteractor>::New();
    d->interactor->SetRenderWindow(d->renderWindow);
    // VTK's default ActionManifestDirectory is the empty string, so it looks
    // for vtk_openxr_actions.json in the process's CWD — which is wherever the
    // user launched us from, not where VTK installed the manifest. Without the
    // manifest the interactor logs "Failed to load actions" and never calls
    // BeginSession(), so the HMD stays blank and Render() fails with
    // XR_ERROR_SESSION_NOT_RUNNING. The path is resolved at configure time by
    // CMake (see VISIVO_VTK_OPENXR_ACTIONS_DIR). The trailing slash matters:
    // the interactor concatenates directory + filename without a separator.
#ifdef VISIVO_VTK_OPENXR_ACTIONS_DIR
    d->interactor->SetActionManifestDirectory(VISIVO_VTK_OPENXR_ACTIONS_DIR);
#endif
    // Install our subclassed interactor style BEFORE Initialize() so the
    // base class doesn't fully wire up the stock one we'd then have to
    // displace. The custom style is identical to vtkOpenXRInteractorStyle
    // except its thumbstick handlers (OnViewerMovement3D / OnElevation3D)
    // are no-ops — see the comment on VisivoOpenXRInteractorStyle for why
    // stock locomotion broke rendering on data cubes. Grab / select / menu
    // / clip / probe all still work via the unchanged inherited handlers.
    vtkNew<VisivoOpenXRInteractorStyle> visivoStyle;
    d->interactor->SetInteractorStyle(visivoStyle);

    d->interactor->Initialize();

    // Take over the event loop. Without this, ``Done`` stays at its
    // constructor-default ``true`` and every ``DoOneEvent`` call from the
    // QTimer returns immediately — exactly the "headset is on but nothing
    // is rendered" failure mode.
    d->interactor->EnableManualEventLoop();

    d->vrRenderer->ResetCamera();

    // Diagnostic dump. With astronomical FITS cubes the volume bounds can be
    // enormous (10^6+) or off-origin in ways that wreck VR depth precision.
    // Print enough state to correlate "the volume vanished after I moved"
    // with the actual camera / scale / clipping numbers.
    {
        double physScale = d->renderWindow->GetPhysicalScale();
        double physTrans[3] = { 0, 0, 0 };
        d->renderWindow->GetPhysicalTranslation(physTrans);
        double clip[2] = { 0, 0 };
        double camPos[3] = { 0, 0, 0 };
        if (auto *cam = d->vrRenderer->GetActiveCamera()) {
            cam->GetClippingRange(clip);
            cam->GetPosition(camPos);
        }
        qInfo().nospace()
            << "[vr] volume bounds = ["
            << bounds[0] << ".." << bounds[1] << ", "
            << bounds[2] << ".." << bounds[3] << ", "
            << bounds[4] << ".." << bounds[5] << "]";
        qInfo().nospace()
            << "[vr] physicalScale = " << physScale
            << "   physicalTranslation = ("
            << physTrans[0] << ", " << physTrans[1] << ", " << physTrans[2] << ")";
        qInfo().nospace()
            << "[vr] initial camera = ("
            << camPos[0] << ", " << camPos[1] << ", " << camPos[2] << ")"
            << "   clippingRange = [" << clip[0] << ", " << clip[1] << "]";
    }

    // Drive the OpenXR event loop from a QTimer instead of the blocking
    // Start(). Each tick calls DoOneEvent(), which is exactly one iteration
    // of what Start() does internally: poll XR events, sync actions, run a
    // frame. WaitAndBeginFrame() inside Render() blocks for ~11 ms at 90 Hz,
    // releasing the CPU back to OpenXR — Qt's event loop picks up UI events
    // between ticks, so the desktop stays interactive and the user can press
    // the same button again (or any other UI affordance) to stop VR.
    //
    // We also self-detect session exits from *inside* the headset (system
    // menu → Exit, or HMD removed long enough that the runtime tears down
    // the session). When IsSessionRunning() flips back to false after having
    // been true, we trigger close() via a queued single-shot so the QTimer
    // signal-emit unwinds cleanly before the timer (a member of Impl) is
    // destroyed.
    QObject::connect(&d->frameTimer, &QTimer::timeout, this, [this]() {
        if (!d || !d->active || !d->interactor || !d->renderWindow)
            return;
        d->interactor->DoOneEvent(d->renderWindow, d->vrRenderer);
        if (d->sessionWasRunning
            && !vtkOpenXRManager::GetInstance().IsSessionRunning()) {
            QTimer::singleShot(0, this, [this]() { this->close(); });
        } else if (vtkOpenXRManager::GetInstance().IsSessionRunning()) {
            d->sessionWasRunning = true;
        }
    });
    d->frameTimer.setInterval(0); // fire as fast as possible — OpenXR paces us
    d->active = true;
    qInfo() << "[vr] OpenXR session started, pumping frames via QTimer";
    d->frameTimer.start();
    emit sessionStarted();
    return true;
#else
    Q_UNUSED(desktopRenderer);
    Q_UNUSED(cubeVolume);
    m_lastError = u"VR support is not compiled in. Rebuild with -DVISIVO_ENABLE_VR=ON "
                  u"(requires VTK built with VTK_MODULE_ENABLE_VTK_RenderingOpenXR=YES)."_s;
    return false;
#endif
}

void CubeVRController::close()
{
#ifdef VISIVO_HAS_VR
    if (!d) return;
    const bool wasActive = d->active;
    d->active = false;          // stop the QTimer slot from doing any more work
    d->frameTimer.stop();
    if (d->interactor) {
        d->interactor->TerminateApp();
    }
    if (d->renderWindow && d->renderWindow->GetInitialized()) {
        d->renderWindow->Finalize();
    }
    d->interactor   = nullptr;
    d->vrRenderer   = nullptr;
    d->renderWindow = nullptr;
    delete d;
    d = nullptr;
    if (wasActive) {
        qInfo() << "[vr] OpenXR session ended";
        emit sessionEnded();
    }
#endif
}

bool CubeVRController::isActive() const
{
#ifdef VISIVO_HAS_VR
    return d && d->active;
#else
    return false;
#endif
}
