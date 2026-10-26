# Chromium sanitizer oracle

The image contains the Chrome for Testing Stable build `156.0.8078.47` (revision
1706539), the Stable of 2026-10-19, raised from `155.0.8059.84` on 2026-10-26 when the CSS
property oracle (`../cssprop`) took the same build.
The upstream archive is fetched by exact version from Google's Chrome for
Testing public storage and checked against SHA-256
`e6c94490f0b6b337225aeb2d2a5034858516b7017356980ba94f5176d8774a50`
(the bucket's own MD5 of that archive, `cvpoYXzFXs9OK75pfi91hA==` as `x-goog-hash` gives it, matched the download the hash was taken from).
The complete built image digest is recorded in `../IMAGES`.

The runtime gate invokes Chromium with `--disable-javascript`, background
networking disabled, a resolver rule that rejects hosts, and the container
runtime's `--network none`. It interprets representative local HTML files and
checks the browser's serialized DOM. It does not run page scripts or fetch
resources.
