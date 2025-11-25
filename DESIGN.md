I'm going to write a build hook in C++ that forwards builds to job schedulers, like Slurm. Rather than being submitted as a batch job, it will need to be submitted as an interactive reservation, because before the actual build can take place, dependencies need to be copied over to the machine. In lieu of submitting a batch job which runs a custom program that "waits" for this to happen before starting the build, it'd be easier to submit an interactive job. Unfortunately, not all clusters permit interactive jobs.

For a batch job, all it would really need to do is wait for a file to exist, to signal that it's safe to start the build. Its arguments could be: the name of the file to wait for, and the name of the derivation it should realize once that file exists. TODO: look at how build-remote.cc prepares the remote for building the derivation.

build-remote, upon accepting a build, copies the dependencies to the remote, and then invokes sshStore->buildDerivation(*drvPath, drv). What we need to do is copy the dependencies to the remote, copy the derivation file of the build target to the remote, and then invoke nix-store --realise <drvPath>.

- [x] Setup slurm's REST API daemon.
- [x] Configure project with a rest library
- [x] Include nix in build tree, builds successfully as meson subproject
- [x] Use ccache
- [x] Parse a config file
- [x] Use the rest library to perform a basic test
- [x] Try to submit a job to slurm that does anything.
- [x] Use the rest API to submit a job to slurm that realizes a drv
- [x] Open a SSH connection and store to the remote
- [x] Create a derivation with dependencies that rebuilds every time
- [x] Tail the build log
- [x] Resolve the dependencies and copy them to the remote
- [x] Copy the drv to the remote to trigger the build starting
- [x] Act as a build hook, try to submit to slurm, and if successful return accept.
- [ ] Query exit code once we get activity on stdout rather than timed with exponential backoff
- [x] Fix logging
- [ ] Cleanup slurm code
- [ ] Don't hardcode /nix/store in slurm job
- [ ] On decline, instead of returning decline, try forwarding to the regular build hook.