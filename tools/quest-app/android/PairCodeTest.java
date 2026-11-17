package __PACKAGE__;

/**
 * PairCode's checks on a plain JVM (issue #383). Not built into the app:
 * backend/tests/test_quest_app_wrapper.py compiles and runs it when a JDK is
 * there. Exits non-zero on the first failure.
 */
public final class PairCodeTest {
    static int checks = 0;

    static void eq(Object want, Object got, String what) {
        checks++;
        if (!String.valueOf(want).equals(String.valueOf(got))) {
            throw new AssertionError(what + ": wanted <" + want + "> got <" + got + ">");
        }
    }

    public static void main(String[] args) {
        final String loader = "https://vr.example.org/?launch=immersive";

        // Typed codes.
        eq("ABC234", PairCode.typed(" abc-234 ").code, "typed, grouped, lower case");
        eq("ABC234", PairCode.typed("AB C2 34").code, "typed with spaces");
        eq(false, PairCode.typed("ABC123").ok(), "1 is not in the alphabet");
        eq(false, PairCode.typed("ABCO23").ok(), "O is not in the alphabet");
        eq(false, PairCode.typed("ABC23").ok(), "too short");
        eq(false, PairCode.typed("").ok(), "empty");
        eq(true, PairCode.typed("").problem.startsWith("Type the code"), "empty says what to do");

        // Scanned: the desktop's QR code (page_url with #pair=).
        eq("ABC234", PairCode.scanned("https://vr.example.org/#pair=ABC-234", loader).code, "the desktop's QR");
        eq("ABC234", PairCode.scanned("https://vr.example.org/#pair=abc234&x=1", loader).code, "with more after it");
        eq("ABC234", PairCode.scanned("https://VR.example.org:443/#pair=ABC234", loader).code, "default port, host case");
        eq("ABC234", PairCode.scanned("ABC-234", loader).code, "a bare code");
        eq(false, PairCode.scanned("https://other.example.org/#pair=ABC234", loader).ok(), "another loader");
        eq(true, PairCode.scanned("https://other.example.org/#pair=ABC234", loader).problem.contains("other.example.org"),
                "another loader is named");
        eq(false, PairCode.scanned("http://vr.example.org/#pair=ABC234", loader).ok(), "another scheme");
        eq(false, PairCode.scanned("https://vr.example.org/?pair=ABC234", loader).ok(), "the code in the query");
        eq(false, PairCode.scanned("https://vr.example.org/#token=abc", loader).ok(), "no code");
        eq(false, PairCode.scanned("https://vr.example.org/#pair=ABC1", loader).ok(), "a bad code");
        eq(false, PairCode.scanned("WIFI:S:home;T:WPA;P:secret;;", loader).ok(), "another kind of QR code");
        eq("", PairCode.scanned("", loader).problem, "nothing read: nothing to say");

        // The launch the scanner sends.
        eq("https://vr.example.org/?launch=immersive#pair=ABC234", PairCode.loaderLaunch(loader, "ABC234"), "launch");
        eq("https://vr.example.org/?launch=immersive#pair=ABC234",
                PairCode.loaderLaunch(loader + "#old", "ABC234"), "launch drops an old fragment");

        // What the launcher opens.
        eq(true, PairCode.isLoaderLaunch("https://vr.example.org/?launch=immersive#pair=ABC234", loader), "loader launch");
        eq(true, PairCode.isLoaderLaunch("https://vr.example.org/index.html?launch=immersive#pair=ABC234", loader), "index.html");
        eq(false, PairCode.isLoaderLaunch("https://vr.example.org/?launch=immersive", loader), "loader without a code");
        eq(false, PairCode.isLoaderLaunch("https://evil.example.org/?launch=immersive#pair=ABC234", loader), "another host");
        eq(false, PairCode.isLoaderLaunch("https://vr.example.org:8443/#pair=ABC234", loader), "another port");
        eq(true, PairCode.isLoaderLaunch("http://localhost:8790/sub/?launch=immersive#pair=ABC234",
                "http://localhost:8790/sub/?launch=immersive"), "a development loader under a path");
        eq(false, PairCode.isLoaderLaunch("http://localhost:8790/other/#pair=ABC234",
                "http://localhost:8790/sub/?launch=immersive"), "outside the loader's directory");
        // The desktop's QR code handed to the app as a link (the headset's own scanner).
        eq("https://vr.example.org/?launch=immersive#pair=ABC234",
                PairCode.loaderLaunchFor("https://vr.example.org/#pair=abc-234", loader), "QR link to the app's launch");
        eq("https://vr.example.org/?launch=immersive#pair=ABC234",
                PairCode.loaderLaunchFor("https://vr.example.org/?other=1#pair=ABC234&x=y", loader), "query replaced");
        eq(null, PairCode.loaderLaunchFor("https://vr.example.org/", loader), "no code: nothing");
        eq(null, PairCode.loaderLaunchFor("https://evil.example.org/#pair=ABC234", loader), "another host: nothing");
        eq(true, PairCode.isBackendPage("http://localhost:8781/vr/?launch=immersive#ticket=x"), "USB launch");
        eq(true, PairCode.isBackendPage("http://127.0.0.1:8000/prefix/vr/index.html"), "USB launch, prefix");
        eq(false, PairCode.isBackendPage("http://192.168.1.2:8000/vr/"), "a LAN backend");
        eq(false, PairCode.isBackendPage("https://vr.example.org/"), "the loader is not a backend page");
        eq(false, PairCode.isBackendPage("not a url"), "garbage");

        System.out.println("PairCodeTest: " + checks + " checks passed");
    }
}
