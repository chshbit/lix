{
  description = "The purely functional package manager";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-23.11-small";
    nixpkgs-regression.url = "github:NixOS/nixpkgs/215d4d0fd80ca5163643b03a33fde804a29cc1e2";
    pre-commit-hooks = {
      # Can go back to `cachix/git-hooks.nix` when this is merged:
      # https://github.com/cachix/git-hooks.nix/pull/401
      url = "github:9999years/git-hooks.nix/add-default-pre-commit-hooks";
      inputs = {
        flake-compat.follows = "flake-compat";
        nixpkgs.follows = "nixpkgs";
        nixpkgs-stable.follows = "nixpkgs";
      };
    };
    flake-compat = {
      url = "github:edolstra/flake-compat";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      nixpkgs-regression,
      pre-commit-hooks,
      flake-compat,
    }:
    let
      inherit (nixpkgs) lib;
      inherit (lib) fileset;

      officialRelease = true;

      # Set to true to build the release notes for the next release.
      buildUnreleasedNotes = false;

      version = lib.fileContents ./.version + versionSuffix;
      versionSuffix =
        if officialRelease then
          ""
        else
          "pre${
            builtins.substring 0 8 (self.lastModifiedDate or self.lastModified or "19700101")
          }_${self.shortRev or "dirty"}";

      linux32BitSystems = [ "i686-linux" ];
      linux64BitSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      linuxSystems = linux32BitSystems ++ linux64BitSystems;
      darwinSystems = [
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      systems = linuxSystems ++ darwinSystems;

      crossSystems = [
        "armv6l-linux"
        "armv7l-linux"
        "x86_64-freebsd13"
        "x86_64-netbsd"
      ];

      stdenvs = [
        "gccStdenv"
        "clangStdenv"
        "stdenv"
        "libcxxStdenv"
        "ccacheStdenv"
      ];

      forAllSystems = lib.genAttrs systems;

      forAllCrossSystems = lib.genAttrs crossSystems;

      forAllStdenvs =
        f:
        lib.listToAttrs (
          map (stdenvName: {
            name = "${stdenvName}Packages";
            value = f stdenvName;
          }) stdenvs
        );

      # Memoize nixpkgs for different platforms for efficiency.
      nixpkgsFor = forAllSystems (
        system:
        let
          make-pkgs =
            crossSystem: stdenv:
            import nixpkgs {
              localSystem = {
                inherit system;
              };
              crossSystem =
                if crossSystem == null then
                  null
                else
                  {
                    system = crossSystem;
                  }
                  // lib.optionalAttrs (crossSystem == "x86_64-freebsd13") { useLLVM = true; };
              overlays = [
                (overlayFor (p: p.${stdenv}))
                (final: prev: { nixfmt = final.callPackage ./nix-support/nixfmt.nix { }; })
              ];

              config.permittedInsecurePackages = [ "nix-2.13.6" ];
            };
          stdenvs = forAllStdenvs (make-pkgs null);
          native = stdenvs.stdenvPackages;
        in
        {
          inherit stdenvs native;
          static = native.pkgsStatic;
          cross = forAllCrossSystems (crossSystem: make-pkgs crossSystem "stdenv");
        }
      );

      testNixVersions =
        pkgs: client: daemon:
        let
          nix = pkgs.callPackage ./package.nix {
            pname =
              "nix-tests"
              + lib.optionalString (
                lib.versionAtLeast daemon.version "2.4pre20211005"
                && lib.versionAtLeast client.version "2.4pre20211005"
              ) "-${client.version}-against-${daemon.version}";

            inherit fileset;
          };
        in
        nix.overrideAttrs (prevAttrs: {
          NIX_DAEMON_PACKAGE = daemon;
          NIX_CLIENT_PACKAGE = client;

          dontBuild = true;
          doInstallCheck = true;

          configureFlags = prevAttrs.configureFlags ++ [
            # We don't need the actual build here.
            "--disable-build"
          ];

          installPhase = ''
            mkdir -p $out
          '';

          installCheckPhase =
            lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
              export OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES
            ''
            + ''
              mkdir -p src/nix-channel
              make installcheck -j$NIX_BUILD_CORES -l$NIX_BUILD_CORES
            '';
        });

      binaryTarball =
        nix: pkgs:
        let
          inherit (pkgs) buildPackages;
          installerClosureInfo = buildPackages.closureInfo { rootPaths = [ nix ]; };
        in
        buildPackages.runCommand "nix-binary-tarball-${version}"
          {
            #nativeBuildInputs = lib.optional (system != "aarch64-linux") shellcheck;
            meta.description = "Distribution-independent Nix bootstrap binaries for ${pkgs.system}";
          }
          ''
            cp ${installerClosureInfo}/registration $TMPDIR/reginfo

            dir=nix-${version}-${pkgs.system}
            fn=$out/$dir.tar.xz
            mkdir -p $out/nix-support
            echo "file binary-dist $fn" >> $out/nix-support/hydra-build-products
            tar cvfJ $fn \
              --owner=0 --group=0 --mode=u+rw,uga+r \
              --mtime='1970-01-01' \
              --absolute-names \
              --hard-dereference \
              --transform "s,$TMPDIR/reginfo,$dir/.reginfo," \
              --transform "s,$NIX_STORE,$dir/store,S" \
              $TMPDIR/reginfo \
              $(cat ${installerClosureInfo}/store-paths)
          '';

      overlayFor =
        getStdenv: final: prev:
        let
          currentStdenv = getStdenv final;
          comDeps =
            with final;
            commonDeps {
              inherit pkgs;
              inherit (currentStdenv.hostPlatform) isStatic;
            };
        in
        {
          nixStable = prev.nix;

          # Forward from the previous stage as we don’t want it to pick the lowdown override
          nixUnstable = prev.nixUnstable;

          build-release-notes = final.buildPackages.callPackage ./maintainers/build-release-notes.nix { };
          clangbuildanalyzer = final.buildPackages.callPackage ./misc/clangbuildanalyzer.nix { };
          boehmgc-nix = (final.boehmgc.override { enableLargeConfig = true; }).overrideAttrs (o: {
            patches = (o.patches or [ ]) ++ [
              ./boehmgc-coroutine-sp-fallback.diff

              # https://github.com/ivmai/bdwgc/pull/586
              ./boehmgc-traceable_allocator-public.diff
            ];
          });

          default-busybox-sandbox-shell = final.busybox.override {
            useMusl = true;
            enableStatic = true;
            enableMinimal = true;
            extraConfig = ''
              CONFIG_FEATURE_FANCY_ECHO y
              CONFIG_FEATURE_SH_MATH y
              CONFIG_FEATURE_SH_MATH_64 y

              CONFIG_ASH y
              CONFIG_ASH_OPTIMIZE_FOR_SIZE y

              CONFIG_ASH_ALIAS y
              CONFIG_ASH_BASH_COMPAT y
              CONFIG_ASH_CMDCMD y
              CONFIG_ASH_ECHO y
              CONFIG_ASH_GETOPTS y
              CONFIG_ASH_INTERNAL_GLOB y
              CONFIG_ASH_JOB_CONTROL y
              CONFIG_ASH_PRINTF y
              CONFIG_ASH_TEST y
            '';
          };

          nix = final.callPackage ./package.nix {
            inherit versionSuffix fileset;
            stdenv = currentStdenv;
            boehmgc = final.boehmgc-nix;
            busybox-sandbox-shell = final.busybox-sandbox-shell or final.default-busybox-sandbox-shell;
          };
        };
    in
    {
      # A Nixpkgs overlay that overrides the 'nix' and
      # 'nix.perl-bindings' packages.
      overlays.default = overlayFor (p: p.stdenv);

      hydraJobs = {
        # Binary package for various platforms.
        build = forAllSystems (system: self.packages.${system}.nix);

        # FIXME(Qyriad): remove this when the migration to Meson has been completed.
        # NOTE: mesonBuildClang depends on mesonBuild depends on build to avoid OOMs
        # on aarch64 builders caused by too many parallel compiler/linker processes.
        mesonBuild = forAllSystems (
          system:
          (self.packages.${system}.nix.override { buildWithMeson = true; }).overrideAttrs (prev: {
            buildInputs = prev.buildInputs ++ [ self.packages.${system}.nix ];
          })
        );
        mesonBuildClang = forAllSystems (
          system:
          (nixpkgsFor.${system}.stdenvs.clangStdenvPackages.nix.override { buildWithMeson = true; })
          .overrideAttrs
            (prev: {
              buildInputs = prev.buildInputs ++ [ self.hydraJobs.mesonBuild.${system} ];
            })
        );

        # Perl bindings for various platforms.
        perlBindings = forAllSystems (system: nixpkgsFor.${system}.native.nix.perl-bindings);

        # Binary tarball for various platforms, containing a Nix store
        # with the closure of 'nix' package.
        binaryTarball = forAllSystems (
          system: binaryTarball nixpkgsFor.${system}.native.nix nixpkgsFor.${system}.native
        );

        # docker image with Nix inside
        dockerImage = lib.genAttrs linux64BitSystems (system: self.packages.${system}.dockerImage);

        # API docs for Nix's unstable internal C++ interfaces.
        internal-api-docs =
          let
            nixpkgs = nixpkgsFor.x86_64-linux.native;
            inherit (nixpkgs) pkgs;

            nix = pkgs.callPackage ./package.nix {
              inherit
                versionSuffix
                fileset
                officialRelease
                buildUnreleasedNotes
                ;
              inherit (pkgs) build-release-notes;
              internalApiDocs = true;
              boehmgc = pkgs.boehmgc-nix;
              busybox-sandbox-shell = pkgs.busybox-sandbox-shell;
            };
          in
          nix.overrideAttrs (prev: {
            # This Hydra job is just for the internal API docs.
            # We don't need the build artifacts here.
            dontBuild = true;
            doCheck = false;
            doInstallCheck = false;
          });

        # System tests.
        tests = import ./tests/nixos { inherit lib nixpkgs nixpkgsFor; } // {
          # Make sure that nix-env still produces the exact same result
          # on a particular version of Nixpkgs.
          evalNixpkgs =
            with nixpkgsFor.x86_64-linux.native;
            runCommand "eval-nixos" { buildInputs = [ nix ]; } ''
              type -p nix-env
              # Note: we're filtering out nixos-install-tools because https://github.com/NixOS/nixpkgs/pull/153594#issuecomment-1020530593.
              time nix-env --store dummy:// -f ${nixpkgs-regression} -qaP --drv-path | sort | grep -v nixos-install-tools > packages
              [[ $(sha1sum < packages | cut -c1-40) = 402242fca90874112b34718b8199d844e8b03d12 ]]
              mkdir $out
            '';

          nixpkgsLibTests = forAllSystems (
            system:
            import (nixpkgs + "/lib/tests/release.nix") {
              pkgs = nixpkgsFor.${system}.native;
              nixVersions = [ self.packages.${system}.nix ];
            }
          );
        };

        pre-commit = builtins.mapAttrs (
          system: pre-commit-lib:
          pre-commit-lib.run {
            src = self;
            hooks = {
              no-commit-to-branch = {
                enable = true;
                settings.branch = [ "main" ];
              };
              check-case-conflicts.enable = true;
              check-executables-have-shebangs = {
                enable = true;
                stages = [ "commit" ];
              };
              check-shebang-scripts-are-executable = {
                enable = true;
                stages = [ "commit" ];
              };
              check-symlinks = {
                enable = true;
                excludes = [ "^tests/functional/lang/symlink-resolution/broken$" ];
              };
              check-merge-conflicts.enable = true;
              end-of-file-fixer = {
                enable = true;
                excludes = [
                  "\\.drv$"
                  "^tests/functional/lang/"
                ];
              };
              mixed-line-endings = {
                enable = true;
                excludes = [ "^tests/functional/lang/" ];
              };
              # TODO: Once the test suite is nicer, clean up and start
              # enforcing trailing whitespace on tests that don't explicitly
              # check for it.
              trim-trailing-whitespace = {
                enable = true;
                stages = [ "commit" ];
                excludes = [ "^tests/functional/lang/" ];
              };
              treefmt = {
                enable = true;
                settings.formatters =
                  let
                    pkgs = nixpkgsFor.${system}.native;
                  in
                  [ pkgs.nixfmt ];
              };
            };
          }
        ) pre-commit-hooks.lib;
      };

      checks = forAllSystems (
        system:
        let
          rl-next-check =
            name: dir:
            let
              pkgs = nixpkgsFor.${system}.native;
            in
            pkgs.buildPackages.runCommand "test-${name}-release-notes" { } ''
              LANG=C.UTF-8 ${lib.getExe pkgs.build-release-notes} ${dir} >$out
            '';
        in
        {
          # FIXME(Qyriad): remove this when the migration to Meson has been completed.
          mesonBuild = self.hydraJobs.mesonBuild.${system};
          mesonBuildClang = self.hydraJobs.mesonBuildClang.${system};
          binaryTarball = self.hydraJobs.binaryTarball.${system};
          perlBindings = self.hydraJobs.perlBindings.${system};
          nixpkgsLibTests = self.hydraJobs.tests.nixpkgsLibTests.${system};
          rl-next = rl-next-check "rl-next" ./doc/manual/rl-next;
          rl-next-dev = rl-next-check "rl-next-dev" ./doc/manual/rl-next-dev;
          pre-commit = self.hydraJobs.pre-commit.${system};
        }
        // (lib.optionalAttrs (builtins.elem system linux64BitSystems)) {
          dockerImage = self.hydraJobs.dockerImage.${system};
        }
      );

      packages = forAllSystems (
        system:
        rec {
          inherit (nixpkgsFor.${system}.native) nix;
          default = nix;
        }
        // (
          lib.optionalAttrs (builtins.elem system linux64BitSystems) {
            nix-static = nixpkgsFor.${system}.static.nix;
            dockerImage =
              let
                pkgs = nixpkgsFor.${system}.native;
                image = import ./docker.nix {
                  inherit pkgs;
                  tag = version;
                };
              in
              pkgs.runCommand "docker-image-tarball-${version}"
                { meta.description = "Docker image with Nix for ${system}"; }
                ''
                  mkdir -p $out/nix-support
                  image=$out/image.tar.gz
                  ln -s ${image} $image
                  echo "file binary-dist $image" >> $out/nix-support/hydra-build-products
                '';
          }
          // builtins.listToAttrs (
            map (crossSystem: {
              name = "nix-${crossSystem}";
              value = nixpkgsFor.${system}.cross.${crossSystem}.nix;
            }) crossSystems
          )
          // builtins.listToAttrs (
            map (stdenvName: {
              name = "nix-${stdenvName}";
              value = nixpkgsFor.${system}.stdenvs."${stdenvName}Packages".nix;
            }) stdenvs
          )
        )
      );

      devShells =
        let
          makeShell =
            pkgs: stdenv:
            let
              nix = pkgs.callPackage ./package.nix {
                inherit stdenv versionSuffix fileset;
                boehmgc = pkgs.boehmgc-nix;
                busybox-sandbox-shell = pkgs.busybox-sandbox-shell or pkgs.default-busybox-sandbox;
                forDevShell = true;
              };
              pre-commit = self.hydraJobs.pre-commit.${pkgs.system} or { };
            in
            (nix.override {
              buildUnreleasedNotes = true;
              officialRelease = false;
            }).overrideAttrs
              (
                prev:
                {
                  # Required for clang-tidy checks
                  buildInputs =
                    prev.buildInputs
                    ++ lib.optional (pre-commit ? enabledPackages) pre-commit.enabledPackages
                    # Unfortunately `git-hooks.nix` can't propagate `treefmt`
                    # formatters into `enabledPackages` correctly.
                    ++ [ pkgs.nixfmt ]
                    ++ lib.optionals (stdenv.cc.isClang) [
                      pkgs.llvmPackages.llvm
                      pkgs.llvmPackages.clang-unwrapped.dev
                    ];
                  nativeBuildInputs =
                    prev.nativeBuildInputs
                    ++ lib.optional (stdenv.cc.isClang && !stdenv.buildPlatform.isDarwin) pkgs.buildPackages.bear
                    # Required for clang-tidy checks
                    ++ lib.optionals (stdenv.cc.isClang) [
                      pkgs.buildPackages.cmake
                      pkgs.buildPackages.ninja
                      pkgs.buildPackages.llvmPackages.llvm.dev
                    ]
                    ++
                      lib.optional (stdenv.cc.isClang && stdenv.hostPlatform == stdenv.buildPlatform)
                        # for some reason that seems accidental and was changed in
                        # NixOS 24.05-pre, clang-tools is pinned to LLVM 14 when
                        # default LLVM is newer.
                        (pkgs.buildPackages.clang-tools.override { inherit (pkgs.buildPackages) llvmPackages; })
                    ++ [
                      # FIXME(Qyriad): remove once the migration to Meson is complete.
                      pkgs.buildPackages.meson
                      pkgs.buildPackages.ninja

                      pkgs.buildPackages.clangbuildanalyzer
                    ];

                  src = null;

                  installFlags = "sysconfdir=$(out)/etc";
                  strictDeps = false;

                  shellHook = ''
                    PATH=$prefix/bin:$PATH
                    unset PYTHONPATH
                    export MANPATH=$out/share/man:$MANPATH

                    # Make bash completion work.
                    XDG_DATA_DIRS+=:$out/share

                    ${lib.optionalString (pre-commit ? shellHook) pre-commit.shellHook}
                  '';
                }
                // lib.optionalAttrs (stdenv.buildPlatform.isLinux && pkgs.glibcLocales != null) {
                  # Required to make non-NixOS Linux not complain about missing locale files during configure in a dev shell
                  LOCALE_ARCHIVE = "${lib.getLib pkgs.glibcLocales}/lib/locale/locale-archive";
                }
              );
        in
        forAllSystems (
          system:
          let
            makeShells =
              prefix: pkgs:
              lib.mapAttrs' (k: v: lib.nameValuePair "${prefix}-${k}" v) (
                forAllStdenvs (stdenvName: makeShell pkgs pkgs.${stdenvName})
              );
          in
          (makeShells "native" nixpkgsFor.${system}.native)
          // (makeShells "static" nixpkgsFor.${system}.static)
          // (forAllCrossSystems (
            crossSystem:
            let
              pkgs = nixpkgsFor.${system}.cross.${crossSystem};
            in
            makeShell pkgs pkgs.stdenv
          ))
          // {
            default = self.devShells.${system}.native-stdenvPackages;
          }
        );
    };
}
