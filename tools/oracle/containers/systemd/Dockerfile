# The systemd reference: systemd's own `systemd-analyze verify`.
#
# **One reference, and an instrument that answers a different question than it
# looks like it answers.** `systemd-analyze verify` exits **0** on a syntax error -
# it warns and skips the line, and reserves its exit status for *semantic* failure
# ("Service has no ExecStart="). So the oracle is the diagnostics, classified into
# grammar and semantic, and tools/oracle/ini_sd_diff.py says which strings are
# which. Reading the exit status would have scored every document as legal.
#
# **Values come back through `Environment=`**, which validates each word as
# `NAME=VALUE` and reports a failure verbatim - after unquoting, unescaping and word
# splitting. No verb prints a parsed setting, so that diagnostic is the only channel
# there is, and it is systemd's entire value grammar echoed back one word per line.
#
# Two limits of that channel, both measured and both handled by the gate rather
# than hoped away: it **rewrites CR to LF** (the four spellings of a carriage return
# all arrive as 0x0a while an 0x0e escape arrives as 0x0e, so the rewriting is the
# logger's) and it **truncates at 2,097 bytes**. Documents whose words would hit
# either are excluded and counted. notes/text/INI-DIALECTS.md section A.17 has the
# transcript.
#
# **This machine has no systemd at all** - no `systemd-analyze`, no `systemctl`,
# PID 1 is `init` - while carrying 164 unit files shipped by other packages. So the
# corpus is local and the reference cannot be, which is the reverse of the Desktop
# Entry pin and the reason this one had to be built before anything could be
# measured.
#
# The base is pinned by digest and systemd is a Debian package, so what pins the
# reference is that digest plus the version the driver prints, which the gate checks
# against containers/IMAGES. The compiler is deliberately not pinned: what decides
# these answers is systemd's source, and the driver only frames the protocol.
FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc libc6-dev systemd \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY driver.c /build/driver.c

RUN sha=$(sha256sum driver.c | cut -d' ' -f1) \
 && sdver=$(dpkg-query -W -f='${Version}' systemd) \
 && gcc -std=c17 -O2 -Wall -Wextra -o /usr/local/bin/systemd-unit-driver driver.c \
        -DGTEXT_DRIVER_SHA="\"$sha\"" \
        -DGTEXT_SD_VERSION="\"$sdver\"" \
 && gcc --version | head -1 \
 && /usr/local/bin/systemd-unit-driver --version
