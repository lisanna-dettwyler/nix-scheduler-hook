{
  description = "A build hook for Nix that dispatches builds through a job scheduler.";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    restclient-cpp = {
      url = "github:mrtazz/restclient-cpp";
      flake = false;
    };
  };

  outputs = { self, flake-utils, ... }@inputs: flake-utils.lib.eachDefaultSystem (system: {
    packages = rec {
      default = nix-scheduler-hook;
      nix-scheduler-hook = import ./default.nix {
        pkgs = import inputs.nixpkgs { inherit system; };
        restclient-cpp = inputs.restclient-cpp;
      };
    };
  });
}
