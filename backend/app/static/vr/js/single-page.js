// One active VR page per browser (issue #226, point 5). Every "Open on
// Headset" (and every adb open) leaves another Quest Browser window behind;
// in the first Quest session one of two was empty, and Enter VR pressed
// there gave a black world. A page that opens now tells the older ones, on a
// BroadcastChannel of its origin, to stand down: they say "Open in another
// window", disable Enter VR and stop drawing, and can take over again with
// "Use this window" (which then tells the others in turn).
//
// The paired loader (#213) runs the page in a same-origin iframe of the
// loader's origin: its pages share that origin's channel, so two loader
// windows behave the same way. A page served by the backend (localhost
// through adb reverse) and one in the loader are different origins and
// cannot see each other; the loader is the way in on a headset, so in
// practice the pages of one way in meet.
//
// Pure: the channel is injected; unit-tested in backend/tests/vr/single-page.test.mjs.

export const CHANNEL_NAME = 'visivo-vr-page';

/** A page id: random, so two pages opened in the same millisecond still differ. */
export function pageId(rand = Math.random, now = Date.now) {
  return `${now().toString(36)}-${Math.floor(rand() * 2 ** 32).toString(36)}`;
}

/**
 * The page's place among the open pages. `channel`: a BroadcastChannel (or
 * a stand-in with postMessage and onmessage); `onChange(active, info)` is
 * called when the page stands down or takes over. A page is active from its
 * start; it stands down when another page announces itself later.
 */
export class SinglePage {
  constructor({ channel, id = pageId(), now = Date.now, onChange = () => {} }) {
    this.channel = channel;
    this.id = id;
    this.now = now;
    this.onChange = onChange;
    this.active = true;
    this.since = now();        // when this page last became the active one
    this.by = null;            // the page that took over
    if (channel) channel.onmessage = (e) => this._receive(e && e.data);
  }

  /** Announce this page as the active one (at start, or "Use this window"). */
  claim() {
    this.since = this.now();
    const was = this.active;
    this.active = true;
    this.by = null;
    this._post({ type: 'claim', id: this.id, t: this.since });
    if (!was) this.onChange(true, {});
  }

  _post(m) {
    try { if (this.channel) this.channel.postMessage(m); } catch { /* closed */ }
  }

  _receive(m) {
    if (!m || typeof m !== 'object' || m.id === this.id) return;
    if (m.type === 'claim') {
      // A newer claim than ours (or a tie broken by the id): stand down.
      const newer = m.t > this.since || (m.t === this.since && String(m.id) > this.id);
      if (newer && this.active) {
        this.active = false;
        this.by = { id: m.id, t: m.t };
        this.onChange(false, { by: m.id });
      } else if (!newer && this.active) {
        // An older page claimed again after we did (its clock, or a page
        // restored from the back-forward cache): we are newer, say so.
        this._post({ type: 'claim', id: this.id, t: this.since });
      }
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
