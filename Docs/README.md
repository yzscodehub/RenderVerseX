# Documentation map

RenderVerseX documentation has three explicit classes. The classification is
machine-readable in `engine-capabilities.json`; this page explains how to use
it.

## Current contracts

Only the files listed in `documentationClasses.currentContracts` describe the
implemented repository contract. They cover the build and test entry points,
module boundaries, capability status, asset cooking, and Shipping validation.
When code and prose disagree, the Manifest, configured CMake inventory,
Presets, capability JSON, and validation artifacts are authoritative.

## Future proposals

`Docs/RVX-NG/` contains forward-looking architecture proposals. A proposal is
not an implemented API, enabled product feature, or Shipping guarantee until
its capability entry and required evidence are updated in the current
contract.

## Historical plans and evidence

`Docs/superpowers/` and the remaining top-level Markdown files under `Docs/`
are historical plans, investigations, design notes, and phase evidence. Their
sample code and commands are preserved for traceability and must not be used
as the current integration contract.

Module entries whose `owner` is `unassigned` have no established team owner.
That value records missing ownership; it is not evidence of an existing
governance or approval process.
