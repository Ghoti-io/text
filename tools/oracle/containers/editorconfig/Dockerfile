# The EditorConfig reference: editorconfig-core-c.
#
# **Two references in one image, and they disagree** - the good case, the same
# shape as containers/inidesktop. Both are here, `editorconfig-core-c` built from
# a commit and `editorconfig-core-py` installed from a pinned pip version, so that
# one digest pins both and the gate has one probe rather than two. Neither is
# normative: the normative artefact is
# `editorconfig-core-test`, whose 34 `parser` assertions
# tools/conformance/run-ini-editorconfig.sh scores separately. These two are here
# to be *differed against*, and a disagreement between them is the finding -
# notes/text/INI-DIALECTS.md records which questions each one answers wrongly.
#
# The sharing between them is **the specification and the conformance suite, not
# code**: different languages, different authors, no common dependency. So
# agreement on a rule the suite covers is weak evidence (both were tuned to pass
# it) and agreement on a rule it does not cover is worth something.
#
# **Built here rather than pulled** because Debian's `editorconfig` package
# bundles core-c with the suite version in its own version string, and what
# decides these answers is core-c's source at a tag. The tag is checked out by
# commit, not by name, because a tag can move.
#
# The driver links libeditorconfig directly rather than shelling out to the CLI,
# because the differential asks tens of thousands of questions and a process per
# question would dominate the run. Its own SHA-256 is compiled in and checked by
# the gate, for the reason containers/gitconfig/Dockerfile gives: an image
# carrying a driver built from an older copy of driver.c answers confidently and
# wrongly.
FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc libc6-dev cmake make git pkg-config libpcre2-dev ca-certificates \
      python3 python3-pip \
 && rm -rf /var/lib/apt/lists/*

# v0.12.11 (2026-04-15), the current release, resolved to its commit so that a
# moved tag cannot change the reference.
ARG EC_COMMIT=0ba157eff167c1b2d1bc0c3e13975b7a73fb9d25
WORKDIR /src
RUN git clone --quiet https://github.com/editorconfig/editorconfig-core-c.git . \
 && git fetch --quiet origin "$EC_COMMIT" \
 && git checkout --quiet "$EC_COMMIT" \
 && git rev-parse HEAD

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCUMENTATION=OFF \
 && cmake --build build -j"$(nproc)" \
 && cmake --install build

# The second reference. Pinned by version, and the version is printed at run time
# and checked against IMAGES like core-c's - a pip resolution that moved would
# change the printed line and fail the check rather than quietly changing what the
# comparison means. `--break-system-packages` because this image is the
# environment: there is nothing else in it to break.
RUN pip3 install --quiet --no-cache-dir --break-system-packages \
      "editorconfig==0.17.1" \
 && python3 -c "import editorconfig; print(editorconfig.VERSION)"

WORKDIR /build
COPY driver.c /build/driver.c
COPY py_driver.py /build/py_driver.py

# The Python driver's own SHA-256, of the file as copied in, substituted into the
# installed copy. Same reason as the C driver's: an image carrying a driver built
# from an older copy would answer confidently and wrongly, and the gate checks the
# printed line.
RUN sha=$(sha256sum /build/py_driver.py | cut -d' ' -f1) \
 && sed "s/GTEXT_PY_DRIVER_SHA\", \"unknown\"/GTEXT_PY_DRIVER_SHA\", \"$sha\"/" \
      /build/py_driver.py > /usr/local/bin/editorconfig-py-driver \
 && chmod 0755 /usr/local/bin/editorconfig-py-driver \
 && grep -q "$sha" /usr/local/bin/editorconfig-py-driver

RUN ldconfig \
 && sha=$(sha256sum driver.c | cut -d' ' -f1) \
 && ecver=$(pkg-config --modversion editorconfig 2>/dev/null || echo unknown) \
 && pyver=$(python3 -c "import editorconfig as e; print('%d.%d.%d' % e.VERSION[:3])") \
 && gcc -std=c17 -O2 -Wall -Wextra -o /usr/local/bin/editorconfig-driver driver.c \
        -DGTEXT_DRIVER_SHA="\"$sha\"" \
        -DGTEXT_EC_VERSION="\"$ecver\"" \
        -DGTEXT_EC_PY_VERSION="\"$pyver\"" \
        $(pkg-config --cflags --libs editorconfig) \
 && gcc --version | head -1 \
 && /usr/local/bin/editorconfig-driver --version \
 && /usr/local/bin/editorconfig-py-driver --version
