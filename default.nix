{ pkgs, nix ? ./subprojects/nix, restc-cpp ? ./subprojects/restc-cpp }:
with pkgs;
stdenv.mkDerivation {
  name = "nix-scheduler-hook";
  src = ./.;
  nativeBuildInputs = [
    meson
    cmake
    ninja
    rapidjson
    gtest
    boost
    openssl
    zlib
  ];

  postUnpack = ''
    mkdir $sourceRoot/subprojects
    cp -r ${restc-cpp} $sourceRoot/subprojects/restc-cpp
  '';

  installPhase = ''
    mkdir -p $out/bin
    mv demo $out/bin
  '';
}