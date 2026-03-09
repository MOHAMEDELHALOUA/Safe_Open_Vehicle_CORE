# Building blocks of the scrample and s-core communication module

## mw_com_config.json


# How does S-CORE actually move data between processes?
The binding you used is SHM — Shared Memory. Here's how it works:
```
Process A (skeleton)          Shared Memory Region          Process B (proxy)
─────────────────────         ────────────────────          ─────────────────
Allocate() ──────────────→   [ slot 0: MapApiLanesStamped ]
                              [ slot 1: empty              ]
                              [ slot 2: empty              ]
Send() marks slot ready ──→  [ slot 0: READY              ]
                                                      ←── GetNewSamples() reads slot 0
                                                           SamplePtr holds slot 0
                                                           until it goes out of scope
                                                      ──→  slot 0 released back to pool
```
