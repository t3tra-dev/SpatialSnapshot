#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "${1:-macOS}" in
    macOS) destination='platform=macOS'; output=lab-mac ;;
    iOS) destination='generic/platform=iOS'; output=lab-device ;;
    simulator) destination='generic/platform=iOS Simulator'; output=lab-ios ;;
    *) echo "Usage: sh Scripts/build-lab.sh [macOS|iOS|simulator]" >&2; exit 2 ;;
esac
xcodebuild -project "$task_root/Apps/SpatialSnapshotLab/SpatialSnapshotLab.xcodeproj" \
    -scheme SpatialSnapshotLab -configuration Debug -destination "$destination" \
    -derivedDataPath "$task_root/.build/$output" \
    -clonedSourcePackagesDirPath "$task_root/.build/$output-packages" \
    CODE_SIGNING_ALLOWED=NO build
