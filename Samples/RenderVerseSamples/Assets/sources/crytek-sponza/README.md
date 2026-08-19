# Crytek Sponza source and deterministic derivative

- Upstream archive: `crytek-sponza-2016-06-28.zip`
- SHA-256: `da005cbee0be2df2abc8513f3ceb61bcb6f69aac112babcd9c00169a27c2770c`
- Extracted, unmodified source: `raw/`
- Runtime derivative: `../../models/crytek-sponza/`
- Conversion tool: `Scripts/Assets/convert_crytek_sponza.py`
- Tool version: `1.0.0`

Regenerate from the repository root with the bundled or system Python:

```text
python Scripts/Assets/convert_crytek_sponza.py \
  --source-root Samples/RenderVerseSamples/Assets/sources/crytek-sponza/raw \
  --output-root Samples/RenderVerseSamples/Assets/models/crytek-sponza
```

The converter refuses to overwrite an existing output. Remove or move the
derived directory only as part of an intentional asset-update review, then
independently compare every output hash and update Catalog v3/CookManifest v2.
