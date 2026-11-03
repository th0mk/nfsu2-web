#!/bin/bash
# Keeps the local hub alive: WSL kills the processes of a session when its last wsl.exe ends, so yuzu-online-test.ps1 starts THIS with a
# hidden, long-lived wsl.exe.   hub-keep.sh [friends=0] [port=8765] [seconds=900]
bash "$(dirname "$0")/start-local-roam.sh" "${1:-0}" "${2:-8765}" > /tmp/hub-start.log 2>&1
sleep "${3:-900}"
