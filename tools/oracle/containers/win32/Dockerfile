# The Win32 profile-API reference: wine's kernel32, asked through
# GetPrivateProfileString, GetPrivateProfileSection and
# GetPrivateProfileSectionNames.
#
# **This is the one reference in this module that is not the thing it stands
# for.** wine is a reimplementation of the Windows API, not Windows, and no probe
# run here can establish what a Microsoft kernel32 does. Two of this dialect's
# rules are corroborated independently by Microsoft's own documentation - quote
# stripping and case insensitivity - and the other twenty-eight rest on wine
# alone. tools/oracle/ini_win32_diff.py prints that limit with every run rather
# than leaving it in a comment, because a differential whose reference might not
# be the reference has to say so where the number is read.
#
# It is also the only reference here that **cannot be made to agree with an
# application**: GetPrivateProfileString consults the registry's IniFileMapping
# for the section and reads the file only "if there is no subkey or entry for the
# section name". This image has an empty registry for those keys, so it reads the
# file - which is the case this dialect is for, and is not every case.
#
# Two toolchains, and they pin different things. mingw-w64 cross-compiles the
# driver and is **deliberately not pinned** beyond the base digest, for the reason
# containers/gitconfig/Dockerfile gives: what decides these answers is wine's
# implementation of the profile API, and a different compiler emitting the same
# call does not read a file differently. wine *is* pinned, by the base digest plus
# the version the driver prints, which the gate checks against the IMAGES entry.
#
# The driver's own SHA-256 is compiled into it and checked by the gate, because an
# image carrying a driver built from an older copy of driver.c would answer
# confidently and wrongly.
FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

# wine64 rather than the `wine` metapackage: the metapackage pulls wine32 and with
# it a foreign architecture, which needs dpkg --add-architecture and doubles the
# image for a 32-bit loader nothing here calls.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc-mingw-w64-x86-64 wine64 ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY driver.c /build/driver.c

RUN sha=$(sha256sum driver.c | cut -d' ' -f1) \
 && winever=$(dpkg-query -W -f='${Version}' wine64) \
 && x86_64-w64-mingw32-gcc -std=c17 -O2 -Wall -Wextra -pedantic-errors \
        -o /usr/local/bin/win32-profile-driver.exe driver.c \
        -DGTEXT_DRIVER_SHA="\"$sha\"" \
        -DGTEXT_WINE_VERSION="\"$winever\"" \
 && x86_64-w64-mingw32-gcc --version | head -1

# A writable prefix built at image time, so that a run does not pay for wine's
# first-start bootstrap and does not need a writable home. WINEDEBUG silences the
# fixme stream, which otherwise interleaves with the driver's stdout - the
# two-streams-interleave failure this repository has met before.
# The prefix is built here so a run does not pay wine's first-start bootstrap.
# `wine64` is the loader's real path: the `wine` wrapper script belongs to the
# `wine` metapackage, which is the one this image deliberately does not install.
ENV WINEDEBUG=-all WINEDLLOVERRIDES=mscoree=d XDG_RUNTIME_DIR=/tmp/xdg
RUN mkdir -p /wine /tmp/xdg \
 && WINEPREFIX=/wine /usr/lib/wine/wine64 wineboot --init 2>/dev/null; true
RUN WINEPREFIX=/wine /usr/lib/wine/wine64 \
      /usr/local/bin/win32-profile-driver.exe --version

COPY entry.sh /usr/local/bin/entry.sh
ENTRYPOINT ["/usr/local/bin/entry.sh"]
