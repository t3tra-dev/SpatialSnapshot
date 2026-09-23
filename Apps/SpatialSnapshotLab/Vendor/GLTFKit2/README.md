# GLTFKit2 0.5.15

This package downloads the official, unmodified XCFramework from the
[0.5.15 release](https://github.com/warrenm/GLTFKit2/releases/tag/0.5.15).

Archive: `GLTFKit2.xcframework.zip`
SHA-256: `9d0c338282acce4986494aa02a5f1495278f56c60d43f31453fefea6875b4928`

The URL and checksum are pinned in `Package.swift` and match the
[upstream manifest](https://github.com/warrenm/GLTFKit2/blob/0.5.15/Package.swift).
SwiftPM downloads and verifies the archive when resolving packages. The first build
needs network access; subsequent builds can reuse the downloaded artifact until its
cache is removed. Xcode and command-line builds use their own package caches.

Only this manifest and the license notices are distributed in the repository.
The approximately 87 MiB extracted XCFramework is not committed, stored in Git LFS,
or republished as a SpatialSnapshot release asset. The upstream release is the binary
distribution source. To update it, change the versioned URL and upstream checksum
together, then run the macOS tests and all three Lab builds in CI.

GLTFKit2 is an app dependency; it is not a dependency of the portable SpatialSnapshot runtime.

GLTFKit2, cgltf, and cgltf's embedded JSMN use the MIT license. Their notices are in
`LICENSE`, `LICENSE-cgltf.txt`, and `LICENSE-jsmn.txt`, respectively.
The JSMN notice is reproduced from `GLTFKit2/deps/cgltf/cgltf.h` in upstream tag 0.5.15.
The app also bundles these notices as `Resources/ThirdPartyNotices.txt`.
