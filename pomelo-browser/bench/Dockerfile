# 效能比較用的環境：Debian 13＋WebKitGTK 6.0（編譯小柚瀏覽器）＋Firefox ESR＋Chromium＋Xvfb
FROM debian:13
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends build-essential pkg-config \
      libwebkitgtk-6.0-dev libgtk-4-dev libx11-dev libxext-dev libxrandr-dev libcairo2-dev libjpeg-dev \
      xvfb xauth xdotool dbus dbus-x11 fonts-noto-cjk procps ca-certificates firefox-esr chromium \
 && rm -rf /var/lib/apt/lists/*
