// vu_recompiler.cpp — Recompilador VU → ARM NEON
// Los VUs usan SIMD float32×4 — mapeo directo a instrucciones NEON de ARM.
// TODO: implementar emisor NEON para instrucciones VU comunes.
// Por ahora el VU Core usa un intérprete funcional.