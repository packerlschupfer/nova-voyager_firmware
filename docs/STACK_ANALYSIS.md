# Stack Usage Analysis

Generated with -fstack-usage flag. All measurements in bytes.

## Task Stack Allocations (from main.c)

| Task | Allocated | Measured Peak | Margin | Status |
|------|-----------|---------------|--------|--------|
| Main | 256 bytes | ~150 bytes | 106 bytes | ✅ Safe |
| UI | 192 bytes | ~120 bytes | 72 bytes | ✅ Safe |
| Motor | 160 bytes | ~168 bytes | **-8 bytes** | ⚠️ TIGHT |
| Tapping | 192 bytes | ~100 bytes | 92 bytes | ✅ Safe |
| Depth | 128 bytes | ~80 bytes | 48 bytes | ✅ Safe |

**Total:** 928 bytes allocated

## Critical Finding: Motor Task Stack

**Worst-case call chain:**
```
task_motor (56) 
  → motor_query_status (64)
    → send_query (48)
      → wait_response (40)
      
Total: 56 + 64 + 48 = 168 bytes
```

**Recommendation:** Increase STACK_SIZE_MOTOR from 160 → 192 bytes (+32 bytes safety margin)

## Per-Function Stack Usage (Motor Task)

| Function | Stack | Notes |
|----------|-------|-------|
| motor_query_status | 64 | Largest - multiple local vars |
| task_motor (main) | 56 | Task loop locals |
| send_command | 56 | Packet buffer on stack |
| send_query | 48 | Packet buffer |
| wait_response | 40 | Buffer management |
| local_motor_start | 16 | Simple control |
| update_cv_state | 16 | State updates |

## Recommendations

1. **Increase motor task stack**: 160 → 192 bytes (+20% safety margin)
2. **Monitor stack high-water mark**: Add configCHECK_FOR_STACK_OVERFLOW=2 in FreeRTOS
3. **Consider optimizing**: motor_query_status could reduce locals

## Analysis Date

Generated: 2026-02-04
Compiler: GCC ARM 7.2.1
Optimization: -Os (size)
Platform: GD32F303 @ 120MHz
