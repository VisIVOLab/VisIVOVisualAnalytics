package __PACKAGE__;

import java.net.URI;
import java.net.URISyntaxException;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * The headset app's decisions about addresses and codes (issue #383), with
 * no Android API: unit-tested on a plain JVM (tools/quest-app/android/
 * PairCodeTest.java, run by backend/tests/test_quest_app_wrapper.py).
 *
 * - What a scanned QR code or a typed code means: the desktop's Open on
 *   Headset dialog shows a pairing code (6 characters) and a QR code of the
 *   public loader's address with the code after #pair=.
 * - Which addresses the app opens: a backend's VR page on the headset's own
 *   localhost (the desktop's USB launch, issue #255), or the loader the app
 *   was built for (its start URL) with a code.
 */
public final class PairCode {
    private PairCode() {}

    /** The pairing service's alphabet (services/vr-pair: no 0/O/1/I). */
    public static final String ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    public static final int LENGTH = 6;

    /** What a scan or a typed code gave: a code, or why there is none. */
    public static final class Result {
        public final String code;     // "" when there is none
        public final String problem;  // "" when there is a code
        Result(String code, String problem) { this.code = code; this.problem = problem; }
        public boolean ok() { return !code.isEmpty(); }
    }

    static Result code(String c) { return new Result(c, ""); }
    static Result problem(String p) { return new Result("", p); }

    /** A code as typed or scanned: spaces and dashes out, upper case. */
    public static String normalise(String text) {
        if (text == null) return "";
        StringBuilder b = new StringBuilder();
        for (char ch : text.trim().toCharArray()) {
            if (Character.isWhitespace(ch) || ch == '-') continue;
            b.append(ch);
        }
        return b.toString().toUpperCase(Locale.ROOT);
    }

    /** A pairing code: 6 characters of the service's alphabet. */
    public static boolean valid(String code) {
        if (code == null || code.length() != LENGTH) return false;
        for (char ch : code.toCharArray()) if (ALPHABET.indexOf(ch) < 0) return false;
        return true;
    }

    /** The code typed in the app's field. */
    public static Result typed(String text) {
        String c = normalise(text);
        if (c.isEmpty()) return problem("Type the code the computer shows.");
        if (!valid(c)) {
            return problem("A code has 6 letters and digits, like ABC-234 (no 0, O, 1 or I): check it.");
        }
        return code(c);
    }

    /**
     * What a QR code says. `loaderStart` is the app's own start URL (the
     * loader): a QR code of the loader's address must be of that loader
     * (same scheme, host and port), since a code only means something to
     * the pairing service of the loader that shows it. A bare code is
     * taken as it is.
     */
    public static Result scanned(String text, String loaderStart) {
        String t = text == null ? "" : text.trim();
        if (t.isEmpty()) return problem("");
        String lower = t.toLowerCase(Locale.ROOT);
        if (!lower.startsWith("http://") && !lower.startsWith("https://")) {
            String c = normalise(t);
            return valid(c) ? code(c) : problem("This QR code is not a VisIVO VR pairing code.");
        }
        URI u;
        try { u = new URI(t); } catch (URISyntaxException e) {
            return problem("This QR code is not a VisIVO VR pairing code.");
        }
        String c = fragmentValue(u.getRawFragment(), "pair");
        if (c.isEmpty()) return problem("This QR code is not a VisIVO VR pairing code.");
        c = normalise(c);
        if (!valid(c)) return problem("This QR code's pairing code cannot be read: type the code instead.");
        URI loader = parse(loaderStart);
        if (loader == null || !sameOrigin(u, loader)) {
            return problem("This code is for the pairing page at " + hostOf(u) + ", but this app pairs through "
                    + (loader == null ? "another page" : hostOf(loader))
                    + ". Set the computer's headset pairing to this app's service (Settings → Backends).");
        }
        return code(c);
    }

    /** The loader's address that pairs with `code` and enters VR by itself. */
    public static String loaderLaunch(String loaderStart, String code) {
        String base = loaderStart;
        int hash = base.indexOf('#');
        if (hash >= 0) base = base.substring(0, hash);
        return base + "#pair=" + code;
    }

    /**
     * A VR page of a backend on this headset's localhost (adb reverse), at
     * any port and under any path prefix: the desktop's USB launch (#255).
     */
    public static boolean isBackendPage(String url) {
        URI u = parse(url);
        if (u == null) return false;
        String scheme = lower(u.getScheme()), host = lower(u.getHost()), path = u.getPath();
        return ("http".equals(scheme) || "https".equals(scheme))
                && ("localhost".equals(host) || "127.0.0.1".equals(host))
                && path != null && (path.endsWith("/vr/") || path.endsWith("/vr/index.html"));
    }

    /**
     * The app's loader (same scheme, host and port as `loaderStart`, a path
     * under its directory) with a code after #pair=: the scanner's launch.
     */
    public static boolean isLoaderLaunch(String url, String loaderStart) {
        URI u = parse(url), l = parse(loaderStart);
        if (u == null || l == null || !sameOrigin(u, l)) return false;
        String dir = l.getPath() == null || l.getPath().isEmpty() ? "/" : l.getPath();
        dir = dir.substring(0, dir.lastIndexOf('/') + 1);
        String path = u.getPath() == null || u.getPath().isEmpty() ? "/" : u.getPath();
        if (!path.startsWith(dir) || path.contains("/../")) return false;
        return valid(normalise(fragmentValue(u.getRawFragment(), "pair")));
    }

    /**
     * What the launcher opens for a loader address with a code (the
     * scanner's launch, or the desktop's QR code read by the headset's own
     * scanner and handed to the app as a verified link): the app's own start
     * URL with that code, so the loader always pairs and enters VR by
     * itself; null when `url` is not one (isLoaderLaunch).
     */
    public static String loaderLaunchFor(String url, String loaderStart) {
        if (!isLoaderLaunch(url, loaderStart)) return null;
        URI u = parse(url);
        return loaderLaunch(loaderStart, normalise(fragmentValue(u.getRawFragment(), "pair")));
    }

    static String fragmentValue(String rawFragment, String key) {
        if (rawFragment == null) return "";
        for (String part : rawFragment.split("&")) {
            if (part.startsWith(key + "=")) {
                try {
                    return URLDecoder.decode(part.substring(key.length() + 1), StandardCharsets.UTF_8.name());
                } catch (Exception e) {
                    return "";
                }
            }
        }
        return "";
    }

    static URI parse(String s) {
        if (s == null) return null;
        try {
            URI u = new URI(s);
            return u.getScheme() == null || u.getHost() == null ? null : u;
        } catch (URISyntaxException e) {
            return null;
        }
    }

    static boolean sameOrigin(URI a, URI b) {
        return lower(a.getScheme()).equals(lower(b.getScheme())) && lower(a.getHost()).equals(lower(b.getHost()))
                && port(a) == port(b);
    }

    static int port(URI u) {
        if (u.getPort() >= 0) return u.getPort();
        return "https".equals(lower(u.getScheme())) ? 443 : 80;
    }

    static String hostOf(URI u) {
        return u.getPort() >= 0 ? u.getHost() + ":" + u.getPort() : String.valueOf(u.getHost());
    }

    static String lower(String s) { return s == null ? "" : s.toLowerCase(Locale.ROOT); }
}
