# RenderQualityMetricsValidation

The executable-free GFX24 qualification contract is deliberately implemented
as hermetic Python validation tools until the RenderQualityLab capture owner is
wired into the CTest fixture graph.  Its executable self-test is:

```powershell
py Scripts/test_render_quality_qualification.py
```

The test creates synthetic, temporary **input fixtures only**.  It does not
produce a golden image, claim a GPU result, or establish a performance
baseline.  The visual checker requires a public sample manifest as well as the
bundle, policy, source commit, run id, preset, and current receipt digest; it
derives every per-surface integer ROI from that manifest's normalized region.
Temporal evidence may advance only through that sample's declared, strictly
increasing `captureTicks`; all other qualification identity fields stay exact.
Native fixture ownership and approval of goldens/baselines remain GFX24
composition responsibilities, not a result of this hermetic test.
