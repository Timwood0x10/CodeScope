# Token Savings Report

> CodeScope vs. reading raw source files — measured across real analysis scenarios.
> Hardware: Apple M3 Max, 36 GB RAM. OS: macOS.

## Measured Savings

| Scenario | CodeScope (tokens) | Raw Source (tokens) | Savings |
|----------|-------------------|---------------------|---------|
| Find function definition | ~21 | ~2,265 | **99.1%** |
| Trace callers | ~18 | ~2,000 | **99.1%** |
| Architecture overview | ~32 | ~1,875 | **98.3%** |
| Function analysis | ~43 | ~4,733 | **99.1%** |
| Symbol search | ~23 | ~958 | **97.6%** |
| USB driver subsystem | ~250 | ~24,000 | **99.0%** |
| Linux kernel scheduler | ~180 | ~15,000 | **98.8%** |
| COW process trace | ~85 | ~8,500 | **99.0%** |
| **Average** | **~81** | **~7,416** | **98.9%** |

Chinese version: `docs/zh/token_savings_report.md`
