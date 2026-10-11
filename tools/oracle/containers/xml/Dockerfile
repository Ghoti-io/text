# The XML references: libxml2 and expat, in one image with one driver.
#
# **Two implementations that share no code**, which is what makes agreement
# between them worth something and disagreement worth reading. libxml2 is the C
# parser from GNOME and expat is James Clark's; neither links or includes the
# other. They are asked together because they disagree with each other in places
# - libxml2 is lenient about a few things the specification forbids and expat is
# strict about a few things libxml2 never checked - and a library scored against
# only one of them would inherit that one's gaps. Both are scored, and where they
# differ the disagreement is read against the specification.
#
# The base is pinned by digest and the two libraries are Debian packages, so what
# pins the references is that digest plus the version line the driver prints at
# run time; the gate checks it against the IMAGES entry. A Debian point release
# that moved either library changes the line and fails the check rather than
# silently changing the comparison. Debian's "libxml2 2.9.14" is a
# `2.12.7+dfsg+really2.9.14` package: the version the library reports about
# itself is the one that decides its answers, and it is the one pinned here.
#
# The compiler is deliberately not pinned, for the reason containers/tomlpp/
# Dockerfile gives. The driver's own SHA-256 is compiled into it and checked,
# because an image carrying a driver built from an older driver.c would answer
# confidently and wrongly.
FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc libc6-dev pkg-config libxml2-dev libexpat1-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY driver.c /build/driver.c

RUN sha=$(sha256sum driver.c | cut -d' ' -f1) \
 && pkgs="debian libxml2 $(dpkg-query -W -f='${Version}' libxml2), expat $(dpkg-query -W -f='${Version}' libexpat1)" \
 && gcc -std=gnu17 -O2 -Wall -Wextra -o /usr/local/bin/xml-driver driver.c \
        $(pkg-config --cflags --libs libxml-2.0 expat) \
        -DGTEXT_DRIVER_SHA="\"$sha\"" \
        -DGTEXT_XML_PACKAGES="\"$pkgs\"" \
 && gcc --version | head -1 \
 && /usr/local/bin/xml-driver --version \
 && doc='<a xmlns:p="u" p:x="1">t</a>' \
 && printf '%s\n%s-1\n' "$(printf '%s' "$doc" | wc -c)" "$doc" | /usr/local/bin/xml-driver | tee /tmp/smoke \
 && grep -q '^EXPAT-NS ok$' /tmp/smoke && grep -q '^XML2 1 1 ' /tmp/smoke
