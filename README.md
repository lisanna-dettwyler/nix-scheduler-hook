# Nix Scheduler Hook

This is a build hook that allows Nix builds to be forwarded to clusters running a job scheduler by submitting each build as its own job. It assumes that you can access the host over SSH by the hostname, e.g. `ssh <hostname>`. SSH is used for copying the dependencies and results to and from the job host, and for streaming the build log. Settings are managed with an `nsh.conf` file in your Nix configuration directory, e.g. `/etc/nix/nsh.conf` or `~/.config/nix/nsh.conf`, using the same `key = value` format that `nix.conf` uses.

General settings:

- `job-scheduler`: Which job scheduler to use, available choices are 'slurm', 'slurm-native', and 'pbs'. Default: `slurm`.
- `system`: The system type of this cluster, jobs requiring a different system will not be routed to the scheduler. Default: `x86_64-linux`.
- `system-features`: Optional system features supported by the machines in the cluster. Can be used to force derivations to build only via nix-scheduler-hook by adding 'nsh' as a required system feature. Default: `nsh`.
- `mandatory-system-features`: System features that the derivations must require in order to be built on the cluster. Default: (empty).
- `store-dir`: The logical remote Nix store directory. Only change this if you know what you're doing. Default: `/nix/store`.
- `remote-store`: The store URL to be used on the remote machine. See: [https://nix.dev/manual/nix/latest/store/types/](https://nix.dev/manual/nix/latest/store/types/). Default: `auto`.
- `remote-nix-bin-dir`: Path to the Nix bin directory to use on the remote system. This should be a shared location on your cluster. Useful for when your cluster does not have Nix installed (see below).
- `collect-garbage`: Run `nix-store --gc` on the `remote-store` after each job completes. Default: `false`.

## Supported Job Schedulers

### Slurm

Slurm is supported through its [REST API](https://slurm.schedmd.com/rest.html). This requires `slurmrestd` to be running on the cluster and for you to have a valid JWT token for your account. At a minimum, you should set `slurm-jwt-token` in `nsh.conf` to your JWT token.

The current settings available for Slurm are:

- `slurm-state-dir` (required): Where to store temporary files on the cluster that are used during execution. It is recommended to use a location in your home directory for security reasons.
- `slurm-api-host`: Hostname or address of the Slurm REST API endpoint. Default: `localhost`.
- `slurm-api-port`: Port to use for the Slurm REST API endpoint. Default: `6820`.
- `slurm-jwt-token` (required if using Slurm): JWT token for authentication to the Slurm REST API.
- `slurm-extra-submission-params`: Extra parameters to set in the `/job/submit` API request, as a JSON dictionary that will be merged with the 'job' value in the [`job_submit_req`](https://slurm.schedmd.com/rest_api.html#v0.0.44_job_submit_req) object. Takes precedence over parameters specified at the derivation level.

Extra job parameters to control things like required CPU count and memory (in megabytes) can also be specified on a per-derivation basis. For Slurm, this can be set in the `extraSlurmParams` attribute of a derivation, and it functions exactly like the `slurm-extra-submission-params` setting. For example:

```nix
runCommand "myjob" {
  extraSlurmParams = builtins.toJSON {
    cpus_per_task = 4;
    memory_per_node = {
      set = true;
      number = 1024;
    };
  };
} ''
echo "Hello Slurm!" > $out
''
```

If `slurmrestd` is not available, you can use the 'slurm-native' scheduler instead, which uses libslurm. The settings available are:

- `slurm-state-dir` (required): Where to store temporary files on the cluster that are used during execution. It is recommended to use a location in your home directory for security reasons.
- `slurm-conf`: Path to slurm.conf. If unset, Slurm will attempt to locate it automatically.

Using Slurm through the REST API allows the most flexibility with specifying job parameters. When using the native version, the following job constraints can be specified on a per-derivation basis through the `slurmNativeConstraints` attribute:

- `cpus`: The number of CPUs required by the job.
- `memPerNode`: The minimum real memory in megabytes required for the node the job runs on.
- `memPerCPU`: The minimum real memory in megabytes required for each CPU. Mutually exclusive with `memPerNode`.

Example:

```nix
runCommand "myjob" {
  slurmNativeConstraints = builtins.toJSON {
    cpus = 4;
    memPerNode = 8192;
  };
} ''
echo "Hello Slurm Native!" > $out
''
```

### PBS

PBS is supported through libpbs, so you may have to recompile NSH against an older version depending on what your cluster is running.

The current settings available for PBS are:

- `pbs-host`: Hostname or address of the host running the PBS server. Default: `PBS_SERVER` value from `pbs.conf`.
- `pbs-port`: Port that the PBS server is listening on. Default: `15001`.

If `pbs-host` is left unspecified, values for both the host and port are taken from `pbs.conf`.

Job resources can be specified on a per-derivation basis via the `pbsResources` derivation attribute. All values should be strings. For example:

```nix
runCommand "myjob" {
  pbsResources = builtins.toJSON {
    ncpus = "4";
    mem = "1gb";
  };
} ''
echo "Hello PBS!" > $out
''
```

## Installation

After building from source, edit your `nix.conf` and set `build-hook = /path/to/nsh`. NSH is currently not available in nixpkgs or as a NixOS module, but this should be remedied soon.

## Fallback to Normal Build Hook

If NSH would decline a build, instead of simply declining, it attempts to launch the normal build hook and forwards it the build details. The normal build hook will then either accept or decline the build.

## Usage on Clusters Without Nix Installed

It is possible to use this hook to submit jobs to clusters without Nix installed, it just requires a small amount of one-time setup.

Start on a machine that *does* have Nix installed and that can connect to a cluster login node to access your home directory. Download the package `nixStatic` on this machine, being sure to specify a system matching the cluster. This is especially important if, for example, you are on a Mac but your cluster is running Linux.

```bash
nix build --system x86_64-linux nixpkgs#nixStatic
```

Next, copy Nix Static from your machine to your home directory on the cluster (or a shared location you have access to).

```bash
scp -r ./result login.example.com:/home/you/nix-static
```

Configure your `nsh.conf` file with the following settings:

```conf
remote-store = /local/store
remote-nix-bin-dir = /home/you/nix-static/bin
```

This will cause NSH to invoke the Nix Static binaries on the remote machine when performing a build and copying dependencies and results. See below notes on best practices for setting `remote-store`.

## Managing Which Derivations Get Built on the Cluster

The `system-features` and `mandatory-system-features` configuration settings can be used to filter which derivations are sent to the cluster and which are built through other means (fallback or locally). `nsh` is the default value of the `system-features` setting. If you want to force a derivation to build on your cluster, you can add `nsh` as a `requiredSystemFeatures`.

```nix
runCommand "myjob" {
  requiredSystemFeatures = [ "nsh" ];
} ''
echo "Hello Slurm!" > $out
''
```

By default, all derivations are opportunistically sent to NSH to be built on the cluster. If you want to prevent all but certain derivations from building on your cluster, you can additionally make use of the `mandatory-system-features` NSH setting. By default it is empty. If you set it to `nsh`, this will make all derivations which don't have `nsh` as a `requiredSystemFeatures` (e.g., everything in nixpkgs) build either through the fallback (regular) remote building hook or locally, and not on the cluster. This allows you to be selective about what gets sent to the cluster and what uses your own local resources for building.

## Known Limitations

Because of https://github.com/NixOS/nix/issues/14760, it is impossible for NSH to clean up any outstanding jobs if the build gets manually cancelled, e.g. with ctrl-c. It is therefore currently the responsibility of the user to clean up all the jobs NSH submits to the scheduler on their behalf if they cancel a build.

Some recent versions of Nix do not respect the `build-hook` option in `nix.conf`, requiring you to pass NSH via `--option` instead. This issue has been fixed in upstream as of [0e3a620](https://github.com/NixOS/nix/commit/0e3a6203747b6c3c24dec34cb3df5b829bf47100).

It is not possible to set `nix.settings.build-hook` on NixOS when using Lix. The `nix.conf` validation step will fail complaining that `build-hook` is a deprecated setting. It is still possible to use NSH with Lix through `--option build-hook` on the command-line, although fallback to the regular build hook is broken.

It is [recommended](https://discourse.nixos.org/t/how-do-i-best-use-nix-to-create-a-development-environment-on-an-hpc-cluster-without-the-possibility-of-system-wide-installation/71096/2) to use a `remote-store` location that is *not* on a shared filesystem, for performance reasons and to not exhaust your file count quota. Note that using a location in `/tmp` will not work because Nix disallows stores to exist in world-writable locations. Using a location in `/run/user/<uid>/` is not recommended as it is possible (although unlikely) for the job to start immediately and complete before a store connection is established to the remote, leaving a small window of time during which the contents of `/run/user/<uid>/` could be cleaned up. This could happen if a prior build of the same derivation was interrupted, leaving the derivation file available in the store for the new job to use immediately. It is safest to use a location backed by a local disk, and to make use of the `collect-garbage = true` NSH option to clean up after every job.
