#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Capture the request headers a real Chrome sends, in wire order.

Drives an installed google-chrome (headless) against the local test
server (tests/server/test_server.py) through the request kinds crnet
emulates, and prints what the server received. Use it when bumping
chromium/VERSION to check whether src/profile.cc and the expectations
in tests/test_headers.cc still match the new release.

    tests/fingerprint/capture_chrome.py [--chrome google-chrome] [--json]

Only the local server is contacted.
"""
import argparse
import asyncio
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "server"))
import test_server as ts  # noqa: E402

PAGE = b"""<!doctype html><html><body><pre id=out></pre>
<form id=f method=POST action="/echo?kind=navigate_form_post">
<input name=a value=b></form>
<script>
async function go() {
  const j = (u, o) => fetch(u, o).then(r => r.text());
  await j('/echo?kind=fetch_get');
  await j('/echo?kind=fetch_get_page_accept',
          {headers: {'Accept': 'application/json, text/plain, */*'}});
  await j('/echo?kind=fetch_post_json', {method: 'POST',
          headers: {'Content-Type': 'application/json', 'X-Custom': '1'},
          body: '{"a":1}'});
  try { await fetch('https://127.0.0.1:' + location.port +
                    '/echo?kind=fetch_cross_site'); } catch (e) {}
  const x = new XMLHttpRequest();
  x.open('GET', '/echo?kind=xhr_get', false);
  x.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
  x.send();
  if (location.search.includes('form'))
    document.getElementById('f').submit();
  else
    location.href = '/echo?kind=navigate_script';
}
go();
</script></body></html>"""

CAPTURED = []


def route(req):
    if req.path == "/capture.html":
        return ts.Response(200, [("content-type", "text/html")], PAGE)
    if req.path == "/echo":
        kind = dict(req.query).get("kind", "navigate_typed")
        CAPTURED.append({"kind": kind, "protocol": req.protocol,
                         "headers": [list(h) for h in req.headers]})
    return ORIG_ROUTE(req)


ORIG_ROUTE = ts.route
ts.route = route


async def run_chrome(chrome, url):
    prof = tempfile.mkdtemp(prefix="crnet-capture-")
    proc = await asyncio.create_subprocess_exec(
        chrome, "--headless=new", "--no-sandbox",
        "--ignore-certificate-errors", "--user-data-dir=" + prof,
        "--virtual-time-budget=4000", "--dump-dom", url,
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
    try:
        await asyncio.wait_for(proc.wait(), 30)
    except asyncio.TimeoutError:
        proc.kill()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chrome", default="google-chrome")
    ap.add_argument("--json", action="store_true", help="raw JSON output")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="crnet-capture-cert-")
    crt, key = ts.make_cert(tmp)
    servers = [
        await asyncio.start_server(ts.serve_tls, "127.0.0.1", 0,
                                   ssl=ts.tls_context(crt, key, alpn))
        for alpn in (["h2", "http/1.1"], ["http/1.1"])
    ]
    for srv in servers:
        port = srv.sockets[0].getsockname()[1]
        base = "https://localhost:%d" % port
        await run_chrome(args.chrome, base + "/echo")
        await run_chrome(args.chrome, base + "/capture.html?form")
        await run_chrome(args.chrome, base + "/capture.html?nav")

    seen = set()
    result = []
    for c in CAPTURED:
        key = (c["kind"], c["protocol"])
        if key in seen:
            continue
        seen.add(key)
        result.append(c)

    if args.json:
        json.dump(result, sys.stdout, indent=1)
        print()
        return
    for c in result:
        print("== %s (%s)" % (c["kind"], c["protocol"]))
        for k, v in c["headers"]:
            print("   %s: %s" % (k, v if len(v) < 90 else v[:87] + "..."))


if __name__ == "__main__":
    asyncio.run(main())
