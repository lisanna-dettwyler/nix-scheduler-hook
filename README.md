# Nix Scheduler Hook

This is a build hook that allows Nix builds to be forwarded to clusters running a job scheduler by submitting each build as its own job. It assumes that you can access the host over SSH by the hostname, e.g. `ssh <hostname>`. SSH is used for copying the dependencies and results to and from the job host, and for streaming the build log. Settings are managed with an `nsh.conf` file in your Nix configuration directory, e.g. `/etc/nix/nsh.conf` or `~/.config/nix/nsh.conf`, using the same `key = value` format that `nix.conf` uses.

General settings:

- `job-scheduler`: Which job scheduler to use, currently only 'slurm' is available. Default: `slurm`.
- `system`: The system type of this cluster, jobs requiring a different system will not be routed to the scheduler. Default: `x86_64-linux`.

## Supported Job Schedulers

Currently, only Slurm is supported through its [REST API](https://slurm.schedmd.com/rest.html). This requires `slurmrestd` to be running on the cluster and for you to have a valid JWT token for your account. At a minimum, you should set `slurm-jwt-token` in `nsh.conf` to your JWT token.

The current settings available for Slurm are:

- `slurm-api-host`: Hostname or address of the Slurm REST API endpoint. Default: `localhost`.
- `slurm-api-port`: Port to use for the Slurm REST API endpoint. Default: `6820`.
- `slurm-jwt-token`: JWT token for authentication to the Slurm REST API.
- `slurm-extra-submission-params`: Extra parameters to set in the `/job/submit` API request, as a JSON dictionary that will be merged with the 'job' value in the [`job_submit_req`](https://slurm.schedmd.com/rest_api.html#v0.0.44_job_submit_req) object. Takes precedence over parameters specified at the derivation level.

Extra job parameters to control things like required CPU count and memory can also be specified on a per-derivation basis. For Slurm, this can be set in the `extraSlurmParams` attribute of a derivation, and it functions exactly like the `slurm-extra-submission-params` setting. For example:

```nix
runCommand "myjob" {
  extraSlurmParams = builtins.toJSON {
    cpus_per_task = 4;
    memory_per_node = {
      set = true;
      number = 1024;
    };
  };
}
```

## Installation

After building from source, edit your `nix.conf` and set `build-hook = /path/to/nsh`. NSH is currently not available in nixpkgs or as a NixOS module, but this should be remedied soon.

## Known Limitations

Because of https://github.com/NixOS/nix/issues/14760, it is impossible for NSH to cleanup any outstanding jobs if the build gets manually cancelled, e.g. with ctrl-c. It is therefore currently the responsibility of the user to cleanup all the jobs NSH submits to the scheduler on their behalf if they cancel a build.
