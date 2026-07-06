# CodeScope Benchmarks Archive

This directory contains performance benchmark reports and project analysis results.

## Document Structure

### Chinese Reports (Original)
- `large_project_index_report.md` - Large project indexing benchmarks (rustc, Bun, JDK, Linux kernel)
- `timbre_model_analysis.md` - InstrumentTimbre model architecture analysis
- `linux-kernel.md` - Linux kernel benchmark results (English)

### English Translations
All English versions are available in `../en/` directory:
- `en/large_project_index_report.md` - English translation
- `en/timbre_model_analysis.md` - English translation
- `en/linux-analysis.md` - Linux kernel analysis (English)

## Benchmark Coverage

| Project | Files | Language | Index Time | Nodes | Status |
|---------|-------|----------|------------|-------|--------|
| rustc | 36,807 | Rust | 1m44s | 4.13M | ✅ |
| Bun | 9,641 | Zig/C++/JS | 1m1s | 2.95M | ✅ |
| JDK | 19,821 | Java | 3m31s | 8.05M | ✅ |
| Linux kernel | 60,468 | C | ~300s (timeout) | — | ⚠️ |
| InstrumentTimbre | 142 | Python | 0.94s | — | ✅ |

## Performance Insights

**Key Findings:**
1. Parse time accounts for only 1-3% of total indexing time
2. SQLite write is the main bottleneck (30-45%)
3. buildGraph phase takes 29-38% of time
4. FTS indexing adds 19-29% overhead
5. Token savings: ~98.9% vs raw source files, ~85% vs codebase-memory-mcp

**Throughput:**
- Peak: 11,523 files/s (rustc)
- Typical: 150-350 files/s (medium projects)
- Lower: 93 files/s (JDK - high node density)

---

For detailed analysis, refer to individual reports in this directory or their English translations in `../en/`.