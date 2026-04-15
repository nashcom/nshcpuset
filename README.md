# nshcpuset - Windows Efficiency Core Troubleshooting

# Introduction

The follwing information and simple tool has been written to troubleshoot and fix a HCL Notes Client issue I came accross.
For multiple weeks I have been hunting a performance problem on my new Thinkpad T41 with a modern CPU.

It turned out that modern CPUs with a combination of E-Cores and P-Cores can cause weird performance problems.
Windows "tries" to schedule processes (and acutally threads) on the cores which should best fit the application need.
But this might not always workout when an application has mixed a mixed workload.

# HCL Notes Development Team is looking into it

The HCL Notes Client Team & Product Management are aware of this very specific problem and are actively looking into a solution.
This might not hit you depending on how many P-Cores you have and the work-load on your machine.
But it is good to be aware that this can happen -- not only with Notes but also other applications.

# This might hit other applications

The writeup also describes the background of the problem and how applications can work more smoothly with hybrid CPU architectures.
My tool is the currently recommended way to troubleshoot and work-around in case you have issues.

Other applications like VMware workstation also had to make adjustments for running with CPUs which have a mix of E-Cores and P-Cores.

In my special case the rendering performance in my Notes Client dropped dramatically during Sametime calls and other a bit more demanding operations.
You could really watch the client painting Windows in slow motion.

Windows 11 provides new APIs to allow applications let Windows know that a thread in an application is better suited for a P-Core.
This functionality would need to be build into the application.

The HCL Notes and other applications don't use those new API calls. To be frank I have not heard about those APIs before I started the research.
I did know about hypbrid CPUs with P-Cores and E-Cores and also did hear about issues on VMware workstation from friend.
But I never thought that a wrong scheduling on Windows side could slow down an application like this.
I did some research to find a temporary work-around for my own environment after looking into all other settings, driver updates including GPU driver and settings.

IMHO Microsoft should have raised this in more public to have developers aware -- Like Apple does when they introduce new functionality.
In this case it is a combination of Intel new CPUs and how Microsoft handles them.


# Band Aid

Setting the P-Core affinity for my Notes Basic Client really helped. And the same should work for the Standard Client.
Below are the details behind the issue and the available APIs.


# Hybrid CPUs on Windows: How to Prefer Performance Cores (P-Cores)

Modern CPUs (Intel hybrid, increasingly others) combine Performance cores (P-cores) and Efficiency cores (E-cores).
Windows uses a sophisticated scheduler (with help from Intel Thread Director on supported systems) to decide where threads run.

The key point: 

Applications cannot directly choose cores, but they can strongly influence scheduling behavior.


# What applications should do today

## 1. Use Thread Power Throttling (QoS hint) — the most important step

Applications should explicitly signal that their work is performance-sensitive:

```c
THREAD_POWER_THROTTLING_STATE state = {0};
state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
state.StateMask = 0; // disable throttling → prefer performance

SetThreadInformation(
    GetCurrentThread(),
    ThreadPowerThrottling,
    &state,
    sizeof(state)
);
```

### What this does

* Disables EcoQoS behavior
* Signals the scheduler to prefer P-cores
* Works in conjunction with Windows scheduling and hardware feedback

Relevant APIs to explore:

* `SetThreadInformation`
* `THREAD_POWER_THROTTLING_STATE`
* `ThreadPowerThrottling`


## 2. Use CPU Sets for explicit control (if needed)

If an application needs more control, it can use CPU Sets.

### Steps:

1. Query CPU topology:

   * `GetSystemCpuSetInformation`
2. Select CPUs:

   * Typically `EfficiencyClass == 0` → P-cores
3. Assign:

   * `SetThreadSelectedCpuSets` or `SetProcessDefaultCpuSets`


### Why CPU Sets?

* More flexible than affinity masks
* Forward-compatible with future CPU designs
* Can still allow controlled fallback

Relevant APIs:

* `GetSystemCpuSetInformation`
* `SetThreadSelectedCpuSets`
* `SetProcessDefaultCpuSets`


## 3. Set appropriate thread / process priority

```c
SetThreadPriority(...);
SetPriorityClass(...);
```

Important:

* Affects when a thread runs
* Does not control which core type is used

Still useful in combination with QoS hints.

Relevant APIs:

* `SetThreadPriority`
* `SetPriorityClass`


## 4. Avoid forcing affinity unless absolutely necessary

```c
SetThreadAffinityMask(...);
```

* Guarantees placement
* But:

  * brittle across hardware
  * bypasses scheduler intelligence
  * can reduce overall performance

Relevant API:

* `SetThreadAffinityMask`


# Recommended in-application strategy

For most applications:

1. Disable power throttling (QoS hint)
2. Use normal or slightly elevated priority
3. Let Windows + Intel Thread Director optimize execution

This provides the best balance of:

* performance
* efficiency
* portability


# What applications cannot reliably do

* Force “P-core only” execution without affinity or CPU sets
* Control scheduling decisions completely
* Override system-wide power or thermal policies


# Workarounds used in practice (external control)

In real-world environments (admins, power users, performance tuning), stronger control is often required.


## 1. Set process affinity to P-cores

Example:

```powershell
$p = Get-Process myapp
$p.ProcessorAffinity = 0x00F
```

Or when starting the process

```
cmd /c "start /affinity F myapp.exe"
```

The key challenge here is to find out which cores are the P-Cores and what affinity mask this results in.
It's acutally not that easy to find out which cores are the P-Cores.
There are extranal tools to help. But you can also use Windows API as shown in the small tool below.

### What this achieves:

* Hard restriction to selected CPUs (e.g., P-cores)
* Deterministic behavior

### Downsides:

* Requires mapping logical CPUs to P-cores
* Not portable across systems
* Disables scheduler flexibility


## 2. Raise process priority

```powershell
$p.PriorityClass = "AboveNormal"
```

Important:

* Improves scheduling responsiveness
* Does not prevent E-core usage


## 3. Combined approach (commonly used)

Affinity (P-cores) plus Above Normal priority

This effectively:

* Forces execution on P-cores
* Ensures good scheduling responsiveness

This is a practical and widely used workaround, especially when applications are not QoS-aware.


## 4. Attempting to modify QoS externally

In theory:

* Enumerate threads
* Call `SetThreadInformation` on each

In practice:

* Threads are short-lived
* Requires continuous monitoring
* Race conditions and access issues
* Not reliable or scalable

This approach is generally not recommended.


# Key takeaway

For application developers:

* Use Thread Power Throttling (QoS) first
* Use CPU Sets if deterministic behavior is required
* Avoid hard affinity unless necessary

For operators and power users:

* Use affinity plus priority for strong control
* Accept trade-offs in flexibility and portability


# Final thought

Windows scheduling on hybrid CPUs is:

* policy-driven, not strictly controlled
* optimized using runtime feedback and hardware signals

Trying to override the scheduler usually leads to fragile solutions and inconsistent results.

The best results come from working with the scheduler (QoS).
The most deterministic results come from overriding it (affinity or CPU sets).

---

# Example: External P-Core Mapping Tool

The following simple tool **nshcpuset** demonstrates a practical external approach to steer selected processes toward P-cores using modern Windows APIs.
The better approach would be that the application tells Windows which threads need the faster P-Cores -- For example for UI operations.

## Overview

This utility:

* Detects CPU topology using **CPU Sets**
* Identifies P-cores based on `EfficiencyClass`
* Scans running processes and optionally filters them
* Applies:

  * **P-core CPU Set restriction**
  * **Above Normal process priority**

This provides deterministic behavior without modifying the target application.


## How it works

### 1. CPU topology detection

The tool queries Windows for CPU set information:

* Uses `GetSystemCpuSetInformation`
* Builds an internal list of CPUs:

  * Logical processor index
  * CPU Set ID
  * Efficiency class

It then determines:

* The **maximum `EfficiencyClass`**
* Treats those CPUs as **P-cores**

This avoids hardcoding CPU layouts and works across different hybrid architectures.


### 2. Identifying P-cores

Each CPU is classified as:

* P-core → highest `EfficiencyClass`
* E-core → lower `EfficiencyClass`

This is the same mechanism Windows uses internally for scheduling decisions.


### 3. Process discovery and filtering

The tool supports two modes:

#### Scan mode (`-scan`)

* Enumerates all running processes (`CreateToolhelp32Snapshot`)
* Retrieves executable path (`QueryFullProcessImageName`)
* Extracts:

  * Company name (via version info)
  * Code signing subject (via certificate APIs)

This allows identifying processes from specific vendors (e.g HCL in our case).

#### Set mode (`-set <name>`)

* Matches processes by name or substring
* Applies P-core mapping to matching processes


### 4. Applying P-core preference

For each matching process:

#### a) Open process

```c
OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, ...)
```

#### b) Adjust priority

```c
SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS);
```

This improves scheduling responsiveness but does not control core selection by itself.

#### c) Apply CPU Sets

```c
SetProcessDefaultCpuSets(hProc, ids, count);
```

Where:

* `ids[]` contains only CPU Set IDs for P-cores

This effectively constrains the process to P-cores while still using the modern CPU Sets mechanism (instead of legacy affinity masks).


## Why CPU Sets instead of affinity

The tool intentionally uses CPU Sets instead of `SetProcessAffinityMask` because:

* CPU Sets are topology-aware
* They are forward-compatible with future CPU designs
* They integrate better with the Windows scheduler

This is the recommended modern alternative to affinity masks.


## Behavior and limitations

* The process is restricted to P-cores at the scheduler level
* New threads automatically inherit the process CPU Set restriction
* No need to track or modify individual threads

However:

* CPU classification depends on `EfficiencyClass`
* Behavior may vary across vendors and future CPU designs
* Scheduler decisions (e.g., load balancing) still apply within the selected CPU set


## Practical use case

This approach is useful when:

* Applications are not QoS-aware
* You need deterministic performance behavior
* You cannot modify the application itself

Typical usage:

```
nshcpuset.exe -set domino
nshcpuset.exe -set notes,java
```

Example Output:

```
nshcpuset.exe -set nlnotes.exe

CPU layout:
----------------------------------------
CPU  0 -> EfficiencyClass=1 (P-core)
CPU  1 -> EfficiencyClass=1 (P-core)
CPU  2 -> EfficiencyClass=1 (P-core)
CPU  3 -> EfficiencyClass=1 (P-core)
CPU  4 -> EfficiencyClass=0 (E-core)
CPU  5 -> EfficiencyClass=0 (E-core)
CPU  6 -> EfficiencyClass=0 (E-core)
CPU  7 -> EfficiencyClass=0 (E-core)
CPU  8 -> EfficiencyClass=0 (E-core)
CPU  9 -> EfficiencyClass=0 (E-core)
CPU 10 -> EfficiencyClass=0 (E-core)
CPU 11 -> EfficiencyClass=0 (E-core)

P-Cores : 4
E-Cores : 8

Applying to nlnotes.exe PID=24404
Adjust priority for PID=24404
Applied P-core preference to PID=24404 (4 cores)
```

## Relevant Windows APIs

For readers who want to explore further:

* `GetSystemCpuSetInformation`
* `SetProcessDefaultCpuSets`
* `SetPriorityClass`
* `CreateToolhelp32Snapshot`
* `Process32First / Process32Next`
* `QueryFullProcessImageName`
* `GetFileVersionInfo / VerQueryValue`
* `CryptQueryObject / CertFindCertificateInStore`


## Summary

This simple tool represents a practical “Plan B”:

* Instead of relying on applications to provide QoS hints
* It enforces P-core usage externally using CPU Sets

It complements the recommended in-application approach and provides a reliable fallback when applications are not optimized for hybrid CPU scheduling.
