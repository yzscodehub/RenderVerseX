# Legacy ModelViewer validation harness

The `ModelViewer` target is retained temporarily for compatibility with the
existing rendering validation matrix. It is not the product-facing model
inspection sample.

Use the unified sample host for normal model inspection:

```text
RenderVerseSamples --sample model-viewer --model <path>
RenderVerseSamples --sample model-viewer --model <path> --environment-file <path.hdr|path.exr>
RenderVerseSamples --sample model-viewer --asset <model-id> --environment <environment-id> --catalog <catalog.json> --asset-root <directory>
```

The legacy executable still owns historical test-only switches for GPU-driven
qualification, ray-tracing budgets and history, particles, resize tests, and
special evidence reports. Those capabilities must move to dedicated samples or
validation executables before this target can become a thin compatibility
wrapper or be removed.

New general-purpose ModelViewer behavior must be implemented in
`Samples/RenderVerseSamples/Scenes/ModelViewerSample.*` and shared host services,
not added to this legacy `main.cpp`.
