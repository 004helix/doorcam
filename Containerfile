FROM fedora:36 AS base

RUN \
  dnf install -y \
    https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm \
    https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm && \
  dnf install -y --exclude=proj-data-* \
    util-linux \
    turbojpeg \
    opencv \
    ffmpeg \
    libevent \
    intel-compute-runtime \
    libva-utils \
    libva-intel-driver \
    intel-media-driver \
    ocl-icd \
    python3 \
    python3-yaml \
    python3-requests \
    python3-setproctitle && \
  dnf clean all

FROM base AS build

COPY . /app
RUN \
  dnf install -y --exclude=proj-data-* \
    opencv-devel \
    ffmpeg-devel \
    libevent-devel \
    libva-devel \
    ocl-icd-devel \
    gcc-c++ \
    cmake \
    meson \
    chrpath \
    git && \
  dnf clean all && \
  find /app -depth -type d -name __pycache__ -exec rm -rf {} \; && \
  cd /app/src && git clone https://github.com/004helix/ffmpjpeg-httpd.git && \
  cd ffmpjpeg-httpd && make && mv ffmpjpeg-httpd ../../bin && \
  cd /app/src/doorcam && rm -rf build && meson build && ninja -C build && \
  mv build/libqrscan.so /app/lib && \
  mv build/libmotion.so /app/lib && \
  mv build/libv4l2mjpg.so /app/lib && \
  mv lib/libDynamsoftBarcodeReader.so /app/lib && \
  strip /app/lib/libqrscan.so && \
  strip /app/lib/libmotion.so && \
  strip /app/lib/libv4l2mjpg.so && \
  strip /app/lib/libDynamsoftBarcodeReader.so && \
  chrpath -d /app/lib/libqrscan.so && \
  chrpath -d /app/lib/libmotion.so && \
  chrpath -d /app/lib/libv4l2mjpg.so && \
  chrpath -d /app/lib/libDynamsoftBarcodeReader.so && \
  rm -rf /app/src /app/Containerfile /app/entrypoint.sh && \
  chown -R 1000:1000 /app

FROM base
LABEL maintainer "Raman Shyshniou <rommer@ibuffed.com>"

COPY --from=build /app /app
COPY entrypoint.sh /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]