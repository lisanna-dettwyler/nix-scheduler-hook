{ pkgs, restclient-cpp, openpbs, slurm }:
with pkgs;
stdenv.mkDerivation {
  name = "nix-scheduler-hook";
  src = ./src;

  nativeBuildInputs = [
    meson
    cmake
    ninja
    pkg-config
  ];

  buildInputs = [
    boost
    curl
    nix.libs.nix-util
    nix.libs.nix-store
    nix.libs.nix-main
    nlohmann_json
    openpbs
    slurm
  ];

  postUnpack = ''
    mkdir $sourceRoot/subprojects
    cp -r ${restclient-cpp} $sourceRoot/subprojects/restclient-cpp
  '';

  installPhase = ''
    mkdir -p $out/bin
    mv nsh $out/bin
    mkdir -p $out/lib
    mv subprojects/restclient-cpp/librestclient_cpp.so $out/lib
  '';
}
