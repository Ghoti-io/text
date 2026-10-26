# The CSS property oracle: Chromium (Blink) and Firefox (Gecko, whose style engine is Stylo), driven through WebDriver BiDi by
# one small driver (driver.mjs).
#
# **Two engines that share no code.** Blink's style engine and Gecko's Stylo are two implementations of CSS; Stylo is the code that
# Servo's `stylo`, `cssparser` and `selectors` crates publish, so Servo would add no engine to Gecko and no Servo driver is built.
# Playwright's patched Firefox and WebKit builds are not oracles: they are the vendor's browsers with changes this library has not
# read. Each answer of the differential is what the stock browser says; where they disagree with each other the served draft
# decides (tools/oracle/css_prop_divergences.txt).
#
# What is pinned, and what is only recorded:
#
#   pinned     the base image, by digest (the Debian one the other oracle images use);
#   pinned     Chrome for Testing 156.0.8078.47 (revision 1706539, the Stable of 2026-10-19) and the chromedriver of the same
#              build, by the SHA-256 of their zips. The bucket publishes an MD5 of each, and both hashes below were taken from
#              a download that matched it (chrome-linux64.zip cvpoYXzFXs9OK75pfi91hA==, chromedriver-linux64.zip
#              v1opVaA+SY8LhMwO7EnR4A==, base64 as the bucket's x-goog-hash header gives them);
#   pinned     stock Firefox 158.0.1 from Mozilla's release archive, by the SHA-256 Mozilla publishes in the release's SHA256SUMS
#              (linux-x86_64/en-US/firefox-158.0.1.tar.xz);
#   pinned     geckodriver 0.37.1, by the SHA-256 GitHub records for the release asset geckodriver-v0.37.1-linux64.tar.gz;
#   pinned     the WebSocket client (`ws` 8.22.0), by the integrity hash in package-lock.json (installed with `npm ci`);
#   pinned     the driver, by the SHA-256 of driver.mjs, package.json and package-lock.json together, written to DRIVER_SHA and
#              printed by `cssprop-driver --version`: the gate recomputes it from the repository's files (css_prop_diff.py);
#   measured   the versions: `cssprop-driver --version` and the header of every answer run each vendor binary with `--version` and read the
#              installed `ws` package, so the printed line is what is installed and not a file the build wrote;
#   recorded   the four archive hashes above, written to /opt/cssprop/PINS from the build arguments after `sha256sum -c` passed on them;
#              they are printed by `--version` and in the header of every answer, and containers/IMAGES names the same four;
#   recorded   the Debian nodejs package and the libraries the browsers link, installed with apt-get from the repositories of the
#              day: the driver prints node's version, IMAGES does not enforce it, and a point release changes the printed line
#              and is seen, not refused.
#
# The gate checks the printed line against containers/IMAGES (the five versions and the four hashes) and the driver hash, and refuses to
# classify answers (a replay too) whose header names other versions or hashes. There is no host mode:
# oracle_env.py refuses one. No network at run time: everything is fetched here, once, and the container runs with --network none
# (the loopback sockets between the driver and the vendor's driver are all it needs).

FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

ARG CHROME_VERSION=156.0.8078.47
ARG CHROME_SHA256=e6c94490f0b6b337225aeb2d2a5034858516b7017356980ba94f5176d8774a50
ARG CHROMEDRIVER_SHA256=2a5024610c53f340a1aaa5d9a3e2a4fa448be7d71cca5238bd8c3c7f16181df4
ARG FIREFOX_VERSION=158.0.1
ARG FIREFOX_SHA256=dce45470139242cf294eb6ada3d192a9bb334696842b02b71ade448a139eb41a
ARG GECKODRIVER_VERSION=0.37.1
ARG GECKODRIVER_SHA256=e815130ea95983e162ae91843b48d3a3ce991735635fce83a647afde21e09f7e

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl unzip xz-utils nodejs npm fonts-liberation libasound2 libatk-bridge2.0-0 \
      libatk1.0-0 libatspi2.0-0 libcairo2 libcups2 libdbus-1-3 libdbus-glib-1-2 libdrm2 libexpat1 \
      libfontconfig1 libgbm1 libglib2.0-0 libgtk-3-0 libnspr4 libnss3 libpango-1.0-0 libu2f-udev \
      libvulkan1 libx11-6 libx11-xcb1 libxcb1 libxcomposite1 libxdamage1 libxext6 libxfixes3 \
      libxkbcommon0 libxrandr2 libxt6 \
 && rm -rf /var/lib/apt/lists/*

RUN set -eu; cd /tmp \
 && curl -fsSL --retry 3 "https://storage.googleapis.com/chrome-for-testing-public/${CHROME_VERSION}/linux64/chrome-linux64.zip" -o chrome.zip \
 && echo "${CHROME_SHA256}  chrome.zip" | sha256sum -c - \
 && curl -fsSL --retry 3 "https://storage.googleapis.com/chrome-for-testing-public/${CHROME_VERSION}/linux64/chromedriver-linux64.zip" -o chromedriver.zip \
 && echo "${CHROMEDRIVER_SHA256}  chromedriver.zip" | sha256sum -c - \
 && curl -fsSL --retry 3 "https://archive.mozilla.org/pub/firefox/releases/${FIREFOX_VERSION}/linux-x86_64/en-US/firefox-${FIREFOX_VERSION}.tar.xz" -o firefox.tar.xz \
 && echo "${FIREFOX_SHA256}  firefox.tar.xz" | sha256sum -c - \
 && curl -fsSL --retry 3 "https://github.com/mozilla/geckodriver/releases/download/v${GECKODRIVER_VERSION}/geckodriver-v${GECKODRIVER_VERSION}-linux64.tar.gz" -o geckodriver.tar.gz \
 && echo "${GECKODRIVER_SHA256}  geckodriver.tar.gz" | sha256sum -c - \
 && unzip -q chrome.zip -d /opt && mv /opt/chrome-linux64 /opt/chrome \
 && unzip -q chromedriver.zip -d /opt && mv /opt/chromedriver-linux64 /opt/chromedriver \
 && tar -xJf firefox.tar.xz -C /opt \
 && mkdir /opt/geckodriver && tar -xzf geckodriver.tar.gz -C /opt/geckodriver \
 && rm -f chrome.zip chromedriver.zip firefox.tar.xz geckodriver.tar.gz \
 && /opt/chrome/chrome --version | grep -q "^Google Chrome for Testing ${CHROME_VERSION}" \
 && /opt/chromedriver/chromedriver --version | grep -q "^ChromeDriver ${CHROME_VERSION} " \
 && /opt/firefox/firefox --version | grep -q "^Mozilla Firefox ${FIREFOX_VERSION}$" \
 && /opt/geckodriver/geckodriver --version | grep -q "^geckodriver ${GECKODRIVER_VERSION} "

WORKDIR /opt/cssprop
COPY package.json package-lock.json driver.mjs /opt/cssprop/

RUN npm ci --ignore-scripts --no-audit --no-fund \
 && sha=$(cat driver.mjs package.json package-lock.json | sha256sum | cut -d' ' -f1) \
 && echo "$sha" > /opt/cssprop/DRIVER_SHA \
 && echo "chrome=${CHROME_SHA256} chromedriver=${CHROMEDRIVER_SHA256} firefox=${FIREFOX_SHA256} geckodriver=${GECKODRIVER_SHA256}" > /opt/cssprop/PINS \
 && echo "debian nodejs $(dpkg-query -W -f='${Version}' nodejs)" > /opt/cssprop/VERSIONS \
 && printf '#!/bin/sh\ncd /opt/cssprop\nexec node /opt/cssprop/driver.mjs "$@"\n' > /usr/local/bin/cssprop-driver \
 && chmod +x /usr/local/bin/cssprop-driver \
 && cssprop-driver --version

ENV HOME=/tmp MOZ_HEADLESS=1
