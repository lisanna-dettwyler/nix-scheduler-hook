{ pkgs, restclient-cpp }:
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
  '';

  installPhase = ''
    mkdir -p $out/bin
    mv nsh $out/bin
    mkdir -p $out/lib
    mv subprojects/restclient-cpp/librestclient_cpp.so $out/lib
  '';
}
