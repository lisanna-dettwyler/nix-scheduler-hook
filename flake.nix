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
      # url = "github:openpbs/openpbs";
      url = "github:lisanna-dettwyler/openpbs/fix-tcl8";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, ... }@inputs: flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = import nixpkgs { inherit system; };
      # nix = pkgs.nix.overrideSource { name = "nix"; outPath = ./nix; };
      nix = pkgs.nix.overrideSource { name = "patched"; outPath = builtins.path {
        name = "patched";
        path = ./nix;
      }; };
      # nix = pkgs.nix.overrideSource ./nix;
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
        # version = "23.06.06";
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
        dontStrip = true;
        CFLAGS = "-O0 -g";
        CXXFLAGS = "-O0 -g";
        postInstall = ''
          substituteInPlace $out/libexec/pbs_habitat --replace-fail /bin/ls ls
          export -f wrapProgram
          export -f wrapProgramShell
          export -f assertExecutable
          export -f makeShellWrapper
          # substituteInPlace $(${findutils}/bin/find $out/bin/ $out/libexec/ -type f -exec ${file}/bin/file "{}" + | ${gawk}/bin/awk -F: '/text/ {print $1}') \
          #   --replace-quiet "2> /dev/null" "" \
          #   --replace-quiet "2>/dev/null" "" \
          #   --replace-quiet "> /dev/null" "" \
          #   --replace-quiet ">/dev/null" ""
          # ${findutils}/bin/find $out/bin/ $out/libexec/ -type f -exec file "{}" + | ${gawk}/bin/awk -F: '/ELF/ {print $1}' | xargs patchelf --add-rpath ${munge}/lib --add-needed libmunge.so
          ${findutils}/bin/find $out/bin/ $out/libexec/ -type f -exec file "{}" + | ${gawk}/bin/awk -F: '/ELF/ {print $1}' | xargs -I '{}' bash -c "wrapProgram {} --set LD_LIBRARY_PATH ${munge}/lib"
          # ls $out/libexec | xargs -I '{}' bash -c "wrapProgram $out/libexec/{} --set LD_LIBRARY_PATH ${munge}/lib --run 'set +e' --run 'set -x'"
          for file in pbs_dedicated pbs_holidays pbs_resource_group pbs_sched_config; do
              cp src/scheduler/$file $out/etc/
          done
        '';
      };
    in rec {
      checks = import ./tests.nix {
        inherit nixpkgs pkgs openpbs nix;
        nix-scheduler-hook = packages.default;
      };
      packages = rec {
        inherit openpbs nix;
        nixDebug = pkgs.symlinkJoin {
          name = "nix-debug";
          paths = [
            nix.nix-cli.debug
          ] ++ (map (p: p.value.debug or nix.nix-cli.debug) (pkgs.lib.attrsToList nix.libs));
          stripPrefix = "/lib/debug";
        };
        default = nix-scheduler-hook;
        nix-scheduler-hook = import ./default.nix {
          inherit pkgs openpbs;
          restclient-cpp = inputs.restclient-cpp;
        };
      };
    }
  );
}
