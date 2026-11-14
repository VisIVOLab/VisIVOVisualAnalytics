// One active VR page per browser (issue #226, point 5; by content since
// #237). Every "Open on Headset" (and every adb open) left another Quest
// Browser window behind; in the first Quest session one of two was empty, and
// Enter VR pressed there gave a black world. A page that opens tells the
// older ones, on a BroadcastChannel of its origin, that it is the one in use:
// they say "Open in another window", disable Enter VR and stop drawing, and
// can take over again with "Use this window" (which then tells the others in
// turn).
//
// By content (#237): a page that shows nothing (a tab the browser restored,
// an empty loader) never makes a page with a cube stand down. It stands down
// itself and names the cube shown in the other window. Between two pages
// that both show something, or both nothing, the newer one is in use. A
// page's content is a cube on show, a load under way or a link to load one.
// A claim of an older build carries no content: it counts as content (as
// before #237, the newer page wins).
//
// The paired loader (#213) runs the page in a same-origin iframe of the
// loader's origin: its pages share that origin's channel, so two loader
// windows behave the same way. A page served by the backend (localhost
// through adb reverse) and one in the loader are different origins and
// cannot see each other; the loader is the way in on a headset, so in
// practice the pages of one way in meet.
//
// The headset app (#255): a page in the immersive app's VR session is
// *pinned*. Ending its session would end the app, and a second launch's page
// would be killed with it (the user left in a dark void, measured on a Quest
// 3 in #254), so a pinned page never stands down: it answers every claim
// with its own, marked pinned, and the newer page stands down instead
// (reason 'vr') and hands it its launch (a message addressed to it: send()).
// Two levels: PINNED_ENTERING while its session is being asked for,
// PINNED_IN_VR once it runs. A page in VR outranks one entering, and never
// stands down at all; between two entering pages the usual order holds.
//
// Pure: the channel is injected; unit-tested in backend/tests/vr/single-page.test.mjs.

export const CHANNEL_NAME = 'visivo-vr-page';
/** Pin levels (#255): asking for the app's VR session, in it. */
export const PINNED_ENTERING = 1;
export const PINNED_IN_VR = 2;

const pinLevel = (v) => (v === true ? PINNED_ENTERING : Math.max(0, Math.min(PINNED_IN_VR, Number(v) || 0)));

/** A page id: random, so two pages opened in the same millisecond still differ. */
export function pageId(rand = Math.random, now = Date.now) {
  return `${now().toString(36)}-${Math.floor(rand() * 2 ** 32).toString(36)}`;
}

/**
 * Which of two pages is in use: `mine` and `theirs` are {t, id, content,
 * held}. Each claim has one rank: (held, then its time, then its id). A page
 * holds its rank once it shows something (or is loading it) or the user
 * chose it ("Use this window"), and keeps it until it stands down: a page
 * with content beats one without, the user's choice beats an older page with
 * a cube, and between equals the newer claim wins. A page's rank never drops
 * while it is in use, and one rank per claim is a total order, so the pages
 * settle on exactly one in use whatever order the messages arrive in (a load
 * that fails keeps its page's rank; the next claim of any page re-ranks).
 * True when `theirs` wins. A claim of an older build (no content field)
 * counts as held.
 */
export function theyWin(mine, theirs) {
  // A page in the headset app's VR session (#255) is never made to stand down;
  // one asking for it gives way only to a page already in it.
  const tp = pinLevel(theirs.pinned), mp = pinLevel(mine.pinned);
  if (tp !== mp) return tp > mp;
  if (mp === PINNED_IN_VR) return false;
  const tc = theirs.content === undefined || !!theirs.content || !!theirs.held;
  const mc = !!mine.content || !!mine.held;
  if (tc !== mc) return tc;
  return theirs.t > mine.t || (theirs.t === mine.t && String(theirs.id) > String(mine.id));
}

/**
 * The page's place among the open pages. `channel`: a BroadcastChannel (or
 * a stand-in with postMessage and onmessage); `onChange(active, info)` is
 * called when the page stands down (info: {by, cube, since, reason:
 * 'content' when the other page shows a cube and this one nothing, 'vr' when
 * the other page is pinned in the headset app's VR session, else 'newer'})
 * or takes over. A page is active from its start. `onMessage(m)`: the other
 * messages addressed to this page (m.to its id).
 */
export class SinglePage {
  constructor({ channel, id = pageId(), now = Date.now, onChange = () => {}, onMessage = () => {}, content = false, cube = '' }) {
    this.channel = channel;
    this.id = id;
    this.now = now;
    this.onChange = onChange;
    this.onMessage = onMessage;
    this.active = true;
    this.pinned = 0;           // the headset app's VR session (#255): PINNED_ENTERING, PINNED_IN_VR
    this.since = now();        // when this page last became the active one
    this.by = null;            // the page that took over
    this.content = !!content;  // shows a cube (or is loading one)
    this.cube = String(cube || '');
    // The rank held while in use: content shown (or loading) or the user's
    // choice, kept until the page stands down (see theyWin).
    this.held = this.content;
    if (channel) channel.onmessage = (e) => this._receive(e && e.data);
  }

  _message() {
    return { type: 'claim', id: this.id, t: this.since, content: this.content, cube: this.cube, held: this.held,
      pinned: this.pinned };
  }

  /** The page's pin level (#255: 0, PINNED_ENTERING, PINNED_IN_VR; true is
   *  PINNED_ENTERING): an active page says so. */
  setPinned(level) {
    const v = pinLevel(level);
    if (v === this.pinned) return;
    this.pinned = v;
    if (this.active) this._post(this._message());
  }

  /** A message for the page `to` (another type than 'claim'), from this one. */
  send(to, message) {
    if (!message || message.type === 'claim') return;
    this._post({ ...message, to: String(to || ''), from: this.id });
  }

  /** Announce this page as the active one (at start, or "Use this window").
   *  `force`: the user chose this window ("Use this window"): the others
   *  stand down whatever they show. */
  claim({ force = false } = {}) {
    this.since = this.now();
    const was = this.active;
    this.active = true;
    this.by = null;
    this.held = this.held || this.content || !!force;
    this._post(this._message());
    if (!was) this.onChange(true, {});
  }

  /** What the page shows changed: an active page says so (with the time of
   *  its claim, unchanged), so an empty page that claimed meanwhile stands
   *  down, and the others know which cube is in use. */
  setContent(content, cube = '') {
    const c = !!content, name = String(cube || '');
    if (c === this.content && name === this.cube) return;
    this.content = c;
    this.cube = name;
    if (this.active && c) this.held = true;
    if (this.active) this._post(this._message());
  }

  _post(m) {
    try { if (this.channel) this.channel.postMessage(m); } catch { /* closed */ }
  }

  _receive(m) {
    if (!m || typeof m !== 'object') return;
    if (m.type !== 'claim') {
      if (m.to === this.id && m.from !== this.id) {
        try { this.onMessage(m); } catch { /* the page's own */ }
      }
      return;
    }
    if (m.id === this.id || !this.active) return;
    const mine = { t: this.since, id: this.id, content: this.content, held: this.held, pinned: this.pinned };
    if (theyWin(mine, m)) {
      const theirContent = m.content === undefined ? true : !!m.content;
      const reason = pinLevel(m.pinned) ? 'vr' : theirContent && !this.content ? 'content' : 'newer';
      this.active = false;
      this.held = false;
      this.by = { id: m.id, t: m.t, cube: String(m.cube || ''), content: theirContent, pinned: pinLevel(m.pinned) > 0 };
      this.onChange(false, { by: m.id, cube: this.by.cube, since: m.t, reason });
    } else {
      // We stay (we show a cube and they do not, or our claim is newer:
      // their clock, or a page restored from the back-forward cache): say so.
      this._post(this._message());
    }
  }

  close() {
    try { if (this.channel) this.channel.close(); } catch { /* closed */ }
    this.channel = null;
  }
}

/** The page's words when it stood down. */
export const STOOD_DOWN = {
  title: 'Open in another window',
  lines: ['This VisIVO VR page was opened again in another window or tab, which is the one in use now.',
    'This one is paused: Enter VR is off here and nothing is drawn. Close it, or press "Use this window" to take over.'],
};

function clock(t) {
  const d = new Date(Number(t));
  if (!Number.isFinite(d.getTime())) return '';
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

/** The words of a page that stood down, for the reason it did (onChange's info). */
export function stoodDownText(info = {}) {
  const at = info.since ? clock(info.since) : '';
  const which = info.cube ? `the window showing ${info.cube}` : 'another window or tab';
  if (info.reason === 'vr') {
    return {
      title: 'VisIVO VR is already in VR',
      lines: ['The headset app was started again while it shows VR: what this launch carries is handed to it.',
        'This page is paused and never enters VR.'],
    };
  }
  if (info.reason === 'content') {
    return {
      title: info.cube ? `${info.cube} is open in another window` : 'The cube is open in another window',
      lines: [`This page shows nothing, so ${which}${at ? ` (in use since ${at})` : ''} stays the VisIVO VR page in use: switch to it in the browser's windows.`,
        'This one is paused: Enter VR is off here. Close it, or press "Use this window" to use this one instead.'],
    };
  }
  if (!info.cube) return STOOD_DOWN;
  return {
    title: STOOD_DOWN.title,
    lines: [`This VisIVO VR page was opened again in ${which}${at ? ` (since ${at})` : ''}, which is the one in use now.`,
      STOOD_DOWN.lines[1]],
  };
}
