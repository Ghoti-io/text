#!/usr/bin/env python3
"""Interpret sanitized representatives in pinned Chromium with scripts/network off."""
import html.parser
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env


FORBIDDEN = {"script", "style", "iframe", "frame", "frameset", "object", "embed",
             "applet", "portal", "fencedframe", "audio", "video", "source", "track",
             "canvas", "noscript", "noembed", "noframes", "svg", "math", "form",
             "input", "button", "select", "textarea", "template"}

CASES = [
    ("<p onclick=x>ok<a href='https://example.test/'>go</a></p>", (), ("p", "a")),
    ("<script>bad</script><style>x{color:red}</style><b>safe</b>", (), ("b",)),
    ("<a href='jav&#x61;script:alert(1)' target=_blank>bad</a>", (), ("a",)),
    ("<svg><foreignObject><p>foreign</p></foreignObject></svg><math><mi>x</mi></math>", (), ()),
    ("<template><b>hidden</b></template><iframe>frame</iframe><p>visible</p>", (), ("p",)),
    ("<noscript><b>bad</b></noscript><canvas>bad</canvas><noembed>bad</noembed>", (), ()),
    ("<noframes>bad</noframes><p>left<source src=x><track src=x>right</p>", (), ("p",)),
    ("<img src='data:text/html,x' onerror=x><img src='/ok.png' alt='ok'>", ("--images",), ("img",)),
    ("<img src='https://example.test/ok.png' alt='ok'>", ("--images",), ("img",)),
    ("<table onclick=x><tr><td colspan=1001>bad</td><td rowspan=0>ok</td></tr></table>",
     ("--tables",), ("table", "tbody", "tr", "td")),
]


class TreeCheck(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.elements = []
        self.unsafe_attributes = []
        self.bad_urls = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self.elements.append(tag)
        self.stack.append(tag)
        for name in attrs:
            if name.lower().startswith("on") or name.lower() == "style":
                self.unsafe_attributes.append((tag, name))
        for name in ("href", "src"):
            value = attrs.get(name, "")
            folded = value.lstrip().lower()
            if folded.startswith(("javascript:", "vbscript:", "data:", "file:", "blob:", "tel:")):
                self.bad_urls.append((tag, name, value))

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        self.handle_endtag(tag)

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i] == tag:
                del self.stack[i:]
                break


def main():
    # Gate controls: validate that the policy checker catches representative
    # planted defects before trusting its verdicts on sanitizer output.
    mutants = {
        "element filtering": "<script>bad()</script>",
        "attribute filtering": '<p onclick="bad()">x</p>',
        "URL classification": '<a href="javascript:bad()">x</a>',
        "image URL classification": '<img src="data:text/html,bad">',
        "CSS removal": '<p style="background:url(javascript:bad())">x</p>',
        "style-element removal": "<style>p{display:none}</style>",
    }
    for label, mutant in mutants.items():
        check = TreeCheck()
        check.feed(mutant)
        if not (FORBIDDEN.intersection(check.elements) or check.unsafe_attributes or check.bad_urls):
            raise SystemExit(f"oracle control failed to detect planted {label} defect")
    print(f"Chromium oracle controls rejected {len(mutants)} planted policy defects")

    binary = os.environ.get("SANITIZE_OURS", "build/sanitize-html-ours")
    if not os.path.exists(binary):
        raise SystemExit("missing sanitizer runner")
    version = oracle_env.version("chromium")
    print("Chromium policy interpretation: " + version)
    for number, (source, flags, expected) in enumerate(CASES, 1):
        sanitized = subprocess.run([binary, *flags], input=source.encode(),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if sanitized.returncode:
            raise SystemExit(f"case {number}: sanitizer failed: {sanitized.stderr.decode(errors='replace')}")
        with tempfile.TemporaryDirectory(prefix="ghoti-sanitize-") as temp:
            page = pathlib.Path(temp) / "fragment.html"
            page.write_bytes(sanitized.stdout)
            command = ["/opt/chrome/chrome", "--headless", "--no-sandbox", "--disable-gpu",
                       "--disable-javascript", "--disable-background-networking",
                       "--disable-component-update", "--disable-sync", "--no-proxy-server",
                       "--host-resolver-rules=MAP * ~NOTFOUND", "--dump-dom", page.as_uri()]
            argv = oracle_env.command("chromium", command, scratch=temp)
            interpreted = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         timeout=30)
            if interpreted.returncode:
                raise SystemExit(f"case {number}: Chromium failed: {interpreted.stderr.decode(errors='replace')}")
        check = TreeCheck()
        check.feed(interpreted.stdout.decode("utf-8", errors="replace"))
        present_forbidden = FORBIDDEN.intersection(check.elements)
        if present_forbidden or check.unsafe_attributes or check.bad_urls:
            raise SystemExit(f"case {number}: browser found forbidden content: "
                             f"elements={sorted(present_forbidden)} attrs={check.unsafe_attributes} urls={check.bad_urls}")
        if not set(expected).issubset(check.elements):
            raise SystemExit(f"case {number}: expected elements {expected}, got {check.elements}")
    print(f"Chromium interpreted {len(CASES)} sanitized cases; no forbidden active markup or URL survived")


if __name__ == "__main__":
    main()
