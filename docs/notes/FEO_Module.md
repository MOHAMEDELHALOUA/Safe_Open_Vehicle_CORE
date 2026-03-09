# Summary
<!-- vscode-markdown-toc -->
* 1. [Why Does it exists](#WhyDoesitexists)
* 2. [Core Concepts](#CoreConcepts)
	* 2.1. [Applications](#Applications)
	* 2.2. [Activities](#Activities)
	* 2.3. [How does FEO differ from the Communication module (what you tested with scrample)?](#HowdoesFEOdifferfromtheCommunicationmodulewhatyoutestedwithscrample)
* 3. [Can we test FEO like you did with scrample?](#CanwetestFEOlikeyoudidwithscrample)

<!-- vscode-markdown-toc-config
	numbering=true
	autoSave=true
	/vscode-markdown-toc-config -->
<!-- /vscode-markdown-toc -->

# What is the FEO Module in S-CORE

***source for more informations***:
```
eclipse-score.github.io/score/main/features/frameworks/feo/architecture/feature_architecture.html
```
**FEO** stands for **Fixed Execution Order** (sometimes also written as "Fixed Order Execution"). It is a framework designed for applications in the ADAS domain that supports both data-driven and time-driven applications, ensuring a fixed, deterministic execution order of tasks and providing infrastructure to reprocess activities in a simulated environment.

##  1. <a name='WhyDoesitexists'></a>Why Does it exists
There are several automotive use cases requiring a fixed and deterministic computation of tasks, and this is particularly crucial for safety-critical applications where the execution order is essential for correct system operation. Eclipse-score Think of it like a camera pipeline in a self-driving car — the image capture activity must always complete before the object detection activity, which must complete before the path planning activity. FEO enforces this guarantee.

Key aspects of S-CORE and FEO framework are:

* a framework for applications (not for platform services).
* for data-driven and time-driven applications (mainly in the ADAS domain)
* support fixed execution order
* supporting reprocessing

##  2. <a name='CoreConcepts'></a>Core Concepts
**Activities** are the building blocks. Each activity has `init()`, `step()`, and `shutdown()` entry points. There are two types: Application Activities (pure computation, single-threaded, using only framework APIs) and Service Activities (interact with the outside world — sensors, actuators, network).

###  2.1. <a name='Applications'></a>Applications
The framework is used to build applications

* Multiple applications based on the framework can run in parallel on the same host machine
* Applications based on the framework can run in parallel with other applications not based on the framework
* The framework does not support communication between different applications (except via service activities, see below)

###  2.2. <a name='Activities'></a>Activities
Applications consist of activities

* Activities are a means to structure applications into building blocks
* Activities have init(), step() and shutdown() entry points
* The framework provides the following APIs to the activities running on it:
    * Read time (feo::time)
    * Communicate to other activities (feo::com)
    * Log (feo::log)
    * Configuration parameters (feo::param)
    * Persistency (feo::pers)
    * Tracing (feo::tracing)
* There are two types of activities:
    * Application activities
    * Service activities

**Static thread mapping** is a key design principle. Activities are mapped to threads in a static way — calling an activity's `step()` from different threads in different iterations can cause execution time jitter from cache misses or different processor core properties, and dynamic assignment can result in non-deterministic task-chain execution time. Eclipse-score.\

**Communication inside FEO** is also static. There is no publish/subscribe mechanism at runtime — the set of communication topics and which activity sends/receives on each topic is "runtime static", meaning it's fixed after startup and doesn't change during the run phase.

**The APIs** FEO provides to activities are: `feo::time`, `feo::com`, `feo::log`, `feo::param`, `feo::pers`, and `feo::tracing`. Activities support both C++ and Rust, and mixed systems are supported.

**Safety target**: FEO is rated with a security: NO and safety: ASIL_B classification.

###  2.3. <a name='HowdoesFEOdifferfromtheCommunicationmodulewhatyoutestedwithscrample'></a>How does FEO differ from the Communication module (what you tested with scrample)?
Scrample demonstrated raw IPC — publishing and consuming messages between processes. FEO sits one level higher: it uses the communication layer underneath but adds the deterministic ordering framework on top.

| Aspect    | Communication (scrample)                | FEO                                  |
|-----------|-----------------------------------------|--------------------------------------|
| Focus     | IPC between processes/servicesExecution | scheduling within an app             |
| Pattern   | Skeleton-Proxy (AUTOSAR)                | Task chain with fixed activity order |
| Language  | C++                                     | Rust primarily                       |
| Demo      | scrample producer/consumer              | `mini-adas` example                  |


##  3. <a name='CanwetestFEOlikeyoudidwithscrample'></a>Can we test FEO like you did with scrample?

Yes, but the experience is a bit different. FEO has its own dedicated repo (`eclipse-score/feo`) with its own built-in examples. The key one is:
`mini-adas` — a Rust-based example that simulates a minimal ADAS pipeline using FEO activities.\
We can clone the repo, build with Bazel or Cargo, and run the example with:

```bash
git clone https://github.com/eclipse-score/feo.git
cd feo
bazel run //examples/rust/mini-adas:adas_primary
```

# How FEO Works — A Practical Deep Dive
1. **Core Idea**

Everything in FEO revolves around a **task chain — a pre-defined, ordered sequence of activities**.\
Think of it like an assembly line where station 1 must always finish before station 2 starts, station 2 before station 3, etc.
The execution order within a task chain is fixed and deterministic. All input service activities must finish before the first application activity runs, and all output service activities run only after all application activities have finished. Eclipse Foundation

A task chain always follows this shape:
```
[Input Service Activity] → [App Activity A] → [App Activity B] → [App Activity C] → [Output Service Activity]
      (read sensors)           (preprocess)       (detect objects)    (plan path)         (send to actuator)
```

2. The Three Roles: Executor, Agent, Activity

These are the three internal players FEO wires together:

**Executor** — the brain. Lives in the primary process. The executor orchestrates the entire task chain — it knows the order, sends invocation commands to agents, and can record the sequence of activity invocations for replay purposes. GitHub

**Agent** — the hands. An agent exists in each process belonging to an application. It connects to the executor during startup, then takes invocation commands sent by the executor and executes them in its local process on behalf of the executor. Eclipse Foundation If you have 3 processes, you have 3 agents.

**Activity** — your code. The thing you actually write. Has three entry points:

* `init()` — called once at startup, allocate your resources here
* `step()` — called every cycle, your actual logic goes here
* `shutdown()` — called once on teardown

3. **Process & Thread Architecture**

An application consists of one primary process and optionally multiple secondary processes. Activities are statically mapped to threads within processes — this static mapping is what enables deterministic, jitter-free execution, since calling an activity from a pre-defined thread avoids cache misses and unpredictable processor core behavior.

 ```
 Primary Process                     Secondary Process
┌──────────────────────┐            ┌──────────────────────┐
│  Executor            │◄──────────►│  Agent               │
│  Agent               │            │  Thread 1            │
│  Thread 1            │            │   └─ ActivityC       │
│   └─ ActivityA       │            │  Thread 2            │
│  Thread 2            │            │   └─ ActivityD       │
│   └─ ActivityB       │            └──────────────────────┘
└──────────────────────┘
```
This multi-process design is not just for performance — one key reason for having multiple processes is to achieve Freedom From Interference for safety-relevant applications GitHub, which is a hard requirement under ISO 26262.

4. **Communication Between Activities**

Activities should not share state directly — state management must be explicit and isolated. Communication between activities uses a topic-based model where the connections are "runtime static": fixed after startup and not changeable during the run phase. \
There can only be one sender per topic but multiple receivers. The receiver cannot modify a message — the framework enforces read-only access, for example via memory protection at the OS level. Eclipse Foundation

* "Unchangeable connections" = "runtime static" means: topic sender/receiver relationships are frozen after startup. No dynamic subscribe/publish during normal operation.  * What "runtime static" means exactly From FEO docs: "static after the startup phase" — during `init()`, FEO can configure connections, but once `step()` calls begin (run phase), the wiring is locked.  ​

```
Startup Phase:        Run Phase:
┌─────────────────┐   ┌─────────────────┐
│ Config loads:   │   │ Activities run  │
│ - /camera  ← A1 │──>│ A1 → /camera    │
│ - /objects← A2  │   │ A2 → /objects   │
│ - /plan   ← A3  │   │ A3 -> /plan     │
└─────────────────┘   └─────────────────┘
        ↓                      │
    Connections          NO NEW subscriptions!
      FIXED                NO topic changes!
```
* How it works in practice

Configuration file (before startup):

```
topics:
  /camera_img:    { sender: SensorInputActivity,  queue_len: 3 }
  /radar_points:  { sender: RadarInputActivity,   queue_len: 3 }
  /perception:    { sender: PerceptionActivity,   queue_len: 1 }

subscriptions:
  PerceptionActivity:  [/camera_img, /radar_points]
  PlanningActivity:    [/perception]
```
During startup (init() phase): FEO wires these connections (shared memory, ring buffers, etc.)

During run (step() phase):

* `PerceptionActivity.step()` always reads from `/camera_img` queue
* Cannot subscribe to new topics
* Cannot unsubscribe
* Queue config (length, overwrite policy) also fixed

5. The Record & Replay Superpower

This is what makes FEO really stand out for ADAS development. The executor can record the full system behavior as a sequence of activity invocations, including all messages going over communication topics. In a replay scenario, the framework reproduces those messages exactly as they were recorded — enabling you to re-run a real-world drive scenario through your activities without a physical vehicle. GitHub

This is essentially deterministic simulation built into the framework itself.

---
7. How You Use It In Your Own Project

In practice, integrating FEO into another project means three things:\

Step 1 — Add it as a Bazel dependency in your `MODULE.bazel` or `WORKSPACE`:

```python
# in MODULE.bazel
bazel_dep(name = "feo", version = "0.5.0")
```

* Step 2 — Write your activities implementing the FEO activity interface:

```rust
// Rust example
impl Activity for MyActivity {
    fn init(&mut self, ctx: &mut InitContext) { /* allocate resources */ }
    fn step(&mut self, ctx: &mut StepContext) { /* your logic */ }
    fn shutdown(&mut self, ctx: &mut ShutdownContext) { /* cleanup */ }
}
```

* Step 3 — Configure the task chain in a config file (JSON/TOML) that tells FEO:

    * which activities exist
    * what order they run in
    * which topics they read/write
    * which process each activity lives in

The design intent is that application development should not rely on a code generator GitHub — you configure, not generate, making it much cleaner to integrate into an existing project.
