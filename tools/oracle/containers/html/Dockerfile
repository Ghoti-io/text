# The HTML references: html5lib and parse5, in one image with one driver protocol.
#
# **Two implementations that share no code**, in two languages: html5lib is the
# Python parser the HTML5 test suites were written against, parse5 is the
# JavaScript one Node tooling (jsdom among them) uses. They disagree in places -
# html5lib was last released in 2020 and has not followed every later change to
# the standard - and a library scored against only one of them would inherit that
# one's gaps. Both are scored, and where they differ the standard decides.
#
# What is pinned, and what is only recorded:
#
#   pinned     the base image, by digest (the one containers/xml uses);
#   pinned     html5lib 1.1, webencodings 0.6.1 and six 1.17.0, by the SHA-256 of their wheels
#              (requirements.txt, installed with --require-hashes);
#   pinned     parse5 8.0.1 and its one dependency, by the integrity hashes in package-lock.json
#              (installed with `npm ci`);
#   pinned     the two driver files, by the SHA-256 of both together, which is compiled into the
#              version line and recomputed by the gate (html_ref.py): an image holding an older
#              copy of either would otherwise answer under a name that no longer describes it;
#   recorded   the Debian python3 and nodejs packages, installed with apt-get from the repositories
#              of the day: the driver prints their versions, IMAGES does not enforce them, and a
#              Debian point release changes the printed line and is seen, not refused. A snapshot
#              repository would pin them; that is not done here.
#
# The gate checks the printed line against containers/IMAGES (html5lib and parse5 versions) and the
# driver hash. There is no host mode: oracle_env.py refuses one.
#
# No network at run time: the packages are fetched here, once, and the container
# runs with --network none.
FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      python3 python3-venv nodejs npm ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/html-oracle
COPY requirements.txt package.json package-lock.json /opt/html-oracle/

RUN python3 -m venv /opt/html-oracle/venv \
 && /opt/html-oracle/venv/bin/pip install --no-cache-dir --require-hashes --only-binary :all: \
        -r requirements.txt \
 && npm ci --ignore-scripts --no-audit --no-fund

COPY driver.py driver.mjs /opt/html-oracle/

RUN sha=$(cat driver.py driver.mjs | sha256sum | cut -d' ' -f1) \
 && echo "$sha" > DRIVER_SHA \
 && echo "debian python3 $(dpkg-query -W -f='${Version}' python3), nodejs $(dpkg-query -W -f='${Version}' nodejs)" > PACKAGES \
 && printf '#!/bin/sh\nexec /opt/html-oracle/venv/bin/python3 /opt/html-oracle/driver.py "$@"\n' > /usr/local/bin/html-driver \
 && chmod +x /usr/local/bin/html-driver \
 && html-driver --version \
 && printf '%s\n' '{"html":"<p>a<table><td>b","scripting":false,"context":null}' > /tmp/req \
 && html-driver html5lib < /tmp/req > /tmp/h5 \
 && html-driver parse5 < /tmp/req > /tmp/p5 \
 && cat /tmp/h5 /tmp/p5 \
 && python3 -c "import json; a, b = (json.loads(open(f).read())['dump'] for f in ('/tmp/h5', '/tmp/p5')); assert a == b and a.count(chr(10)) == 9, (a, b)"
