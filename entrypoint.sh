#!/bin/bash

APP_UID=${APP_UID:-1000}
APP_GID=${APP_GID:-1000}

if [ ! -e /dev/video0 ]; then
    echo "/dev/video0 not found"
    exit 1
fi

if [ ! -e /dev/dri/renderD128 ]; then
    echo "/dev/dri/renderD128 not found"
    exit 1
fi

VIDEO_GID=$(stat -c '%g' /dev/video0)
VIDEO_GROUP=$(awk -F: '$3 == '$VIDEO_GID' { print $1 }' /etc/group)
if [ -z "$VIDEO_GROUP" ]; then
    groupadd -g $VIDEO_GID video0
    VIDEO_GROUP=video0
fi

groupadd -g $APP_GID app
useradd -d /app -g app -G app,$VIDEO_GROUP -s /sbin/nologin -u $APP_UID app

_term() {
  kill -TERM "$app" 2>/dev/null
  exit 0
}

trap _term SIGTERM SIGQUIT SIGINT

export USER=app
export HOME=/app
export PT_NOENV=1

while :; do
    setpriv --reuid app --regid app --init-groups --inh-caps=-all /app/doorcam &
    app=$!
    wait
    sleep 1
done
