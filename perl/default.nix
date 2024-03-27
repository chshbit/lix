{
  lib,
  fileset,
  stdenv,
  perl,
  perlPackages,
  autoconf-archive,
  autoreconfHook,
  pkg-config,
  nix,
  curl,
  bzip2,
  xz,
  boost,
  libsodium,
  darwin,
}:
perl.pkgs.toPerlModule (
  stdenv.mkDerivation {
    name = "nix-perl-${nix.version}";

    src = fileset.toSource {
      root = ../.;
      fileset = fileset.unions [
        ../.version
        ../m4
        ../mk
        ./MANIFEST
        ./Makefile
        ./Makefile.config.in
        ./configure.ac
        ./lib
        ./local.mk
      ];
    };

    nativeBuildInputs = [
      autoconf-archive
      autoreconfHook
      pkg-config
    ];

    buildInputs =
      [
        nix
        curl
        bzip2
        xz
        perl
        boost
      ]
      ++ lib.optional (stdenv.isLinux || stdenv.isDarwin) libsodium
      ++ lib.optional stdenv.isDarwin darwin.apple_sdk.frameworks.Security;

    configureFlags = [
      "--with-dbi=${perlPackages.DBI}/${perl.libPrefix}"
      "--with-dbd-sqlite=${perlPackages.DBDSQLite}/${perl.libPrefix}"
    ];

    enableParallelBuilding = true;

    postUnpack = "sourceRoot=$sourceRoot/perl";
  }
)
