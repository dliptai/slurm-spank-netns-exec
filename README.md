# SPANK plugin for running jobs in a network namespace.
Automatically moves job tasks into a pre-created named network namespace when the job is submitted to a designated partition.
Uses `setns()` in `task_init_privileged` (runs as root, pre-execve) so the job process inherits the namespace across the privilege drop and subsequent `execve()`.

## Configuration
Arguments are set in `plugstack.conf`, e.g.:
```
optional <SLURM_LIB_DIR>/netns_spank.so partition=isolated-jobs netns=/var/run/netns/isolated
```
Where:
```
partition= : jobs must be submitted to this slurm partition (required)
netns=     : full path to the pre-created network namespace (required)
```

## Pre-requisites
The namespace must be created on each applicable compute node, e.g.:
```
ip netns add isolated
ip netns exec isolated ip link set lo up
```

This creates the namespace at `/var/run/netns/isolated`.
The namespace persists until the node reboots and is shared across jobs - it carries no per-job state.
Multiple concurrent jobs safely share it.

## Deployment
Build:
```
gcc -std=c99 -shared -fPIC -Wall -Wextra -I<SLURM_INCLUDE_DIR> -o netns_spank.so netns_spank.c
```

Install
```
cp netns_spank.so <SLURM_LIB_DIR>
chmod 755 <SLURM_LIB_DIR>/netns_spank.so
```

Register in /etc/slurm/plugstack.conf:
```
optional <SLURM_LIB_DIR>/netns_spank.so partition=<p> netns=<ns>
```
