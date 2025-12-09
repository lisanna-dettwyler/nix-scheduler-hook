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
    rapidjson
    gtest
    openssl
    zlib
    libblake3
    libarchive
    libsodium
    brotli
    libcpuid
    libseccomp
    sqlite
    libgit2
    boehmgc
    toml11
    bison
    flex
    lowdown
    editline
    perl540
    bzip2
    perl540Packages.Test2Harness
    perl540Packages.DBI
    perl540Packages.DBDSQLite
    rapidcheck
  ];

  postUnpack = ''
    mkdir $sourceRoot/subprojects
    cp -r ${restclient-cpp} $sourceRoot/subprojects/restclient-cpp
    cp -r ${nlohmann-json} $sourceRoot/subprojects/nlohmann-json
    cp -r ${nix.src} $sourceRoot/subprojects/nix_top
  '';

  installPhase = ''
    mkdir -p $out/bin
    mv demo $out/bin
  '';
}