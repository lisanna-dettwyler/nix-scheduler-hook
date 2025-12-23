{
  description = "A build hook for Nix that dispatches builds through a job scheduler.";

  inputs = {
    # nixpkgs.url = "github:nixos/nixpkgs";
    nixpkgs.url = "github:lisanna-dettwyler/nixpkgs/add-slurmrestd";
    flake-utils.url = "github:numtide/flake-utils";
    restclient-cpp = {
      url = "github:mrtazz/restclient-cpp";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, ... }@inputs: flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = import nixpkgs { inherit system; };
    in rec {
      checks = import ./tests.nix {
        inherit nixpkgs pkgs;
        nix-scheduler-hook = packages.default;
      };
      packages = rec {
        default = nix-scheduler-hook;
        nix-scheduler-hook = import ./default.nix {
          inherit pkgs;
          restclient-cpp = inputs.restclient-cpp;
        };
      };
    }
  );
}
