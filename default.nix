{ pkgs, restc-cpp ? ./subprojects/restc-cpp }:
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
    pkg-config
    libblake3
    libarchive
    libsodium
    brotli
    libcpuid
    nlohmann_json
    curl
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
    cp -r ${restc-cpp} $sourceRoot/subprojects/restc-cpp
    cp -r ${nix.src} $sourceRoot/subprojects/nix_top
  '';

  installPhase = ''
    mkdir -p $out/bin
    mv demo $out/bin
  '';
}