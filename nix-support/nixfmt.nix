# Copy of `nixfmt-rfc-style` vendored from `nixpkgs` master:
# https://github.com/NixOS/nixpkgs/blob/ab6071eb54cc9b66dda436111d4f569e4e56cbf4/pkgs/by-name/ni/nixfmt-rfc-style/package.nix
{
  haskell,
  haskellPackages,
  lib,
  fetchpatch,
}:
let
  inherit (haskell.lib.compose) overrideCabal justStaticExecutables;

  overrides = {
    version = "unstable-2024-03-01";

    patches = [
      (fetchpatch {
        url = "https://github.com/serokell/nixfmt/commit/ca9c8975ed671112fdfce94f2e9e2ad3de480c9a.patch";
        hash = "sha256-UOSAYahSKBsqPMVcQJ3H26Eg2xpPAsNOjYMI6g+WTYU=";
      })
    ];
  };

  raw-pkg = haskellPackages.callPackage (
    {
      mkDerivation,
      base,
      cmdargs,
      directory,
      fetchzip,
      filepath,
      lib,
      megaparsec,
      mtl,
      parser-combinators,
      safe-exceptions,
      scientific,
      text,
      transformers,
      unix,
    }:
    mkDerivation {
      pname = "nixfmt";
      version = "0.5.0";
      src = fetchzip {
        url = "https://github.com/piegamesde/nixfmt/archive/2b5ee820690bae64cb4003e46917ae43541e3e0b.tar.gz";
        sha256 = "1i1jbc1q4gd7fpilwy6s3a583yl5l8d8rlmipygj61mpclg9ihqg";
      };
      isLibrary = true;
      isExecutable = true;
      libraryHaskellDepends = [
        base
        megaparsec
        mtl
        parser-combinators
        scientific
        text
        transformers
      ];
      executableHaskellDepends = [
        base
        cmdargs
        directory
        filepath
        safe-exceptions
        text
        unix
      ];
      jailbreak = true;
      homepage = "https://github.com/serokell/nixfmt";
      description = "An opinionated formatter for Nix";
      license = lib.licenses.mpl20;
      mainProgram = "nixfmt";
    }
  ) { };
in
lib.pipe raw-pkg [
  (overrideCabal overrides)
  justStaticExecutables
]
