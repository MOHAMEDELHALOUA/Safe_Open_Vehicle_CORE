# The Data Types We Need
These are the structs that will go in `datatype.h`:

```cpp
// ── Sensor Layer ─────────────────────────────────────────
struct SpeedData
{
    float    speed_kmh;        // current vehicle speed
    uint32_t timestamp_ms;     // when this was measured
    char     sensor_id[16];    // "speed_sensor_01"
};

struct DistanceData
{
    float    distance_m;       // distance to nearest obstacle
    float    closing_speed;    // how fast obstacle is approaching
    uint32_t timestamp_ms;
    char     sensor_id[16];    // "distance_sensor_01"
};

// ── Fusion Layer ─────────────────────────────────────────
struct VehicleState
{
    float    speed_kmh;
    float    distance_m;
    float    closing_speed_kmh;
    bool     obstacle_detected;
    uint32_t timestamp_ms;
    char     status[16];       // "NORMAL", "WARNING", "CRITICAL"
};

// ── Application Layer ────────────────────────────────────
struct CruiseCommand
{
    float    throttle_pct;     // 0.0 to 100.0
    float    target_speed_kmh;
    uint32_t cycle_id;
    char     source[16];       // "cruise_control"
};

struct BrakeCommand
{
    float    brake_force_pct;  // 0.0 to 100.0
    bool     emergency;        // true = emergency stop
    uint32_t cycle_id;
    char     source[16];       // "emergency_brake"
};

// ── Hardware Layer ───────────────────────────────────────
struct ThrottleCommand
{
    float    throttle_pct;
    bool     override_active;  // true = brake override applied
    uint32_t cycle_id;
};

struct BrakeActuatorCommand
{
    float    brake_force_pct;
    bool     emergency;
    uint32_t cycle_id;
};

// ── ACK (reuse your existing AckMessage) ─────────────────
// AckMessage already defined — no change needed
```
---

## The Services We Need in `mw_com_config.json`

Each arrow in the diagram = one service:
```
Service Name              Direction                    serviceId
────────────────────────────────────────────────────────────────
score/SpeedSensor         sensor → fusion              6434
score/DistanceSensor      sensor → fusion              6435
score/VehicleState        fusion → cruise + brake      6436
score/CruiseCmd           cruise → arbitrator          6437
score/BrakeCmd            brake  → arbitrator          6438
score/ThrottleCmd         arbitrator → throttle_act    6439
score/BrakeActuatorCmd    arbitrator → brake_act       6440
score/ThrottleAck         throttle_act → monitor       6441
score/BrakeAck            brake_act → monitor          6442
```

Note: services 6432 and 6433 are already used by the scrample. We start from 6434 to avoid conflicts.

---

## The FEO Scheduling Plan

FEO ensures processes run in the right order with guaranteed timing. Here is the schedule:
```
Timeline (one full cycle = 100ms):

t=0ms   ─── speed_sensor runs        (20ms period)
t=0ms   ─── distance_sensor runs     (20ms period)
t=1ms   ─── sensor_fusion runs       (20ms period, AFTER both sensors)
t=2ms   ─── emergency_brake runs     (10ms period, AFTER fusion)
t=2ms   ─── cruise_control runs      (50ms period, AFTER fusion)
t=3ms   ─── command_arbitrator runs  (10ms period, AFTER both app processes)
t=4ms   ─── throttle_actuator runs   (10ms period, AFTER arbitrator)
t=4ms   ─── brake_actuator runs      (10ms period, AFTER arbitrator)
t=5ms   ─── system_monitor runs      (100ms period, AFTER actuators)
```

The key FEO concept here is **precedence constraints** — you tell FEO "don't run fusion until both sensors have completed." This is what makes it deterministic.

---

## The Simulated Scenarios

The sensor process will cycle through three realistic scenarios automatically:
```
Scenario 1 — Normal cruise (0s to 10s):
  speed:    ramps from 0 to 80 km/h smoothly
  distance: stays at 100m (no obstacle)
  expected: cruise control maintains 80 km/h, no braking

Scenario 2 — Obstacle approaching (10s to 20s):
  speed:    holds at 80 km/h
  distance: decreases from 100m to 25m (obstacle ahead)
  expected: cruise control reduces throttle, brake system activates at 30m

Scenario 3 — Emergency (20s to 25s):
  speed:    still 80 km/h
  distance: drops from 25m to 5m rapidly
  expected: emergency brake fires, throttle cut to 0, full brake applied
```

---
##  12. <a name='ImplementationOrderStepbyStep'></a>Implementation Order — Step by Step

We build bottom-up, testing each layer before adding the next:
```
Before implementation begins, we will spend time exploring
each S-CORE module through its examples and documentation:

Week 0 (before coding):
  - FEO:         run examples, write a minimal 2-activity test
  - LOG:         integrate into the existing scrample
  - PERSISTENCE: write a counter test across process restarts
  - EXEC:        research QNX portability requirements

This ensures each module is understood before it is used
in the ADAS pipeline, reducing debugging time during
implementation.

Week 1 — Foundation
  Step 1: datatype.h (all structs + all interfaces)
  Step 2: mw_com_config.json (all 9 services)
  Step 3: speed_sensor + distance_sensor (simplest — just send data)
  Step 4: sensor_fusion (first process that is both proxy and skeleton)
  → Test: verify fusion receives from both sensors correctly

Week 2 — Application Layer
  Step 5: pid_controller algorithm (pure C++, no S-CORE)
  Step 6: cruise_control process
  Step 7: emergency_brake process
  → Test: verify both receive VehicleState and produce correct commands

Week 3 — Hardware + Monitoring
  Step 8: command_arbitrator
  Step 9: throttle_actuator + brake_actuator
  Step 10: system_monitor + LOG integration
  → Test: full end-to-end pipeline with all 3 scenarios

Week 4 — FEO + Persistence
  Step 11: integrate FEO scheduling
  Step 12: add persistence (CSV log writing)
  → Test: verify deterministic timing, review logs
