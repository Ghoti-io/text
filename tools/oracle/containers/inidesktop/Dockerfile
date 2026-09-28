# The Desktop Entry references: GLib's GKeyFile and desktop-file-validate.
#
# **Built here rather than pulled, and both references are in one image**, which
# is unlike every other pin in this repository. The reason is that the two answer
# different halves of one question about the *same* document - GKeyFile what a
# value is, the validator whether the document is legal - so a comparison needs
# both, and running them from two images would mean two passes over a population
# of tens of thousands and no guarantee the two passes saw the same bytes.
#
# The base is pinned by digest. The two references are Debian packages, so what
# pins them is that digest plus the versions the driver prints at run time; the
# gate checks the printed line against the IMAGES entry. A Debian point release
# that moved either package would change that line and fail the check rather than
# silently changing the comparison.
#
# The **compiler is deliberately not pinned**, for the reason
# containers/tomlpp/Dockerfile gives: what decides these answers is glib's source
# and the validator's, and a different gcc compiling the same driver does not read
# a desktop entry differently.
#
# The driver's own SHA-256 is compiled into it and checked by the gate, because an
# image carrying a driver built from an older copy of driver.c would answer
# confidently and wrongly - the stale-artifact failure this repository keeps
# meeting.
FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc libc6-dev libglib2.0-dev pkg-config desktop-file-utils \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY driver.c /build/driver.c

RUN sha=$(sha256sum driver.c | cut -d' ' -f1) \
 && dfv=$(dpkg-query -W -f='${Version}' desktop-file-utils) \
 && gcc -std=c17 -O2 -Wall -Wextra -o /usr/local/bin/ini-driver driver.c \
        $(pkg-config --cflags --libs glib-2.0) \
        -DGTEXT_DRIVER_SHA="\"$sha\"" \
        -DGTEXT_DFV_VERSION="\"$dfv\"" \
 && gcc --version | head -1 \
 && /usr/local/bin/ini-driver --version \
 && printf -- '-1\n' | /usr/local/bin/ini-driver > /dev/null
