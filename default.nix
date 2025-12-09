{ pkgs, nlohmann-json, restclient-cpp }:
with pkgs;
stdenv.mkDerivation {
  name = "nix-scheduler-hook";
  src = ./.;
  nativeBuildInputs = [
    meson
    cmake
    ninja
    boost
    pkg-config
    nlohmann_json
    curl
  ];

  buildInputs = [
    nix.libs.nix-util
    nix.libs.nix-store
    nix.libs.nix-main
  ];

  postUnpack = ''
    mkdir $sourceRoot/subprojects
    cp -r ${restclient-cpp} $sourceRoot/subprojects/restclient-cpp
    cp -r ${nlohmann-json} $sourceRoot/subprojects/nlohmann-json
  '';

  installPhase = ''
    mkdir -p $out/bin
    mv nsh $out/bin
  '';
}