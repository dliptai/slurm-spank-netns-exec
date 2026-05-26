# SPANK plugin for running jobs in a network namespace and a mount namespace.
Automatically moves job tasks into a pre-created named network namespace and a mount namespace when the job is submitted to a designated partition.
Uses `setns()` in `task_init_privileged` (runs as root, pre-execve) so the job process inherits the namespaces across the privilege drop and subsequent `execve()`.

## Configuration
Arguments are set in `plugstack.conf`, e.g.:
```
optional <SLURM_LIB_DIR>/netns_spank.so partition=isolated-jobs netns=/var/run/netns/isolated mntns=/var/run/netns/isolated-mnt
```
Where:
```
partition= : jobs must be submitted to this slurm partition (required)
netns=     : full path to the pre-created network namespace (required)
mntns=     : full path to the pre-created mount namespace (required)
```

## Pre-requisites
The namespaces must be created on each applicable compute node, e.g.:
```
ip netns add isolated
ip netns exec isolated ip link set lo up
```

This creates the network namespace at `/var/run/netns/isolated`.
The namespace persists until the node reboots and is shared across jobs - it carries no per-job state.
Multiple concurrent jobs safely share it.

For mount namespaces, you must create and persist the namespace manually, e.g., by bind-mounting from a running process's mount namespace:
```
mount --bind /proc/<pid>/ns/mnt /var/run/mntns/isolated
```

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
optional <SLURM_LIB_DIR>/netns_spank.so partition=<p> netns=<ns> mntns=<mnt>
```

## Testing
To compile the `test` binary run
```
make test
```
The binary takes a slurm spank config string as an argument, e.g. `./test partition=test netns=/var/run/netns/testns mntns=/var/run/netns/testns-mnt`.
It then calls `slurm_spank_init()` with the provided config, followed by `slurm_spank_task_init_privileged()`.

For testing we implement mock `spank_context()` and `spank_getenv()` functions.
The mock `spank_context()` uses the environment variable `SPANK_CONTEXT` to return `S_CTX_REMOTE` if `SPANK_CONTEXT=1`, else returns `S_CTX_LOCAL`.
The mock `spank_getenv()` simply returns local environment variables.

### Test suite:
A simple test suite runs in GitHub actions. The `test` binary is run with different combinations of configurations/options, comparing the return code each time with an expected return code. It also creates real and fake network namespaces, and sets environment variables between tests.

The following tests are implemented:
- end-to-end
- Missing config
- Unknown config
- Not remote context
- Missing SLURM_JOB_PARTITION
- Wrong SLURM_JOB_PARTITION
- Missing netns
- Unopenable netns
- Netns path not owned by root
- setns() fail
