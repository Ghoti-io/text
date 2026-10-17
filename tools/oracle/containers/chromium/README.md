# Chromium sanitizer oracle

The image contains the Chrome for Testing Stable build `155.0.8059.84`, the
latest stable release available when this oracle was pinned on 2026-10-16.
The upstream archive is fetched by exact version from Google's Chrome for
Testing public storage and checked against SHA-256
`cf0f049892dea22e6a6db26d47ed0d7776b7158edcbec4634086633df8bafcbc`.
The complete built image digest is recorded in `../IMAGES`.

The runtime gate invokes Chromium with `--disable-javascript`, background
networking disabled, a resolver rule that rejects hosts, and the container
runtime's `--network none`. It interprets representative local HTML files and
checks the browser's serialized DOM. It does not run page scripts or fetch
resources.
