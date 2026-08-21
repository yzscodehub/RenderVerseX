# Shipping validation contract

Shipping validation uses the configured CMake target inventory rather than
target-name conventions or source-directory guesses.

`Scripts/check_shipping_target_closure.py` requires both
`RVXShippingProduct` and the real executable `RVXShippingConsumer`. The
consumer may be owned by the Tests module only at the root. Every dependency
reachable below it, and every target reachable from the product root, must be
owned by a Manifest module whose domain is `runtime` and whose
`shippingAllowed` field is true. Unknown first-party links, missing roots and
unresolvable target sources fail closed.

The target closure also rejects source-side importers, parsers, source change
monitors and legacy mixed source/cooked loader translation units. A cooked
loader must have its own source file; a runtime policy check that merely
rejects `ResourceLoadDomain::Source` is not sufficient evidence that the
parser code is absent from the product.

Reachable targets' raw link and include declarations are also checked for
`tinygltf`, `tinyexr`, `ufbx`, `stb_image`, `assimp`, and `miniz`, plus
Developer/Editor module tokens. This closes the header-only and static-library
escape that a source-file scan cannot see. Ordinary generated includes below
the configured binary root remain valid.

Example:

```text
python Scripts/check_shipping_target_closure.py \
  --root . \
  --manifest Docs/module-boundaries.json \
  --module-inventory build/<preset>/Architecture/RVXModuleInventory-Release.json
```

`Scripts/check_shipping_dynamic_dependencies.py` inspects the final consumer.
Windows PE import and delay-import tables are parsed directly. Linux uses
`readelf --dynamic --wide`; macOS uses `otool -L`. The checked-in policy first
rejects known compiler/importer/editor/test dependencies and then requires
every remaining direct dependency to match a platform allowlist. Product-
specific runtime libraries must be passed as exact `--allow` entries; broad
regular expressions are not accepted on the command line.

```text
python Scripts/check_shipping_dynamic_dependencies.py \
  --root . \
  --binary build/<preset>/<config>/RVXShippingConsumer.exe \
  --platform windows \
  --allow glfw3.dll \
  --allow spdlog.dll
```

Missing inspection tools, malformed binaries, a requested platform that does
not match the binary format, invalid policy data and unapproved dependencies
all produce a non-zero result. The scan covers direct dynamic dependencies;
the target closure remains the authority for statically linked first-party
code.

## Cooked-only fixture

`Scripts/run_shipping_fixture.cmake` must be run with the host `RVXCook`, not a
target-platform executable. The output root must be below a configured CMake
build tree. The script copies the hermetic
`Tests/Fixtures/ModelViewer/R7Triangle.gltf` input into a private staging
directory, cooks `Cooked/Resources/R7Triangle.rva` and its v2 manifest, then
cooks every program selected for the requested backend by
`Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json`. The source input is removed
before publication. A previous valid fixture is kept in place until every
output has been checked and the staged directory can be atomically renamed.

```text
cmake \
  -DRVX_COOK_EXECUTABLE=<absolute-host-RVXCook> \
  -DRVX_SOURCE_ROOT=<absolute-repository-root> \
  -DRVX_BINARY_FIXTURE_ROOT=<absolute-shipping-build-root>/Tests/Shipping/ShippingFixtures/Release \
  -DRVX_BACKEND=DX12 \
  -DRVX_COMMIT=<7-to-64-digit-hex-commit> \
  -DRVX_PRESET=win_x64_shipping_release \
  -P Scripts/run_shipping_fixture.cmake
```

The published `shipping-fixture-receipt.json` is schema
`RVX_SHIPPING_FIXTURE_RECEIPT` version 1. It binds the commit, preset, backend,
source model, complete shader catalog, host cooker hash, resource/shader
counts, every artifact's size and SHA-256, and a deterministic hash of the
sorted artifact set. The receipt and all cooked output are build evidence and
must never be committed.

## Runtime evidence

The Shipping consumer writes one `RVX_SHIPPING_EVIDENCE` version 2 JSON report.
It is deliberately a closed schema: unknown and missing fields fail. The
required run is exactly eight frames at 320x180, Low quality, Direct rendering.
The requested and actual backend must match, fallback must be false with an
empty reason, cooked loading must succeed, a Source-domain request must be
rejected, and shutdown must be clean. Software adapters are recorded by name
and are accepted only when `RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER` was explicitly
enabled by the validation environment.

```text
python Scripts/check_shipping_evidence.py \
  --root . \
  --report build/<shipping-preset>/Tests/Shipping/ShippingFixtures/Release/shipping-smoke.json \
  --receipt build/<shipping-preset>/Tests/Shipping/ShippingFixtures/Release/shipping-fixture-receipt.json \
  --expected-backend DX12 \
  --expected-commit <commit> \
  --expected-preset win_x64_shipping_release
```

The checker recalculates the receipt SHA-256, every cooked artifact's size and
hash, the sorted artifact-set hash, and the current repository hashes of the
fixed model and shader-catalog inputs. A report from another commit, preset,
backend, fixture, or cooker-produced artifact set cannot satisfy the lane.
