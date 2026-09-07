#!/bin/sh
set -eu
mkdir -p build/tools
curl -fL --retry 3 https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage -o build/tools/linuxdeploy
curl -fL --retry 3 https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage -o build/tools/linuxdeploy-plugin-qt
printf '%s\n' 'c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d  build/tools/linuxdeploy' | sha256sum -c -
printf '%s\n' '15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724  build/tools/linuxdeploy-plugin-qt' | sha256sum -c -
chmod +x build/tools/linuxdeploy build/tools/linuxdeploy-plugin-qt
