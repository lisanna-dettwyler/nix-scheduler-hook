{ pkgs, nixpkgs, nix-scheduler-hook, openpbs, nix }@args:
with pkgs;
let
  slurmconfig = {
    environment.systemPackages = [ (gdb.overrideAttrs (final: prev: {
      src = fetchgit {
        url = "https://sourceware.org/git/binutils-gdb.git";
        rev = "a72b83ab3792532b66cc5c472a20476a8a2fd969";
        hash = "sha256-0cT3/jXctvs9rVKvpAiqGkdMyb2Yfy1S0D6ullFlbkE=";
      };
      preConfigure = ''
        # remove precompiled docs, required for man gdbinit to mention /etc/gdb/gdbinit
        rm -f gdb/doc/*.info*
        rm -f gdb/doc/*.5
        rm -f gdb/doc/*.1
        # fix doc build https://sourceware.org/bugzilla/show_bug.cgi?id=27808
        rm -f gdb/doc/GDBvn.texi

        # GDB have to be built out of tree.
        mkdir _build
        cd _build
      '';
      nativeBuildInputs = prev.nativeBuildInputs ++ [ bison flex ];
      # enableParallelBuilding = false;
    })) ];
    # nix.package = nix.overrideAttrs (final: prev: {
    #   mesonBuildType = "debug";
    #   dontStrip = true;
    #   src = ./nix;
    # });
    nix.package = args.nix;
    services.slurm = {
      controlMachine = "control";
      nodeName = [ "node[1-3] CPUs=1 State=UNKNOWN" ];
      partitionName = [ "debug Nodes=node[1-3] Default=YES MaxTime=INFINITE State=UP" ];
      extraConfig = ''
        AccountingStorageHost=dbd
        AccountingStorageType=accounting_storage/slurmdbd
        AuthAltTypes=auth/jwt
      '';
    };
    networking.firewall.enable = false;
    systemd.tmpfiles.rules = [
      "f /etc/munge/munge.key 0400 munge munge - mungeverryweakkeybuteasytointegratoinatest"
    ];
    nix.settings.substitute = false;
    nix.settings.build-hook = "${nix-scheduler-hook}/bin/nsh";
    # nix.settings.sandbox-paths = "/tmp";
  };
  pbsConfig = {
    networking.firewall.enable = false;
    environment.etc."pbs.conf".text = ''
      PBS_EXEC=${openpbs}
      PBS_SERVER=pbs
      PBS_START_SERVER=1
      PBS_START_SCHED=1
      PBS_START_COMM=1
      PBS_START_MOM=1
      PBS_HOME=/var/spool/pbs
      PBS_CORE_LIMIT=unlimited
      PBS_SUPPORTED_AUTH_METHODS=munge
      PBS_AUTH_METHOD=MUNGE
    '';
    systemd.tmpfiles.rules = [
      "f /etc/munge/munge.key 0400 munge munge - mungeverryweakkeybuteasytointegratoinatest"
    ];
    services.munge.enable = true;
    environment.variables.LD_LIBRARY_PATH = "${munge}/lib";
    environment.systemPackages = [ openpbs ];
    services.openssh.enable = true;
    users.users.root.openssh.authorizedKeys.keys = [
      snakeOilPublicKey
    ];
    nix.settings.substitute = false;
    nix.settings.build-hook = "${nix-scheduler-hook}/bin/nsh";
    nix.extraOptions = ''
      sandbox-paths =
    '';
  };
  inherit (import "${nixpkgs}/nixos/tests/ssh-keys.nix" pkgs)
    snakeOilPrivateKey
    snakeOilPublicKey
    ;
  guestSystem =
    if stdenv.hostPlatform.isLinux then
      stdenv.hostPlatform.system
    else
      let
        hostToGuest = {
          "x86_64-darwin" = "x86_64-linux";
          "aarch64-darwin" = "aarch64-linux";
        };
        supportedHosts = lib.concatStringsSep ", " (lib.attrNames hostToGuest);
        message = "NixOS Test: don't know which VM guest system to pair with VM host system: ${hostPlatform.system}. Perhaps you intended to run the tests on a Linux host, or one of the following systems that may run NixOS tests: ${supportedHosts}";
      in
      hostToGuest.${hostPlatform.system} or (throw message);
in
{
  debugTests = testers.nixosTest {
    name = "debug tests";
    interactive.sshBackdoor.enable = true;
    nodes.main = { ... }: {};
    testScript = ''
      c = """
        nix-build -E 'derivation { name = "test"; builder = "/bin/sh"; args = ["-c" "echo something > $out"]; system = builtins.currentSystem; REBUILD = builtins.currentTime; }'
      """
      main.succeed(c)
    '';
  };
  slurmTests = testers.nixosTest {
    name = "Basic Slurm Tests";
    interactive.sshBackdoor.enable = true;
    nodes =
      let
        computeNode =
          { ... }:
          {
            imports = [ slurmconfig ];
            services.slurm.client.enable = true;
            services.openssh.enable = true;
            users.users.root.openssh.authorizedKeys.keys = [
              snakeOilPublicKey
            ];
          };
      in
      {
        control =
          { ... }:
          {
            imports = [ slurmconfig ];
            services.slurm.server.enable = true;
            systemd.tmpfiles.rules = [
              "f /var/spool/slurmctld/jwt_hs256.key 0400 slurm slurm - thisisjustanexamplejwttoken0000"
            ];
          };

        dbd =
          { pkgs, ... }:
          let
            passFile = pkgs.writeText "dbdpassword" "password123";
          in
          {
            networking.firewall.enable = false;
            systemd.tmpfiles.rules = [
              "f /etc/munge/munge.key 0400 munge munge - mungeverryweakkeybuteasytointegratoinatest"
            ];
            services.slurm.dbdserver = {
              enable = true;
              storagePassFile = "${passFile}";
            };
            services.mysql = {
              enable = true;
              package = pkgs.mariadb;
              initialScript = pkgs.writeText "mysql-init.sql" ''
                CREATE USER 'slurm'@'localhost' IDENTIFIED BY 'password123';
                GRANT ALL PRIVILEGES ON slurm_acct_db.* TO 'slurm'@'localhost';
              '';
              ensureDatabases = [ "slurm_acct_db" ];
              ensureUsers = [
                {
                  ensurePermissions = {
                    "slurm_acct_db.*" = "ALL PRIVILEGES";
                  };
                  name = "slurm";
                }
              ];
            };
          };

        submit =
          { ... }:
          {
            imports = [ slurmconfig ];
            services.slurm.enableStools = true;
            services.slurm.rest.enable = true;
            # services.slurm.rest.debug = "debug";
            virtualisation.memorySize = 8192;
          };

        node1 = computeNode;
        node2 = computeNode;
        node3 = computeNode;
      };

    testScript = ''
      with subtest("can_start_slurmdbd"):
          dbd.wait_for_unit("slurmdbd.service")
          dbd.wait_for_open_port(6819)

      with subtest("cluster_is_initialized"):
          control.wait_for_unit("multi-user.target")
          control.wait_for_unit("slurmctld.service")
          submit.wait_until_succeeds("sinfo | tail -n-1 | awk '{ print $1 }' | grep debug")

      start_all()

      with subtest("can_start_slurmd"):
          for node in [node1, node2, node3]:
              node.wait_for_unit("slurmd")

      submit.wait_for_unit("multi-user.target")

      with subtest("run_distributed_command"):
          submit.succeed("srun hostname")

      submit.wait_for_unit("slurmrestd.service")

      with subtest("generate_config"):
          token = control.succeed("scontrol token lifespan=infinite").split('=')[1].rstrip()
          submit.succeed("echo 'slurm-state-dir = /root/nsh' > /etc/nix/nsh.conf")
          submit.succeed("echo 'slurm-jwt-token = %s' >> /etc/nix/nsh.conf" % token)
          submit.succeed("echo 'system = %s' >> /etc/nix/nsh.conf" % "${guestSystem}")

      build_derivation_simple = """
        nix-build -E '
            derivation {
              name = "test";
              builder = "/bin/sh";
              args = ["-c" "echo something > $out"];
              system = builtins.currentSystem;
              requiredSystemFeatures = [ "nsh" ];
              REBUILD = builtins.currentTime;
            }'
      """

      submit.succeed("mkdir -p ~/.ssh")
      submit.succeed("cat ${snakeOilPrivateKey} > ~/.ssh/privkey.snakeoil")
      submit.succeed("chmod 600 ~/.ssh/privkey.snakeoil")
      submit.succeed("echo 'Host node*' >> ~/.ssh/config")
      submit.succeed("echo '  IdentityFile ~/.ssh/privkey.snakeoil' >> ~/.ssh/config")
      submit.succeed("echo '  StrictHostKeyChecking no' >> ~/.ssh/config")

      with subtest("run_nix_build_simple"):
          submit.succeed(build_derivation_simple)

      build_derivation_deps = """
        nix-build \
          --option build-hook ${nix-scheduler-hook}/bin/nsh \
          --option substitute false \
          -E '
            let
              mkDrv = name: echo: derivation {
                inherit name;
                builder = "/bin/sh";
                args = ["-c" ("echo " + echo + " > $out")];
                system = builtins.currentSystem;
                requiredSystemFeatures = ["nsh"];
              };
            in mkDrv "test-deps" ((mkDrv "dep1" "dep1") + (mkDrv "dep2" "dep2") + (mkDrv "dep3" "dep3"))'
      """

      with subtest("run_nix_build_deps"):
          submit.succeed(build_derivation_deps)

      build_derivation_unsupported_system_features = """
        nix-build \
          --option build-hook ${nix-scheduler-hook}/bin/nsh \
          --option substitute false \
          -E '
            derivation {
              name = "test";
              builder = "/bin/sh";
              args = ["-c" "echo something > $out"];
              system = builtins.currentSystem;
              requiredSystemFeatures = [ "unsupported" ];
            }'
      """

      with subtest("run_nix_build_negative_system_features"):
          submit.fail(build_derivation_unsupported_system_features)

      build_derivation_unsupported_system = """
        nix-build \
          --option build-hook ${nix-scheduler-hook}/bin/nsh \
          --option substitute false \
          -E '
            derivation {
              name = "test";
              builder = "/bin/sh";
              args = ["-c" "echo something > $out"];
              system = "bogus-system";
              requiredSystemFeatures = [ "nsh" ];
            }'
      """

      with subtest("run_nix_build_negative_system"):
          submit.fail(build_derivation_unsupported_system)

      with subtest("run_nix_build_invalid_scheduler"):
          submit.succeed("echo 'job-scheduler = invalid' >> /etc/nix/nsh.conf")
          submit.fail(build_derivation_simple)
      submit.succeed("sed -i s/invalid/slurm/g /etc/nix/nsh.conf")

      with subtest("run_nix_build_negative_ssh"):
          submit.succeed("sed -i s/snakeoil/snake/g ~/.ssh/config")
          submit.fail(build_derivation_simple)
      submit.succeed("sed -i s/snake/snakeoil/g ~/.ssh/config")

      with subtest("run_nix_build_custom_store"):
          submit.succeed("echo 'remote-store = /store' >> /etc/nix/nsh.conf")
          submit.succeed(build_derivation_simple)
      submit.succeed("sed -i 's|/store|auto|g' /etc/nix/nsh.conf")
    '';
  };

  pbsTests = testers.nixosTest {
    name = "Basic PBS Tests";
    interactive.sshBackdoor.enable = true;
    nodes.submit = {
      imports = [ pbsConfig ];
    };
    nodes.pbs = {
      security.sudo.enable = true;
      virtualisation.diskSize = 2048;
      imports = [ pbsConfig ];
      systemd.services.pbs = {
        path = [
          gnused
          coreutils
          hostname
          getent
          gnugrep
          gawk
          postgresql
          su
          procps
          python3
        ];
        wantedBy = [ "multi-user.target" ];
        after = [
          "systemd-tmpfiles-clean.service"
          "network-online.target"
          "remote-fs.target"
        ];
        wants = [ "network-online.target" ];
        serviceConfig = {
          Type = "forking";
          ExecStart = "${openpbs}/libexec/pbs_init.d start";
          ExecReload = "${openpbs}/libexec/pbs_init.d restart";
          ExecStop = "${openpbs}/libexec/pbs_init.d stop";
        };
        preStart = ''
          mkdir -p /var/spool/pbs
        '';
        environment.LD_LIBRARY_PATH = "${munge}/lib";
      };
      services.postgresql.enable = true;
    };
    testScript = ''
      start_all()
      pbs.wait_for_unit("pbs.service")
      pbs.succeed("qmgr -c 'set node pbs queue=workq'")
      pbs.succeed("qmgr -c 'set node pbs resources_available.ncpus=1'")
      pbs.succeed("qmgr -c 'set server acl_roots=root'")
      pbs.succeed("qmgr -c 'set server flatuid=true'")
      pbs.succeed("qmgr -c 'set server job_history_enable=true'")
      pbs.wait_until_succeeds("pbsnodes pbs | grep 'state = free'")
      submit.wait_for_unit("multi-user.target")

      submit.succeed("mkdir -p /etc/nix")
      submit.succeed("echo 'system = %s' >> /etc/nix/nsh.conf" % "${guestSystem}")
      submit.succeed("echo 'job-scheduler = pbs' >> /etc/nix/nsh.conf")
      submit.succeed("echo 'pbs-host = pbs' >> /etc/nix/nsh.conf")

      submit.succeed("mkdir -p ~/.ssh")
      submit.succeed("cat ${snakeOilPrivateKey} > ~/.ssh/privkey.snakeoil")
      submit.succeed("chmod 600 ~/.ssh/privkey.snakeoil")
      submit.succeed("echo 'Host pbs' >> ~/.ssh/config")
      submit.succeed("echo '  IdentityFile ~/.ssh/privkey.snakeoil' >> ~/.ssh/config")
      submit.succeed("echo '  StrictHostKeyChecking no' >> ~/.ssh/config")

      pbs.succeed("mkdir -p ~/.ssh")
      pbs.succeed("cat ${snakeOilPrivateKey} > ~/.ssh/privkey.snakeoil")
      pbs.succeed("chmod 600 ~/.ssh/privkey.snakeoil")
      pbs.succeed("echo 'Host submit' >> ~/.ssh/config")
      pbs.succeed("echo '  IdentityFile ~/.ssh/privkey.snakeoil' >> ~/.ssh/config")
      pbs.succeed("echo '  StrictHostKeyChecking no' >> ~/.ssh/config")
      pbs.succeed("echo 'Host pbs' >> ~/.ssh/config")
      pbs.succeed("echo '  IdentityFile ~/.ssh/privkey.snakeoil' >> ~/.ssh/config")
      pbs.succeed("echo '  StrictHostKeyChecking no' >> ~/.ssh/config")

      build_derivation_simple = """
        nix-build \
          --option build-hook ${nix-scheduler-hook}/bin/nsh \
          --option substitute false \
          -E '
            derivation {
              name = "test";
              builder = "/bin/sh";
              args = ["-c" "echo something > $out"];
              system = builtins.currentSystem;
              requiredSystemFeatures = [ "nsh" ];
              REBUILD = builtins.currentTime;
            }'
      """

      with subtest("run_nix_build_simple"):
          submit.succeed(build_derivation_simple)

      build_derivation_deps = """
        nix-build \
          --option build-hook ${nix-scheduler-hook}/bin/nsh \
          --option substitute false \
          -E '
            let
              mkDrv = name: echo: derivation {
                inherit name;
                builder = "/bin/sh";
                args = ["-c" ("echo " + echo + " > $out")];
                system = builtins.currentSystem;
                requiredSystemFeatures = ["nsh"];
                REBUILD = builtins.currentTime;
              };
            in mkDrv "test-deps" ((mkDrv "dep1" "dep1") + (mkDrv "dep2" "dep2") + (mkDrv "dep3" "dep3"))'
      """

      with subtest("run_nix_build_deps"):
          submit.succeed(build_derivation_deps)
    '';
  };
}
