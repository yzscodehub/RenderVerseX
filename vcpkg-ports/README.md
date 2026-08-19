# Project-owned vcpkg overlays

This directory contains narrowly scoped replacements for ports from the pinned
vcpkg registry. The project-level `vcpkg-configuration.json` is the single
authoritative entry point, so local presets and every CI host resolve the same
ports.

## tinygltf 2.9.7

The registry baseline
`e05ecb1e80f9c3303220699a4956a2fcf5d6ec8d` resolves tinygltf 2.9.7 through
the mutable GitHub tag archive URL. That archive is now served with SHA512
`553c7ad329da5a4d46235747db9d937957d5698e74c8d1751c17da5a1d09d35f4212e2476652a63ee1a12ff74220531b5e288eaaddb1014d47000a82d30f03a2`, while the
registry port expects
`48075f77ff2d2c06688dec7b755faa42c7628559299ac05070eb505add826073f441f370fe1b805b39920788fa6129b6f98c9ed4b2e899dafcc67ea62a8f93d4`.

This overlay preserves the source version and pins the release to the immutable
commit `488a70a3df62a4df1a736e9e56fb8836580c4888`. The commit archive was verified
before recording its SHA512 in the port:

- commit tree: `98b1716228dbb12aa20f015fcb75691b96267390`
- `tiny_gltf.h` Git blob: `3e633857abe163dedc115151e5bb2ede5405af12`
- `LICENSE` Git blob: `34398adf07246224f14a9059a83bbbbbab008c9c`

Remove this overlay only after moving the registry baseline to a tinygltf port
whose source archive passes integrity validation and whose API compatibility has
passed the Resource importer and cross-platform Build Truth gates.
