# The CSS references: tinycss2, the Servo cssparser crate and csstree, in one image with one driver
# protocol.
#
# **Three implementations that share no code**, in three languages: tinycss2 is the Python tokenizer
# and parser (the author of css-parsing-tests wrote both, so it is the one that conforms to that
# suite by construction), cssparser is the Rust crate Servo and Firefox parse CSS with (it is the
# tokenizer of two engines), and csstree is a JavaScript parser with its own tokenizer (the
# linter and analysis tools use it). They disagree in places - tinycss2 and cssparser follow a CSS
# Syntax draft older than the one this library follows, csstree predates CSS Nesting - and a library
# scored against only one of them would inherit that one's age. Each divergence is classified in
# tools/oracle/css_divergences.txt with the sentence of the served draft that decides it.
#
# What is pinned, and what is only recorded:
#
#   pinned     both base images, by digest (the Debian one containers/html uses; the Rust one the regex
#              library's Rust reference uses);
#   pinned     tinycss2 1.5.1 and webencodings 0.6.1, by the SHA-256 of their wheels (requirements.txt,
#              installed with --require-hashes);
#   pinned     csstree 3.2.1 and its two dependencies, by the integrity hashes in package-lock.json
#              (installed with `npm ci`);
#   pinned     cssparser =0.38.0 and everything it needs, by `=` in Cargo.toml and Cargo.lock
#              (built with --locked): a bare version is a caret requirement, which would let the image
#              drift on every rebuild;
#   pinned     the three driver sources, by the SHA-256 of all of them together, which is compiled into
#              the version line and recomputed by the gate (css_ref.py): an image holding an older copy
#              of any would otherwise answer under a name that no longer describes it;
#   recorded   the Debian python3 and nodejs packages, installed with apt-get from the repositories of
#              the day: the driver prints their versions, IMAGES does not enforce them, and a Debian
#              point release changes the printed line and is seen, not refused.
#
# The gate checks the printed line against containers/IMAGES (the three versions) and the driver
# hash. There is no host mode: oracle_env.py refuses one.
#
# No network at run time: the packages are fetched here, once, and the container runs with
# --network none.

FROM docker.io/library/rust@sha256:4cd829461bd5c4d511c32e269da9cb8929223b666519d8004e35fc8d1d771ab7 AS rust

ARG CSSPARSER_VERSION=0.38.0
ENV GHOTI_CSSPARSER_VERSION=${CSSPARSER_VERSION} CARGO_TERM_COLOR=never

WORKDIR /build
COPY driver-rs/Cargo.toml driver-rs/Cargo.lock /build/
COPY driver-rs/src /build/src
RUN grep -q "cssparser = \"=${CSSPARSER_VERSION}\"" Cargo.toml \
 && cargo build --release --locked \
 && ./target/release/cssparser-driver --version

FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      python3 python3-venv nodejs npm ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/css-oracle
COPY requirements.txt package.json package-lock.json /opt/css-oracle/

RUN python3 -m venv /opt/css-oracle/venv \
 && /opt/css-oracle/venv/bin/pip install --no-cache-dir --require-hashes --only-binary :all: \
        -r requirements.txt \
 && npm ci --ignore-scripts --no-audit --no-fund

COPY --from=rust /build/target/release/cssparser-driver /usr/local/bin/cssparser-driver
COPY driver.py driver.mjs driver-rs/Cargo.toml driver-rs/Cargo.lock driver-rs/src/main.rs /opt/css-oracle/src/

RUN cd /opt/css-oracle/src \
 && cp driver.py driver.mjs .. \
 && sha=$(cat driver.py driver.mjs Cargo.toml Cargo.lock main.rs | sha256sum | cut -d' ' -f1) \
 && echo "$sha" > /opt/css-oracle/DRIVER_SHA \
 && echo "debian python3 $(dpkg-query -W -f='${Version}' python3), nodejs $(dpkg-query -W -f='${Version}' nodejs)" > /opt/css-oracle/PACKAGES \
 && printf '#!/bin/sh\n\
case "$1" in\n\
  --version) echo "$(/opt/css-oracle/venv/bin/python3 /opt/css-oracle/driver.py --version), $(cssparser-driver --version), $(node /opt/css-oracle/driver.mjs --version); $(/opt/css-oracle/venv/bin/python3 /opt/css-oracle/driver.py --packages), $(cat /opt/css-oracle/PACKAGES); drivers $(cat /opt/css-oracle/DRIVER_SHA)" ;;\n\
  tinycss2) exec timeout 20 /opt/css-oracle/venv/bin/python3 /opt/css-oracle/driver.py tinycss2 ;;\n\
  cssparser) exec timeout 20 cssparser-driver ;;\n\
  csstree) cd /opt/css-oracle && exec timeout 20 node --max-old-space-size=300 driver.mjs csstree ;;\n\
  *) echo "css-driver: tinycss2 | cssparser | csstree | --version" >&2; exit 2 ;;\n\
esac\n' > /usr/local/bin/css-driver \
 && chmod +x /usr/local/bin/css-driver \
 && css-driver --version \
 && for ref in tinycss2 cssparser csstree; do \
      printf 'tokens - %s\n' "$(printf 'a{b:c} 12px' | od -An -tx1 | tr -d ' \n')" | css-driver $ref; \
    done
