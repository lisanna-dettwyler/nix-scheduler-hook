{ pkgs, nixpkgs, nix-scheduler-hook }:
with pkgs;
let
  slurmconfig = {
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
  };
  inherit (import "${nixpkgs}/nixos/tests/ssh-keys.nix" pkgs)
    snakeOilPrivateKey
    snakeOilPublicKey
    ;
in
{
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
          submit.succeed("mkdir -p /etc/nix")
          submit.succeed("echo 'state-dir = /root/nsh' > /etc/nix/nsh.conf")
          submit.succeed("echo 'slurm-jwt-token = %s' >> /etc/nix/nsh.conf" % token)

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
                args = ["-c" "echo $${echo} > $out"];
                system = builtins.currentSystem;
                requiredSystemFeatures = ["nsh"];
              };
              dep = n: "dep$${n}";
            in mkDrv "test" "$${mkDrv (dep 1) (dep 1)} $${mkDrv (dep 2) (dep 2)} $${mkDrv (dep 3) (dep 3)}"'
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
    '';
  };
}
