# toml++ with its unreleased-features set on: the second reader for the
# relaxations TOML v1.1.0 makes.
#
# **Built here, and the header is the pin.** toml++ is a single header, so its
# SHA-256 is the implementation rather than a name for one; the URL carries a tag
# and a tag can be moved. See SOURCES for the hash and for what 3.4.0's
# unreleased set is and is not.
#
# The base is pinned by digest. The *compiler* is not pinned, and that is
# deliberate rather than overlooked: what decides this reference's answers is
# toml++'s source, and a different g++ compiling the same header does not read
# TOML differently. Pinning it would mean either a gcc image five times the size
# or a Debian snapshot pin, for a guarantee about something that is not the
# claim. The version is printed at build time so that a reader can see which one
# it was.
#
# The driver is compiled here and its own SHA-256 is compiled into it, because an
# image carrying a driver built from an older copy of driver.cpp would answer
# confidently and wrongly - the stale-artifact failure this repository keeps
# meeting. tools/oracle/toml_1_1_diff.py hashes the committed file and refuses to
# compare if the two differ, naming `make oracle-images`.
FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY SOURCES /build/SOURCES
COPY driver.cpp /build/driver.cpp

RUN url=$(awk '!/^#/ && NF { print $2 }' SOURCES) \
 && curl -fsSL -o toml.hpp "$url" \
 && awk '!/^#/ && NF { print $1 "  toml.hpp" }' SOURCES > toml.hpp.sha256 \
 && sha256sum -c toml.hpp.sha256 \
 && sha=$(sha256sum driver.cpp | cut -d' ' -f1) \
 && g++ -std=c++20 -O2 -o /usr/local/bin/toml-driver driver.cpp -I . \
        -DGTEXT_DRIVER_SHA="\"$sha\"" \
 && apt-get purge -y curl && apt-get autoremove -y \
 && g++ --version | head -1 \
 && /usr/local/bin/toml-driver < /dev/null
