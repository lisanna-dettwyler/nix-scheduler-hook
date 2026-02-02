{
  description = "A build hook for Nix that dispatches builds through a job scheduler.";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    restclient-cpp = {
      url = "github:mrtazz/restclient-cpp";
      flake = false;
    };
    openpbs = {
      url = "github:openpbs/openpbs";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, ... }@inputs: flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = import nixpkgs { inherit system; };
      tclWithTk = with pkgs; symlinkJoin {
        name = "tcl-with-tk";
        paths = [
          tcl-8_5
          tk-8_5
          tk-8_5.dev
        ];
      };
      openpbs = with pkgs; stdenv.mkDerivation {
        name = "openpbs";
        src = inputs.openpbs;
        nativeBuildInputs = [ autoconf automake libtool gnum4 tclWithTk swig pkg-config cjson makeWrapper ];
        buildInputs = [ openssl zlib libxt libx11 libpq python3 expat libedit hwloc libical krb5 munge ];
        patchPhase = ''
          substituteInPlace src/cmds/scripts/Makefile.am --replace-fail "/etc/profile.d" "$out/etc/profile.d"
          substituteInPlace m4/pbs_systemd_unitdir.m4 --replace-fail "/usr/lib/systemd/system" "$out/lib/systemd/system"
        '';
        preConfigure = ''
          ./autogen.sh
        '';
        configureFlags = [
          "--with-tcl=${tclWithTk}"
          "--with-swig=${swig}"
          "--sysconfdir=$out/etc"
        ];
        postInstall = ''
          for file in pbs_dedicated pbs_holidays pbs_resource_group pbs_sched_config; do
              cp src/scheduler/$file $out/etc/
          done
        '';
        postFixup = ''
          substituteInPlace $out/libexec/pbs_habitat --replace-fail /bin/ls ls
          ${findutils}/bin/find $out/bin/ $out/sbin/ $out/libexec/ $out/lib/ -type f -exec file "{}" + |
            ${gawk}/bin/awk -F: '/ELF/ {print $1}' |
            xargs patchelf --add-needed libmunge.so --add-rpath ${munge}/lib
        '';
      };
    in rec {
      checks = import ./tests.nix {
        inherit nixpkgs pkgs openpbs;
        nix-scheduler-hook = packages.default;
      };
      packages = rec {
        inherit openpbs;
        default = nix-scheduler-hook;
        nix-scheduler-hook = import ./default.nix {
          inherit pkgs openpbs;
          slurm = with pkgs; symlinkJoin {
            name = "slurm";
            paths = [
              slurm
              slurm.dev
            ];
          };
          restclient-cpp = inputs.restclient-cpp;
        };
      };
    }
  );
}
