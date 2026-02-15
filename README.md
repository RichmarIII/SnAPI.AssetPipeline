# SnAPI.AssetPipeline

SnAPI.AssetPipeline is the asset cooking, packing, and runtime loading module for the SnAPI ecosystem.
It defines canonical asset identities, serializers/factories, and package formats used by higher-level
modules such as SnAPI.GameFramework.

## Pre-Refactor Checkpoint (2026-02-15)

This repository is part of the cross-module checkpoint taken before the planned renderer-side
`IRenderObject` refactor.

- This commit is intended as a rollback-safe baseline.
- Existing asset formats and integration contracts remain unchanged in this checkpoint.
- Module docs will be updated again once the refactor lands.
