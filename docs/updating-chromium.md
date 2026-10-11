# Updating to a new Chromium release

crnet's fingerprint is only as current as the Chromium it is built from.
A crnet built from Chrome N looks like Chrome N. Once most users run
N+2, that is an outlier. Follow Chrome Stable.

## Steps

1. **Pick the release.** Current versions are listed at
   <https://chromiumdash.appspot.com/releases?platform=Linux>, or:

   ```bash
   curl -s 'https://chromiumdash.appspot.com/fetch_releases?channel=Stable&platform=Linux&num=1'
   ```

2. **Fetch it.**

   ```bash
   echo 156.0.XXXX.YY > chromium/VERSION
   scripts/fetch-chromium.sh
   ```

3. **Build and run the tests.**

   ```bash
   cmake --build build && ctest --test-dir build -j"$(nproc)"
   ```

   CMake notices the new version, regenerates `args.gn` and rebuilds.
   Expect occasional compile errors in `src/` when `//net` APIs change.
   Most are renames or new parameters. Check the API in the new
   tree and adapt.

4. **Re-capture Chrome's browser headers** with a Chrome of the *same*
   version installed:

   ```bash
   google-chrome --version
   tests/fingerprint/capture_chrome.py
   ```

   Compare with `src/profile.cc` (the `kNavigateOrder` and `kFetchOrder`
   tables and the `accept` values) and `tests/test_headers.cc`. Things
   that changed in the past: new values in the navigation `accept`
   (e.g. `image/jxl`), new client-hint headers, a different position of
   `priority` or `cookie`. Update both the code and the expectations.

5. **Compare the TLS and HTTP/2 fingerprint** with the real browser:

   ```bash
   build/examples/crget https://tls.peet.ws/api/all > crnet.json
   google-chrome --headless=new --dump-dom https://tls.peet.ws/api/all > chrome.html
   ```

   `tls.ja4`, the extension list and `http2.akamai_fingerprint` must be
   identical. A difference usually means a new piece of browser-side
   configuration that crnet must replicate in `client.cc`, as with the
   TLS Trust Anchor IDs. To find it, grep `chrome/browser/` and
   `services/network/` for what feeds the differing `SSLContextConfig`
   or `HttpNetworkSessionParams` field.

6. **Check the version-dependent identity**: `crnet_default_user_agent()`
   and `crnet_default_sec_ch_ua()` derive from the new major version
   automatically. `Library.SecChUaMatchesChrome155` in
   `tests/test_basic.cc` pins one exact value. Update it to what the new
   Chrome sends.

7. Commit `chromium/VERSION` together with any code and test changes.

## Chromium patches

None are needed today. If a release ever requires one, put a
`git format-patch`-style file in `chromium/patches/*.patch`.
`scripts/fetch-chromium.sh` applies it through `scripts/apply-patches.sh`,
which first restores the touched files, so it can be re-run safely.
Configuring the CMake build checks that every patch is applied and
stops otherwise, since a tree without them would build but behave
differently.
