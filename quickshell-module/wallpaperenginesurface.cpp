// glew before any Qt/GL header.
#include <GL/glew.h>

#include "wallpaperenginesurface.hpp"
#include "wethread.hpp"

#include <EGL/egl.h>

#include <algorithm>
#include <string>
#include <vector>

#include <qcoreapplication.h>
#include <qopenglcontext.h>
#include <qopenglcontext_platform.h>
#include <qquickwindow.h>
#include <qsgsimpletexturenode.h>
#include <qsgtexture.h>
#include <qsgtexture_platform.h>
#include <qthread.h>

namespace qs::wallpaperengine {

namespace {

std::string assetsDir() {
	return std::string(qgetenv("HOME").constData())
	    + "/.local/share/Steam/steamapps/common/wallpaper_engine/assets";
}

} // namespace

WallpaperEngineSurface::WallpaperEngineSurface(QQuickItem* parent): QQuickItem(parent) {
	this->setFlag(QQuickItem::ItemHasContents);
	// Repaint when the WE thread has actually produced a frame - not on a clock
	// of our own running at `fps`. Two clocks at nominally the same rate drift,
	// so a poll clock both repeats frames (a full-screen repaint, a full-surface
	// wl_surface commit and a compositor recomposite for a texture that has not
	// changed) and drops fresh ones. It also is not even the right rate:
	// 1000/fps is integer milliseconds, so fps=24 polled at 41ms is 24.4Hz,
	// fps=144 polls at 166Hz and fps=90 at 90.9Hz.
	//
	// The sink hop puts update() on the GUI thread rather than calling it from
	// inside WE's render loop or from updatePaintNode on the render thread, which
	// keeps the scene-graph sync race-free exactly as the old timer did.
	//
	// Deleted through a deleter rather than plainly, because the sink genuinely
	// outlives this item: a WE thread that ~WeThread gave up on and detached
	// holds a shared_ptr to it inside its onFrame callback, and when that thread
	// finally unwinds it is the last owner. Running ~QObject there would tear
	// down a QObject on a thread that does not own it - unhooking connections and
	// draining posted events belonging to the GUI thread's dispatcher from
	// underneath it. deleteLater() posts a deferred delete to the object's OWN
	// thread instead, which is the one QObject call that is safe from anywhere.
	//
	// The affinity check keeps the ordinary path exactly as it was: when the GUI
	// thread drops the last reference (this item being destroyed, thread already
	// joined), the sink dies right there. Going through deleteLater()
	// unconditionally would instead leave it pending, and a deferred delete still
	// queued when the event loop stops is dropped by ~QCoreApplication - leaking
	// the sink on every clean shutdown to fix a case only the detach path reaches.
	this->mFrameSink = std::shared_ptr<WeFrameSink>(new WeFrameSink, [](WeFrameSink* sink) {
		if (sink->thread() == QThread::currentThread()) {
			delete sink;
		} else {
			sink->deleteLater();
		}
	});
	QObject::connect(this->mFrameSink.get(), &WeFrameSink::frame, this, [this] {
		this->mFrameSincePoll = true;
		// `occluded` is deliberately absent from this test even though it stops
		// repaints, because the PRODUCER is what enforces it: an occluded WE thread
		// publishes nothing and so never gets here. The wake-ups that do still
		// arrive while occluded are exactly the ones that must not be dropped - the
		// one-shot the WE thread sends when it fails to start, which is how `failed`
		// reaches QML and the shell falls back to the static image, and at most one
		// racing frame from the iteration that straddled the flag.
		if (this->mLive && !this->mProjectPath.isEmpty()) this->update();
	});
	QObject::connect(&this->mStallPoll, &QTimer::timeout, this, [this] {
		if (this->mFrameSincePoll) {
			this->mFrameSincePoll = false;
			return;
		}
		this->update();
	});
	this->mStallPoll.setInterval(1000);
	this->updateFrameDriver();

	// Stop + join the WE thread while the event loop and render thread are still
	// alive. If we wait for the item destructor (Qt teardown), the scene-graph
	// render thread deadlocks against the still-running WE thread and the process
	// hangs on quit (SIGTERM appears ignored). aboutToQuit fires first, on the
	// GUI thread, so the join happens at a safe point.
	//
	// Unlike the rebuild paths in updatePaintNode, this one cannot drop the
	// scene-graph node first: only the render thread may touch a node, and nothing
	// guarantees another sync pass between here and teardown. So between the join
	// and the window going away the node still wraps a texture name run()'s
	// epilogue has just glDeleteTextures'd. Left alone deliberately: it is bounded
	// to the shutdown frames of a process that is quitting, a stale GL name samples
	// as garbage or black rather than faulting, and the alternative is reordering
	// teardown around the one path where there is nothing left to see it.
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { this->releaseThread(); });
}

void WallpaperEngineSurface::releaseThread() {
	if (!this->mThread) {
		this->mShareContext.reset();
		return;
	}

	// stop() bounds its join: a wallpaper wedged inside WE's setup() never
	// observes the stop flag, so after a few seconds the thread is DETACHED and
	// left running rather than freezing the shell. Detached does not mean gone,
	// and TWO things it is still using have to outlive this call.
	//
	// The EGLContext: it is still current on that thread, which still issues GL
	// through it. Destroying this QOpenGLContext would pull it out from under a
	// live thread mid-draw, in a context that shares its object namespace with
	// Qt's. That is a real mechanism for one bad wallpaper to damage every later
	// one.
	//
	// The WeThread object: the detached thread is inside WeThread::run(), a
	// MEMBER function that reads mStop/mFps/mScaleMode, locks mMutex, publishes
	// into mFrontTexture/mFrontFence and notifies through mOnFrame. Destroying
	// it would run those members' destructors under a live reader, and freeing
	// it would hand the address back to the allocator - and the very next thing
	// this class does is construct a REPLACEMENT WeThread of exactly that size,
	// which lands on that address as often as not. The dead wallpaper would then
	// publish its texture and its fence into the live one's state.
	//
	// So on the detach path, leak both, deliberately. That thread has already
	// stranded an OS thread, its mpv/SDL state and WE's own GL objects; a few
	// hundred more bytes, once per wedged wallpaper, is the cheap half of the
	// trade. The alternative is undefined behaviour in the shared group.
	if (!this->mThread->stop()) {
		qWarning(
		    "WallpaperEngineSurface: WE thread detached; leaking its GL context and thread "
		    "object deliberately"
		);
		(void) this->mThread.release();
		(void) this->mShareContext.release();
		return;
	}

	this->mThread.reset();
	this->mShareContext.reset();
}

void WallpaperEngineSurface::updateFrameDriver() {
	// live=false freezes on the current frame: the producer keeps running (WE has
	// no pause that survives a resume cleanly) but its notifications stop
	// repainting us, so the surface holds the last texture it published and the
	// window stops committing entirely.
	//
	// occluded=true is the other half of that. There the producer DOES throttle
	// itself (WeThread::setOccluded), so it stops publishing and therefore stops
	// waking us - and the watchdog below has to stand down for it, or it would do
	// precisely what it is built to do: notice the silence and force a repaint a
	// second, so a wallpaper nobody can see would go on committing at 1Hz for the
	// length of a game session. It is update() that costs that commit and not
	// updatePaintNode: a scheduled update makes Qt render and swap the window
	// whether or not the node it produces has changed.
	//
	// But it stands down only while there is a producer to be quiet, which is what
	// mThread is being tested for. With no thread there is nothing left that can
	// ever wake this item again - no frame post through the sink, and no node to
	// mark dirty - so gating on the flag alone stranded the two share-context /
	// EGL-handle failure paths in updatePaintNode, which leave exactly that state
	// and say in their own comments that the watchdog brings them back. The shell
	// that reaches it is the one INTEGRATION.md now tells people to write: a
	// surface that comes up with `occluded` already true because a game is
	// fullscreen. A transient context-creation failure there was retried only when
	// the user closed the game, with `rendered` and `failed` both sitting false in
	// the meantime over a wallpaper that had never started. A retry that commits
	// at 1Hz over a covered output is the price of having a retry at all, and it
	// lasts only until a producer exists: the rebuild block in updatePaintNode
	// posts this call back to us precisely so that re-evaluation happens.
	const bool producerIsIdle = this->mOccluded && this->mThread != nullptr;
	if (this->mLive && !producerIsIdle && !this->mProjectPath.isEmpty()) {
		this->mFrameSincePoll = false;
		if (!this->mStallPoll.isActive()) this->mStallPoll.start();
	} else {
		this->mStallPoll.stop();
	}
}

// aboutToQuit normally gets here first, but an item destroyed on its own (the
// Loader going inactive, a monitor disappearing) still has to go through
// releaseThread rather than the members' declaration order, so that a detached
// thread's context is leaked rather than destroyed under it.
WallpaperEngineSurface::~WallpaperEngineSurface() { this->releaseThread(); }

QSGNode* WallpaperEngineSurface::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* /*data*/) {
	auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);

	const int w = static_cast<int>(this->width());
	const int h = static_cast<int>(this->height());
	if (w <= 0 || h <= 0 || this->mProjectPath.isEmpty()) {
		delete node;
		return nullptr;
	}

	// Runs on the render thread with Qt's GL context current. Build a share
	// context via Qt (so its config + robustness flags match Qt's - raw
	// eglCreateContext sharing fails EGL_BAD_MATCH on NVIDIA), then hand its
	// native EGLContext to the WE thread for a surfaceless makeCurrent.
	//
	// Share against the process-global share context when there is one
	// (AA_ShareOpenGLContexts, set by the patched main.cpp): every scene-graph
	// context Qt creates joins that share group, so WE's textures stay valid
	// across the compositor-forced scene-graph context recreations Hyprland's
	// fullscreen direct-scanout causes - no rebuild, no black-out. A game
	// session can force several recreations a minute; rebuilding the WE thread
	// for each one is what used to wedge video wallpapers into permanent black
	// (mpv teardown under GPU contention overran the join deadline, the old
	// thread detached, and two WE instances then fought over process globals).
	//
	// Without a global share context (stock build), fall back to sharing with
	// the current SG context and rebuilding when it dies. Pointer inequality
	// alone is not enough for that: a destroy+recreate can hand the new
	// QOpenGLContext the old one's heap address, so mContextLost (latched by
	// aboutToBeDestroyed on the adopted context) is the authoritative signal.
	auto* qtCtx = QOpenGLContext::currentContext();
	auto* shareTarget = QOpenGLContext::globalShareContext();
	if (!shareTarget) shareTarget = qtCtx;
	const bool contextChanged =
	    this->mThread && (this->mLoadedContext != shareTarget || this->mContextLost);
	if (contextChanged) qInfo("WallpaperEngineSurface: GL context changed; rebuilding WE thread");
	if (!this->mThread || this->mLoadedPath != this->mProjectPath || contextChanged) {
		// Drop the node before the thread that owns its texture goes away - on
		// EVERY rebuild path, not just the context-change one. The node holds a
		// bare GL texture NAME. After a context change that name belongs to a
		// destroyed share group. After an ordinary project switch - or a scaleMode
		// / audioEnabled reload, which reach here by clearing mLoadedPath - it is
		// worse than dangling: releaseThread joins the WE thread, whose epilogue
		// destroys its render targets, so glDeleteTextures frees the name inside
		// the share group Qt is still drawing from and the next glGenTextures
		// hands it straight back out. The likeliest claimant is the incoming WE
		// thread's own targets[0], since glGenTextures returns the lowest free
		// name - so the surface samples an allocated-but-never-written texture -
		// and the other is any Qt atlas or layer texture created in the meantime,
		// which is the "wallpaper draws a widget's cached layer, fullscreen"
		// symptom the context-change branch was written to prevent. Keeping the
		// node would draw that on every window repaint until the new thread
		// publishes its first frame, which for a video wallpaper is the whole of
		// WE's setup() plus mpv start-up - seconds, not milliseconds - because
		// acquireTexture returns 0 until then and the tail of this function hands
		// the untouched node straight back. A black-out until the first new frame
		// is the correct degradation.
		delete node;
		node = nullptr;
		this->releaseThread();

		// The load that was on screen is over, so retire its generation and take
		// back the two verdicts QML holds about it. `failed` was only ever cleared
		// in setProjectPath, but three other things rebuild the thread - a lost GL
		// context (Hyprland's fullscreen direct-scanout can force several a
		// minute), a scaleMode change and an audioEnabled toggle - so a wallpaper
		// that hit a transient failure once (the VRAM shortfall
		// RenderTarget::create() catches, which by its nature depends on what else
		// is resident) and then reloaded perfectly stayed `failed` for the rest of
		// the session, with the shell parked on the static image in front of a
		// working live wallpaper. WeThread::failed() is a latch that is only ever
		// polled, so there is no recovery edge to observe: the rebuild is the
		// edge, and each attempt gets its own verdict. `rendered` gets the same
		// treatment for the mirror reason - the node is gone and mLoadFrameSeen is
		// re-armed below, so leaving it true tells QML to start a transition
		// against the black screen this rebuild just created, the one thing that
		// property exists to prevent.
		//
		// Retired here rather than further down because the two early returns in
		// between also end the old load without starting a new one; doing it at the
		// top means those paths leave a coherent state - no thread, no node,
		// rendered false, failed false - instead of `rendered` true over nothing.
		// The mLoadFrameSeen/mFailSeen latches are re-armed further down instead,
		// off this bump: an early return cannot misuse a stale one (neither latch
		// is reachable without mThread), and doing it off the generation is what
		// covers the bumps that never get as far as a rebuild.
		//
		// Both flags are GUI-thread state behind change signals and this runs on
		// the render thread with the GUI thread blocked in sync, so they go across
		// queued, exactly like the two verdicts posted further down.
		const auto generation = this->mLoadGeneration.fetch_add(1) + 1;
		QMetaObject::invokeMethod(
		    this,
		    [this, generation] {
			    if (this->mLoadGeneration.load() != generation) return;
			    if (this->mRendered) {
				    this->mRendered = false;
				    emit this->renderedChanged();
			    }
			    if (this->mFailed) {
				    this->mFailed = false;
				    emit this->failedChanged();
			    }
		    },
		    Qt::QueuedConnection
		);

		// Whether this pass ends with a producer or without one is the other half
		// of what updateFrameDriver() has to know, and this block is the only place
		// that changes: the two failure returns below leave mThread null, the tail
		// of the block leaves it live. Posted rather than called, because mStallPoll
		// is a QTimer and belongs to the GUI thread - which is blocked in the sync
		// right now, so the post lands after this pass has returned, which is
		// exactly when mThread holds its final value for the pass. Unconditional,
		// and deliberately not folded into the generation-guarded post above: a
		// superseded load still owes the watchdog the truth about whether anything
		// is left running.
		QMetaObject::invokeMethod(
		    this, [this] { this->updateFrameDriver(); }, Qt::QueuedConnection
		);

		auto share = std::make_unique<QOpenGLContext>();
		share->setFormat(qtCtx->format());
		share->setShareContext(shareTarget);
		if (!share->create() || !share->shareContext()) {
			qWarning("WallpaperEngineSurface: failed to create shared GL context");
			// The node was dropped above and there is no thread left to publish a
			// replacement, so this pass genuinely has nothing to show. Spelled
			// nullptr rather than `node` - which is the same value today - so that
			// moving the drop later can never quietly turn this back into
			// "returns the previous load's deleted texture". The stall watchdog
			// brings us back in a second to retry - including on a covered output,
			// which is why updateFrameDriver() stands the watchdog down for a live
			// producer rather than for the occluded flag: this state has no
			// producer, so the watchdog is the only thing left that can.
			return nullptr;
		}

		auto* eglCtx = share->nativeInterface<QNativeInterface::QEGLContext>();
		auto dpy = eglGetCurrentDisplay();
		if (!eglCtx || dpy == EGL_NO_DISPLAY) {
			qWarning("WallpaperEngineSurface: no EGL context handle available");
			return nullptr; // same as above: nothing left to draw, nothing to draw it
		}

		// Adopting a (possibly new) share target: latch its destruction so the
		// next updatePaintNode rebuilds even if a replacement reuses this heap
		// address. The global share context lives until app shutdown, so with
		// it this effectively never fires; the latch matters for the
		// fallback-to-SG-context case. aboutToBeDestroyed fires on the
		// context's owning thread, so a plain bool is safe for the SG-context
		// case (render thread, same as this code). Reconnecting on a
		// same-context project switch would stack duplicate connections, so
		// only connect when the target is actually new.
		//
		// The latch is paired with a wake-up because context loss is an EDGE and
		// nothing else reports it. Until now the stall watchdog was what eventually
		// noticed, which stopped being true the moment the watchdog learned to stand
		// down for an occluded output - and a covered output is exactly when this
		// fires, since the compositor behaviour that destroys scene-graph contexts
		// several times a minute IS a fullscreen game taking direct scanout. The
		// wallpaper would then stay dead for the length of the game and reload in
		// the seconds after it closed, in front of the user, which is the worst
		// possible moment for a WE reload. One queued update() per loss rebuilds it
		// while nobody is looking and costs nothing when nothing is lost.
		if (this->mLoadedContext != shareTarget || this->mContextLost) {
			QObject::connect(shareTarget, &QOpenGLContext::aboutToBeDestroyed, this, [this] {
				this->mContextLost = true;
				QMetaObject::invokeMethod(this, [this] { this->update(); }, Qt::QueuedConnection);
			}, Qt::DirectConnection);
		}
		this->mContextLost = false;
		this->mShareContext = std::move(share);
		this->mLoadedPath = this->mProjectPath;
		this->mLoadedContext = shareTarget;
		// mLoadFrameSeen/mFailSeen are deliberately NOT re-armed here. They are
		// re-armed against the generation further down instead, which covers this
		// rebuild and the bumps that never reach one - see the comment there.
		//
		// The callback holds its own shared_ptr to the sink, so the sink survives
		// for as long as the thread can still call it - including a thread that
		// outlived this item by being detached.
		// The property map becomes WE's own "name=value" strings here, sorted by
		// key: QVariantMap iterates in key order already, and spelling the sort
		// guarantee out is what keeps the same map always building the same argv
		// (the header's contract). Reading these GUI-thread members from the
		// render thread is safe for the reason mFps and mAudioEnabled always
		// were: the GUI thread is blocked for the scene-graph sync.
		std::vector<std::string> setProperties;
		setProperties.reserve(static_cast<std::size_t>(this->mProperties.size()));
		for (auto it = this->mProperties.constBegin(); it != this->mProperties.constEnd(); ++it) {
			setProperties.push_back(
			    it.key().toStdString() + "=" + it.value().toString().toStdString()
			);
		}
		this->mThread = std::make_unique<WeThread>(
		    dpy, eglCtx->nativeContext(), this->mProjectPath.toStdString(), assetsDir(), w, h,
		    this->mFps, this->mScaleMode.toStdString(), this->mAudioEnabled,
		    // 0..100 -> WE's 0..128; rounding at the boundary so 100 is exactly
		    // the old fixed 128.
		    (this->mVolume * 128 + 50) / 100, this->mAudioProcessing, this->mMouseDisabled,
		    this->mParallaxDisabled, this->mParticlesDisabled, std::move(setProperties),
		    [sink = this->mFrameSink] { sink->post(); }
		);
		// A fresh thread starts unoccluded, which is wrong whenever the shell
		// already knows better: a shell coming up with a game fullscreen, or a
		// wallpaper switched while the output is covered. Hand it the current state
		// here rather than adding a constructor argument, so the load path and the
		// live toggle stay one mechanism. Reading mOccluded from the render thread
		// is safe for exactly the reason reading mFps and mAudioEnabled above is:
		// the GUI thread is blocked for the scene-graph sync.
		this->mThread->setOccluded(this->mOccluded);
		// A fresh thread starts centre-cropped; hand it the stored focus on
		// the load path so a wallpaper that comes up with a non-centre crop
		// (a per-project focus restored from the store) starts where it was
		// left rather than snapping from centre on the first frame. Same
		// reason and same safety as setOccluded just above.
		this->mThread->setFocus(static_cast<float>(this->mFocusX), static_cast<float>(this->mFocusY));
	}

	// Which load the two verdicts below are talking about. They are posted from
	// here, on the render thread with the GUI thread blocked in sync, so they land
	// BEHIND everything already queued on the GUI thread - including a projectPath
	// write that has not run yet - and have to be able to disown themselves on
	// arrival. Read once, so both posts in a pass agree even though only one of
	// them can happen.
	const auto generation = this->mLoadGeneration.load();

	// Re-arm the two per-load latches against the GENERATION rather than against
	// the rebuild that usually accompanies one. Tying them to the rebuild left a
	// bump that provably never reaches it: a projectPath that goes empty and comes
	// straight back - a shell blanking the wallpaper, a model emptying for a tick,
	// a Loader swap, or literally `projectPath = ""; projectPath = same` - bumps
	// twice, and the pass in between takes the empty-path early return at the top
	// of this function. mLoadedPath therefore never stops matching, no rebuild ever
	// happens, and mLoadFrameSeen stays set from the load before: no `rendered`
	// post is made again for the rest of the session, so the shell sits on its
	// static fallback in front of a wallpaper that is rendering perfectly. The
	// mirror is worse - mFailSeen stays set too, so a project that had already
	// failed never re-reports it and `failed` stays false over a black surface.
	//
	// Comparing generations makes the invariant true by construction instead of by
	// argument: every bump gets its own verdict, rebuild or no rebuild. Costs one
	// integer compare per pass, and mAckedGeneration is render-thread-only state
	// like the latches it guards.
	if (this->mAckedGeneration != generation) {
		this->mAckedGeneration = generation;
		this->mLoadFrameSeen = false;
		this->mFailSeen = false;
	}

	// The WE thread gave up on this project. Report it once, on the GUI thread,
	// so QML can put the static wallpaper back; and drop the node, because
	// whatever it is holding is either nothing or a texture that will never be
	// written to again. Without this the surface sits there publishing an
	// unwritten texture and the desktop is simply black, with `rendered` true and
	// nothing in the log to act on.
	if (this->mThread && this->mThread->failed() && !this->mFailSeen) {
		this->mFailSeen = true;
		delete node;
		node = nullptr;
		QMetaObject::invokeMethod(
		    this,
		    [this, generation] {
			    // A projectPath write queued ahead of this call has already moved
			    // us to another wallpaper and cleared the flag. This verdict
			    // belongs to the wallpaper we just left; applying it would mark the
			    // new project failed before it had even tried to load, and until
			    // this fix nothing would ever have cleared it again.
			    if (this->mLoadGeneration.load() != generation) return;
			    if (!this->mFailed) {
				    this->mFailed = true;
				    emit this->failedChanged();
			    }
		    },
		    Qt::QueuedConnection
		);
		return nullptr;
	}

	GLuint texId = this->mThread ? this->mThread->acquireTexture() : 0;
	if (texId == 0) return node; // nothing ready yet; the first publish wakes us

	// First real frame of this project: tell QML (on the GUI thread) so a
	// wallpaper transition can start against actual content, not a black frame.
	if (!this->mLoadFrameSeen) {
		this->mLoadFrameSeen = true;
		QMetaObject::invokeMethod(
		    this,
		    [this, generation] {
			    // Stale exactly like the failure verdict above, and the mirror of
			    // it: setProjectPath clears `rendered` for the incoming project,
			    // and this would put it straight back for a project that has not
			    // drawn a pixel - which is QML starting its wallpaper transition
			    // against a black frame, the failure this property exists to
			    // prevent.
			    if (this->mLoadGeneration.load() != generation) return;
			    if (!this->mRendered) {
				    this->mRendered = true;
				    emit this->renderedChanged();
			    }
		    },
		    Qt::QueuedConnection
		);
	}

	// Mirror the wallpaper's content size out to the QML property. The WE
	// thread publishes it from the blit; comparing here and posting the
	// change turns a per-frame atomic into a single property notification the
	// crop picker can bind to. Guarded on a real change so it fires once when
	// the scene's size settles, not every frame.
	{
		const int cw = this->mThread->contentWidth();
		const int ch = this->mThread->contentHeight();
		if (cw != this->mContentWidth || ch != this->mContentHeight) {
			QMetaObject::invokeMethod(
			    this,
			    [this, cw, ch] {
				    if (cw == this->mContentWidth && ch == this->mContentHeight) return;
				    this->mContentWidth = cw;
				    this->mContentHeight = ch;
				    emit this->contentSizeChanged();
			    },
			    Qt::QueuedConnection
			);
		}
	}

	if (!node) {
		node = new QSGSimpleTextureNode();
		node->setOwnsTexture(true);
		node->setFiltering(QSGTexture::Linear);
		// WE's scene FBO is already oriented for GL sampling; no extra vertical
		// mirror (the driver-output path needed one, the scene-FBO path does not).
	}

	// Wrap WE's GL texture (valid in the shared context) as a scene-graph texture.
	// TextureIsOpaque: a wallpaper is the bottom layer - ignore WE's alpha so the
	// desktop behind the window never shows through where a scene composites with
	// alpha < 1.
	auto* qtTex = QNativeInterface::QSGOpenGLTexture::fromNative(
	    texId,
	    this->window(),
	    QSize(this->mThread->width(), this->mThread->height()),
	    QQuickWindow::TextureIsOpaque
	);
	node->setTexture(qtTex); // ownsTexture => deletes the previous wrapper
	node->setRect(0, 0, w, h);
	return node;
}

void WallpaperEngineSurface::setProjectPath(const QString& projectPath) {
	if (projectPath == this->mProjectPath) return;
	this->mProjectPath = projectPath;
	// Retire the running load here, on the GUI thread, and not only in
	// updatePaintNode's rebuild block: a verdict the render thread has already
	// posted about the old project is sitting in this thread's event queue right
	// now, and the rebuild that would retire it cannot run until this thread
	// reaches the render loop's sync point - which is after that verdict has been
	// delivered. Bumping here is what makes the stale verdict arrive on a
	// generation that is already gone, instead of marking the incoming project
	// failed (or rendered) before it has loaded. Before the emit, so no QML
	// handler - including one that re-enters this setter - can observe the new
	// path while the old load's generation is still current.
	this->mLoadGeneration.fetch_add(1);
	emit this->projectPathChanged();
	if (this->mRendered) {
		this->mRendered = false;
		emit this->renderedChanged();
	}
	// A new project gets a clean slate: the previous one failing says nothing
	// about this one, and leaving `failed` set would strand the shell on the
	// static image for the rest of the session.
	if (this->mFailed) {
		this->mFailed = false;
		emit this->failedChanged();
	}
	this->updateFrameDriver();
	this->update();
}

void WallpaperEngineSurface::setLive(bool live) {
	if (live == this->mLive) return;
	this->mLive = live;
	emit this->liveChanged();
	this->updateFrameDriver();
	this->update();
}

void WallpaperEngineSurface::setFps(int fps) {
	if (fps == this->mFps) return;
	this->mFps = fps;
	emit this->fpsChanged();
	// Nothing to do on this side: fps is the producer's rate, and the surface
	// follows the producer.
	if (this->mThread) this->mThread->setFps(fps);
}

// The reload dance every load-time WE argument shares. Retire the load's
// generation for the reason setProjectPath spells out - a verdict already
// posted about the pre-reload attempt must not land on the reloaded one, and
// the guard is a counter rather than a path comparison because the path does
// not change here. Clear mLoadedPath so updatePaintNode rebuilds the thread.
// Take back the two verdicts: the reload drops the node holding the old
// load's frame, so what is on screen stops being a rendered wallpaper the
// moment the render thread gets there - saying so here rather than an
// event-loop turn later closes the turn in which QML would be told there is
// content to transition against a black surface. Both clears are guarded on
// the current value, so whichever side runs second does nothing.
//
// Callers run this BEFORE their own change signal, so a QML handler running
// synchronously inside the emit - including one that re-enters a setter -
// cannot observe the new value while the retired load's generation is still
// the current one. update() stays with the caller too, after its emit,
// preserving the original ordering exactly.
void WallpaperEngineSurface::retireAndReload() {
	this->mLoadGeneration.fetch_add(1);
	this->mLoadedPath.clear();
	if (this->mRendered) {
		this->mRendered = false;
		emit this->renderedChanged();
	}
	if (this->mFailed) {
		this->mFailed = false;
		emit this->failedChanged();
	}
}

void WallpaperEngineSurface::setScaleMode(const QString& scaleMode) {
	if (scaleMode == this->mScaleMode) return;
	this->mScaleMode = scaleMode;
	// Scaling is a WE startup argument; rebuild the thread so it takes effect.
	this->retireAndReload();
	emit this->scaleModeChanged();
	this->update();
}

void WallpaperEngineSurface::setAudioEnabled(bool audioEnabled) {
	if (audioEnabled == this->mAudioEnabled) return;
	this->mAudioEnabled = audioEnabled;
	// Audio existence is a WE load-time decision (--silent skips stream/volume
	// setup entirely); rebuild the thread so the toggle takes effect.
	this->retireAndReload();
	emit this->audioEnabledChanged();
	this->update();
}

void WallpaperEngineSurface::setVolume(int volume) {
	const int clamped = std::clamp(volume, 0, 100);
	if (clamped == this->mVolume) return;
	this->mVolume = clamped;
	// WE reads its volume once at load (the same stream setup --silent skips),
	// so this is a reload like audioEnabled - which is why the QML side should
	// commit a slider on release rather than per drag tick.
	this->retireAndReload();
	emit this->volumeChanged();
	this->update();
}

void WallpaperEngineSurface::setAudioProcessing(bool audioProcessing) {
	if (audioProcessing == this->mAudioProcessing) return;
	this->mAudioProcessing = audioProcessing;
	this->retireAndReload();
	emit this->audioProcessingChanged();
	this->update();
}

void WallpaperEngineSurface::setMouseDisabled(bool mouseDisabled) {
	if (mouseDisabled == this->mMouseDisabled) return;
	this->mMouseDisabled = mouseDisabled;
	this->retireAndReload();
	emit this->mouseDisabledChanged();
	this->update();
}

void WallpaperEngineSurface::setParallaxDisabled(bool parallaxDisabled) {
	if (parallaxDisabled == this->mParallaxDisabled) return;
	this->mParallaxDisabled = parallaxDisabled;
	this->retireAndReload();
	emit this->parallaxDisabledChanged();
	this->update();
}

void WallpaperEngineSurface::setParticlesDisabled(bool particlesDisabled) {
	if (particlesDisabled == this->mParticlesDisabled) return;
	this->mParticlesDisabled = particlesDisabled;
	this->retireAndReload();
	emit this->particlesDisabledChanged();
	this->update();
}

void WallpaperEngineSurface::setProperties(const QVariantMap& properties) {
	if (properties == this->mProperties) return;
	this->mProperties = properties;
	// Applied over the project's defaults at load (WE's own --set-property
	// path), so a change is a reload like every other load-time argument.
	this->retireAndReload();
	emit this->propertiesChanged();
	this->update();
}

void WallpaperEngineSurface::setFocusX(qreal focusX) {
	const qreal clamped = std::clamp(focusX, 0.0, 1.0);
	if (clamped == this->mFocusX) return;
	this->mFocusX = clamped;
	// LIVE, not a reload: the blit reads the focus every frame, so this pushes
	// straight to the running thread and pans the wallpaper in place - the
	// whole point of doing the fill crop ourselves rather than in WE. mThread
	// is null until the first updatePaintNode, which re-applies both axes on
	// the load path (like setOccluded), so a wallpaper that comes up with a
	// non-centre focus already stored starts there.
	if (this->mThread)
		this->mThread->setFocus(static_cast<float>(this->mFocusX), static_cast<float>(this->mFocusY));
	emit this->focusXChanged();
}

void WallpaperEngineSurface::setFocusY(qreal focusY) {
	const qreal clamped = std::clamp(focusY, 0.0, 1.0);
	if (clamped == this->mFocusY) return;
	this->mFocusY = clamped;
	if (this->mThread)
		this->mThread->setFocus(static_cast<float>(this->mFocusX), static_cast<float>(this->mFocusY));
	emit this->focusYChanged();
}

void WallpaperEngineSurface::setOccluded(bool occluded) {
	if (occluded == this->mOccluded) return;
	this->mOccluded = occluded;
	// Order matters on the way back out. The producer is told first, so it can cut
	// its idle sleep short and get a frame in flight; then the stall watchdog is
	// re-armed; then this update() redraws the uncovered output from whatever is
	// already published instead of waiting on that first new frame. mThread is null
	// until the first updatePaintNode, which re-applies this on the load path, so an
	// output that is already covered when its wallpaper loads starts out idle rather
	// than running a full-rate frame or two first (in practice exactly, though not
	// by construction: the store lands while the new thread is still inside
	// eglMakeCurrent/glewInit/WE's setup(), which dwarf the gap).
	//
	// The MEMBER is what gets pushed to the producer, and the emit comes last, for
	// the reason setProjectPath spells out. An onOccludedChanged handler that
	// writes the property back runs synchronously inside the emit; pushing the
	// captured PARAMETER afterwards would hand the producer the outer call's stale
	// value while mOccluded and the watchdog kept the inner one. The end state -
	// QML and the watchdog believing the output is visible while the producer is
	// latched occluded and publishing nothing - repaints only from the 1Hz
	// watchdog, on the same stale texture, and nothing short of another true/false
	// toggle clears it.
	if (this->mThread) this->mThread->setOccluded(this->mOccluded);
	this->updateFrameDriver();
	this->update();
	emit this->occludedChanged();
}

} // namespace qs::wallpaperengine
