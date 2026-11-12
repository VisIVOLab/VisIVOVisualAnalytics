// The page's shell (epic #166): what keeps it from dead-ending. It owns the
// lifecycle watch (lifecycle.js), the notices on the 2-D page and on the
// headset's message card (notice.js), the first-run controls guide
// (onboarding.js), the diagnostics log and its sending (diagnostics.js), and
// the page-wide error capture. main.js gives it a few callbacks (what is on
// show, how to re-open the cube, enter or leave VR) and calls three hooks per
// frame at most: cardPointer (the controller's ray on the card first),
// cardForFrame (the card to draw) and xrFrame (the frame-rate summary).

import { Lifecycle, PAGE_VR_API, classifyFailure, keepRecoveryPending, noticeFor, isLoopback, isHeadsetBrowser } from './lifecycle.js';
import { NoticeCenter, cardLayout, drawCard, cardModel, cardNeedsMove, parkedCardModel, renderDom, CARD_PX, CARD_SIZE } from './notice.js';
import { guideNotice, guideStep, guideText } from './onboarding.js';
import { DiagLog, LogSender, diagnosticsReport, fpsSummary } from './diagnostics.js';
import { PanelPointer, rayPanel } from './vr-panel.js';

/**
 * `opts`: {page: {build, api}, renderer: () => Renderer, backend: () =>
 * Backend | null (null: no token yet), token: () => string, ctx: {inVR(),
 * exitVR(), enterVR(), recoverDataset(reason) → Promise<{ok, detail}>,
 * hasBackendCube(), loading(), loadGen(), onGuideSeen(), summary() → object, onRetry()}}.
 */
export function createShell({ page, renderer, backend, token, ctx, doc = document, win = window }) {
  const notices = new NoticeCenter();
  const diag = new DiagLog({ secrets: () => [token(), backend() ? backend().session : ''] });
  const where = { loopback: isLoopback(win.location.hostname), headset: isHeadsetBrowser(win.navigator.userAgent) };
  let downSince = 0;
  let recovering = null;
  let lastRecoverAt = 0;
  let loggedVersion = '';
  let pendingRecover = false;     // the backend restarted, or a reopen failed for now: reopen once it is possible
  let pendingGen = 0;             // the load generation when it was set
  let pendingReason = 'restarted';   // what the pending reopen says when it runs

  // ── Sending the log ─────────────────────────────────────────────────────
  const sender = new LogSender({
    build: page.build, userAgent: win.navigator.userAgent,
    post: async (body) => {
      const b = backend();
      if (!b) return { status: 0 };
      return b.log(body);
    },
  });
  diag.onAdd((e) => { if (backend()) sender.enqueue(e); });
  const log = (level, event, message = '') => diag.add(level, event, message);

  // ── The lifecycle ───────────────────────────────────────────────────────
  const life = new Lifecycle({
    page,
    fetchInfo: (opts) => (backend() || { info: (o) => ctx.publicInfo(o) }).info(opts),
    onChange: (ev) => onLife(ev),
  });

  function inVR() { return !!ctx.inVR(); }

  function onLife(ev) {
    const { state, prev } = ev;
    if (state === 'down') {
      if (prev !== 'down') { downSince = Date.now(); log('warning', 'backend-unreachable', String(ev.error && ev.error.message || '')); }
      // Nothing answers, or something answers with an HTTP error (a proxy's
      // 502): two notices, one shown at a time. Refreshed at each failed check
      // (every 1 to 5 s), never more often.
      if (ev.status) {
        notices.clear('unreachable');
        notices.show(noticeFor('http-error', { status: ev.status, inVR: inVR() }), { force: true });
      } else {
        notices.clear('http-error');
        notices.show(noticeFor('unreachable', { ...where, inVR: inVR(), attempts: ev.attempts }), { force: true });
      }
    } else {
      if (prev === 'down') {
        log('info', 'backend-back', `after ${((Date.now() - downSince) / 1000).toFixed(1)} s`);
        notices.clear('unreachable');
        ctx.onRetry();
      }
      notices.clear('unreachable');
      notices.clear('http-error');
    }
    if (state === 'auth') {
      if (prev !== 'auth') log('warning', 'token-refused', '');
      notices.show(noticeFor('auth', { inVR: inVR() }), { force: true });
    } else if (prev === 'auth') {
      notices.clear('auth');
      sender.resume();
      ctx.onRetry();
    }
    const v = ev.version;
    if (v && !v.ok) {
      if (loggedVersion !== v.why) { loggedVersion = v.why; log('warning', 'version-mismatch', v.why || v.kind); }
      notices.show(noticeFor(v.kind, { why: v.why, soft: !!v.soft, inVR: inVR() }));
    } else if (v && v.ok) {
      notices.clear('reload'); notices.clear('update-app');
    }
    if (ev.restarted) log('warning', 'backend-restarted', `instance ${ev.info && ev.info.instance}`);
    // The cube is opened again once the token is accepted (with a refused
    // token the attempt could only fail, and say less than the auth notice).
    if (ev.restarted) { pendingRecover = true; pendingGen = ctx.loadGen(); pendingReason = 'restarted'; }
    if (pendingRecover && state === 'ok') {
      // Kept until a recovery actually starts: a load under way or the
      // throttle only postpones it to a later check. A load finished since the
      // restart has replaced the cube: nothing to reopen then. A reopen that
      // fails for now sets it again (keepRecoveryPending).
      if (!ctx.hasBackendCube() || (ctx.loadGen() !== pendingGen && !ctx.loading())) pendingRecover = false;
      else if (recover(pendingReason)) pendingRecover = false;
    }
    render();
  }

  /** The backend lost the cube's dataset (restart, the desktop session ended): open it again. */
  function recover(reason) {
    if (!ctx.hasBackendCube() || ctx.loading()) return null;
    if (recovering) return recovering;
    if (reason !== 'user' && Date.now() - lastRecoverAt < 10000) return null;
    lastRecoverAt = Date.now();
    // The cube this reopen is for, read before any await.
    const gen = ctx.loadGen();
    recovering = (async () => {
      let r;
      let kind = '';   // classifyFailure's, when the reopen threw
      try { r = await ctx.recoverDataset(reason); } catch (err) {
        kind = classifyFailure(err);
        r = { ok: false, detail: String(err.message || err) };
      }
      recovering = null;
      // A failure that settles after a load replaced the cube says nothing
      // about the cube on show now: superseded, like an answer that saw it.
      if (!r.ok && !r.superseded && ctx.loadGen() !== gen) {
        r = { ok: false, superseded: true, detail: `superseded by a newer load (${r.detail || ''})` };
      }
      if (r.superseded) {
        // A load replaced the cube while it was being reopened: that load's
        // cube is the one on show, nothing to say, nothing left to reopen.
        pendingRecover = false;
        log('info', 'cube-reopen-superseded', reason);
      } else if (r.ok) {
        pendingRecover = false;
        notices.clear('reopen-failed');
        log('info', 'cube-reopened', `${reason}: ${r.detail || ''}`);
        notices.show(noticeFor(reason === 'restarted' ? 'restarted' : 'reopened'), { force: true });
      } else {
        log('error', 'cube-reopen-failed', r.detail || '');
        const auth = kind === 'auth' || /^40[13]:/.test(r.detail || '');
        // What time mends (a refused token, a timeout, a busy or failing
        // server) is tried again at a later healthy check, after the
        // throttle: the cube on show still points at the lost dataset. A file
        // that changed waits for the user (open it again). This latest
        // answer decides, whatever an earlier failure left pending.
        pendingRecover = keepRecoveryPending(r, auth ? 'auth' : kind);
        if (pendingRecover) {
          pendingGen = gen;
          // A retry is never the user's: it keeps to the throttle.
          pendingReason = reason === 'user' ? 'dataset-gone' : reason;
        }
        // A refused token is the auth notice's to explain; the reopen waits for it.
        if (!auth) notices.show(noticeFor('reopen-failed', { detail: r.detail, inVR: inVR() }), { force: true });
      }
      render();
      return r;
    })();
    return recovering;
  }

  /** Every request's failure (api.js requestHooks). */
  function requestFailed(err, path) {
    const kind = life.report(err);
    // A render session asked for a dataset the backend no longer holds counts
    // too (the refiner then waits for the reopened dataset).
    if (kind === 'dataset-gone') {
      log('warning', 'dataset-gone', `${path}: ${err.detail || ''}`);
      recover('dataset-gone');
    } else if (kind === 'server') {
      log('error', 'request-failed', `${path}: ${err.message}`);
    }
  }

  // ── Page-wide errors ────────────────────────────────────────────────────
  win.addEventListener('error', (e) => {
    const where_ = e.filename ? ` (${String(e.filename).split('/').pop()}:${e.lineno})` : '';
    log('error', 'page-error', `${e.message || e.error || 'error'}${where_}`);
  });
  win.addEventListener('unhandledrejection', (e) => {
    const r = e.reason;
    if (r && r.network) return;        // a request with nothing answering: the lifecycle says so
    log('error', 'unhandled-rejection', String((r && (r.stack || r.message)) || r).split('\n').slice(0, 3).join(' | '));
  });

  // ── The guide ───────────────────────────────────────────────────────────
  let guidePage = 0;
  function showGuide(i = 0) {
    guidePage = i;
    notices.show(guideNotice(i), { force: true });
    render();
  }

  // ── Actions from the card or the banner ─────────────────────────────────
  function act(id) {
    const cur = notices.current();
    log('debug', 'notice-action', `${id} on ${cur ? cur.id : '-'}`);
    if (id === 'guide-next' || id === 'guide-back') { showGuide(guideStep(guidePage, id)); return; }
    if (id === 'guide-close') { notices.clear('guide'); ctx.onGuideSeen(); render(); return; }
    if (id === 'dismiss') { if (cur) notices.dismiss(cur.id); render(); return; }
    // A sticky notice folded into a line on the panel until its state changes
    // ("?" on the panel brings it back).
    if (id === 'hide') { if (cur) { notices.hide(cur.id); log('debug', 'notice-hidden', cur.id); } render(); return; }
    if (id === 'retry') { life.retryNow(); ctx.onRetry(); return; }
    if (id === 'reload') { win.location.reload(); return; }
    if (id === 'exit-vr') { ctx.exitVR(); return; }
    if (id === 'enter-vr') { if (cur) notices.dismiss(cur.id); ctx.enterVR(); render(); return; }
    if (id === 'reopen') { lastRecoverAt = 0; recover('user'); }
  }

  // ── The 2-D banner ──────────────────────────────────────────────────────
  let domKey = '';
  function render() {
    const cur = notices.current();
    const key = `${notices.version}|${inVR()}`;
    if (key === domKey) return;
    domKey = key;
    // The guide has its own text on the page; the banner is for the lifecycle.
    renderDom(doc, cur && cur.level === 'guide' && !inVR() ? null : cur, act);
  }

  // The notices that hide by themselves go (once a second).
  win.setInterval(() => { notices.current(); render(); }, 1000);

  // ── The card in the headset ─────────────────────────────────────────────
  const card = {
    canvas: Object.assign(doc.createElement('canvas'), { width: CARD_PX[0], height: CARD_PX[1] }),
    tex: null, key: '', model: null, notice: null, layout: [], hit: null, since: 0, parked: false,
  };
  /** A sticky card in front of the viewer moves aside after this long, so it
   *  never stands between the viewer and the cube for good. */
  const PARK_AFTER_MS = 6000;
  card.pointer = new PanelPointer([]);

  /** The card to draw this frame for a viewer at `head` (4×4), or null. */
  function cardForFrame(head) {
    const n = notices.current();
    if (!n) { card.notice = null; card.model = null; card.hit = null; return null; }
    if (!card.tex) card.tex = renderer().createPanel();
    if (n !== card.notice) {
      const moveIt = !card.notice || card.notice.id !== n.id;
      card.notice = n;
      card.layout = cardLayout(n);
      card.pointer.layout = card.layout;
      if (moveIt) { card.model = null; card.since = performance.now(); card.parked = false; }
    }
    if (n.sticky && !card.parked && performance.now() - card.since > PARK_AFTER_MS) {
      // Aside: 40° to the left and lower, where it stays (it is still there to
      // read and press, the cube is free).
      card.parked = true;
      card.model = parkedCardModel(head);
    } else if (!card.model || (!card.parked && cardNeedsMove(card.model, head))
        || (card.parked && cardNeedsMove(card.model, head, 180, 2.5))) {
      card.model = card.parked ? parkedCardModel(head) : cardModel(head);
    }
    const key = `${notices.version}|${card.pointer.hover}`;
    if (key !== card.key) {
      drawCard(card.canvas.getContext('2d'), n, card.layout, { hover: card.pointer.hover });
      renderer().updatePanel(card.tex, card.canvas);
      card.key = key;
    }
    return { model: card.model, tex: card.tex, size: CARD_SIZE };
  }

  /** The right ray on the card (world space), the trigger; true when the card takes it. */
  function cardPointer(ray, pressed) {
    const hit = card.model && ray ? rayPanel(card.model, CARD_SIZE, ray.o, ray.d, CARD_PX) : null;
    card.hit = hit;
    for (const a of card.pointer.update(hit, pressed && !!card.model)) {
      if (a.type === 'press' && a.widget.action) act(a.widget.action);
    }
    return !!card.model && (card.pointer.takesRay() || card.pointer.owns);
  }

  // ── Frame-rate summaries ────────────────────────────────────────────────
  let fpsAt = 0;
  function xrFrame(now, intervals, targetHz, extra = '') {
    if (!fpsAt) fpsAt = now;
    if (now - fpsAt > 60000) {
      fpsAt = now;
      log('info', 'fps', `${fpsSummary(intervals, targetHz)}${extra ? `; ${extra}` : ''}`);
    }
  }
  function xrStarted() { fpsAt = 0; }

  // ── Diagnostics text ────────────────────────────────────────────────────
  function diagnostics() {
    const s = ctx.summary();
    return diagnosticsReport({
      time: new Date().toISOString(), page, backend: life.info, lifecycle: life.state,
      address: `${win.location.protocol}//${win.location.host}${win.location.pathname}`,
      userAgent: win.navigator.userAgent, ...s, errors: diag.items, secrets: [token(), backend() ? backend().session : ''],
    });
  }

  return {
    notices, diag, life, log, showGuide, act, render, recover, requestFailed,
    cardForFrame, cardPointer, xrFrame, xrStarted, diagnostics, guideText,
    get cardHit() { return card.hit; },
    /** The card on show: its model (world), layout and notice (debug hooks). */
    get card() { return card; },
    get cardOnShow() { return !!card.model; },
    /** The line the panel shows for a hidden sticky notice, or ''. */
    badge() { return notices.badge(); },
    /** "?" on the panel: a hidden notice first, else the guide. */
    help() { if (notices.unhideAll()) render(); else showGuide(0); },
    start() { life.start(); },
    /** Messages the page shows for its own events (context loss, XR end, budget). */
    notify(kind, c = {}, { force = true } = {}) { notices.show(noticeFor(kind, { ...c, inVR: inVR() }), { force }); render(); },
    clearNotice(id) { notices.clear(id); render(); },
    classifyFailure,
    PAGE_VR_API,
  };
}
