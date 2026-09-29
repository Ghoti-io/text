# The git config reference: git itself.
#
# **One reference, and there is no second one to add.** git config has a single
# implementation, and the same program decides both whether a document is legal
# and what its values are - unlike Desktop Entry, where GKeyFile and
# desktop-file-validate disagree and both are needed. So this differential gets
# its second opinion from the generator's own intent instead, which
# tools/oracle/ini_git_diff.py scores as a reading of its own.
#
# The base is pinned by digest and git is a Debian package, so what pins the
# reference is that digest plus the version the driver prints at run time; the
# gate checks the printed line against the IMAGES entry. A Debian point release
# that moved git would change that line and fail the check rather than silently
# changing the comparison.
#
# The **compiler is deliberately not pinned**, for the reason
# containers/tomlpp/Dockerfile gives: what decides these answers is git's source,
# and a different gcc compiling this driver does not read a config file
# differently. The driver only frames the protocol and shells out.
#
# The driver's own SHA-256 is compiled into it and checked by the gate, because an
# image carrying a driver built from an older copy of driver.c would answer
# confidently and wrongly - the stale-artifact failure this repository keeps
# meeting.
FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc libc6-dev git \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY driver.c /build/driver.c

RUN sha=$(sha256sum driver.c | cut -d' ' -f1) \
 && gitver=$(dpkg-query -W -f='${Version}' git) \
 && gcc -std=c17 -O2 -Wall -Wextra -o /usr/local/bin/git-config-driver driver.c \
        -DGTEXT_DRIVER_SHA="\"$sha\"" \
        -DGTEXT_GIT_VERSION="\"$gitver\"" \
 && gcc --version | head -1 \
 && /usr/local/bin/git-config-driver --version \
 && printf -- '-1\n' | /usr/local/bin/git-config-driver > /dev/null
