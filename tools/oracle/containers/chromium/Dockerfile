FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

ARG CHROME_VERSION=155.0.8059.84
ARG CHROME_SHA256=cf0f049892dea22e6a6db26d47ed0d7776b7158edcbec4634086633df8bafcbc

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl unzip fonts-liberation libasound2 libatk-bridge2.0-0 \
      libatk1.0-0 libatspi2.0-0 libcairo2 libcups2 libdbus-1-3 libdrm2 \
      libexpat1 libfontconfig1 libgbm1 libglib2.0-0 libgtk-3-0 libnspr4 \
      libnss3 libpango-1.0-0 libu2f-udev libvulkan1 libx11-6 libxcb1 \
      libxcomposite1 libxdamage1 libxext6 libxfixes3 libxkbcommon0 libxrandr2 \
      python3 \
 && rm -rf /var/lib/apt/lists/* \
 && curl -fsSL --retry 3 \
      "https://storage.googleapis.com/chrome-for-testing-public/${CHROME_VERSION}/linux64/chrome-linux64.zip" \
      -o /tmp/chrome.zip \
 && echo "${CHROME_SHA256}  /tmp/chrome.zip" | sha256sum -c - \
 && unzip -q /tmp/chrome.zip -d /opt \
 && mv /opt/chrome-linux64 /opt/chrome \
 && rm /tmp/chrome.zip \
 && /opt/chrome/chrome --version

ENV CHROMIUM=/opt/chrome/chrome
WORKDIR /workspace
