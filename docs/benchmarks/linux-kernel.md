# Linux Kernel Benchmark

## Environment
- **Machine**: Mac15,11 (Apple Silicon)
- **CPU**: 14 cores
- **RAM**: 36 GB
- **OS**: macOS
- **Date**: 2026-07-05
- **Binary**: ARM64, optimized release build

## Results

| Metric | Value |
|--------|-------|
| Files indexed | 64,694 |
| Total nodes | 12,029,605 |
| Functions | 3,840,680 |
| CALLS edges | 3,727,864 |
| Cross-file CALLS | 1,502,432 (40%) |
| DB size | ~1.2 GB |
| Index time | ~3 min 7 sec |
| Peak memory | ~3 GB |
| Workers | 14 |

## Architecture Improvements

| Change | Before | After |
|--------|--------|-------|
| engine.cpp size | 3,499 lines | 48 lines (split into 6 files) |
| Cross-file resolution | ✗ | 1.5M edges |
| Pipeline | monolithic 2-phase | Translte→Link→Emit 4-phase |
| Linux kernel indexing | SIGILL crash | ✅ 64K files in 3 min |
