{
  description = "A build hook for Nix that dispatches builds through a job scheduler.";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs";
    nix.url = "github:nixos/nix";
    # slurm = {
    #   url = "github:SchedMD/slurm";
    #   flake = false;
    # };
    restc-cpp = {
      url = "github:jgaa/restc-cpp";
      flake = false;
    };
  };

  outputs = { self, ... }@inputs: let
    system = "x86_64-linux";
    pkgs = import inputs.nixpkgs { inherit system; };
  in rec {
    packages.x86_64-linux.default = packages.x86_64-linux.nix-scheduler-hook;
    packages.x86_64-linux.nix-scheduler-hook = import ./default.nix {
      pkgs = pkgs;
      nix = inputs.nix.nix-everything;
      restc-cpp = inputs.restc-cpp;
    };
  };
}
