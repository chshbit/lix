{
  description = "The purely functional package manager";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-23.11-small";
  inputs.nixpkgs-regression.url = "github:NixOS/nixpkgs/215d4d0fd80ca5163643b03a33fde804a29cc1e2";
  inputs.flake-compat = { url = "github:edolstra/flake-compat"; flake = false; };

  outputs = { self, nixpkgs, nixpkgs-regression, flake-compat }:

    let
      inherit (nixpkgs) lib;
      inherit (lib) fileset;

      officialRelease = true;

      # Set to true to build the release notes for the next release.
      buildUnreleasedNotes = false;

      version = lib.fileContents ./.version + versionSuffix;
      versionSuffix =
        if officialRelease
        then ""
        else "pre${builtins.substring 0 8 (self.lastModifiedDate or self.lastModified or "19700101")}_${self.shortRev or "dirty"}";

      linux32BitSystems = [ "i686-linux" ];
      linux64BitSystems = [ "x86_64-linux" "aarch64-linux" ];
      linuxSystems = linux32BitSystems ++ linux64BitSystems;
      darwinSystems = [ "x86_64-darwin" "aarch64-darwin" ];
      systems = linuxSystems ++ darwinSystems;

      crossSystems = [
        "armv6l-linux" "armv7l-linux"
        "x86_64-freebsd13" "x86_64-netbsd"
      ];

      stdenvs = [ "gccStdenv" "clangStdenv" "stdenv" "libcxxStdenv" "ccacheStdenv" ];

      forAllSystems = lib.genAttrs systems;

      forAllCrossSystems = lib.genAttrs crossSystems;

      forAllStdenvs = f:
        lib.listToAttrs
          (map
            (stdenvName: {
              name = "${stdenvName}Packages";
              value = f stdenvName;
            })
            stdenvs);

      # Memoize nixpkgs for different platforms for efficiency.
      nixpkgsFor = forAllSystems
        (system: let
          make-pkgs = crossSystem: stdenv: import nixpkgs {
            localSystem = {
              inherit system;
            };
            crossSystem = if crossSystem == null then null else {
              system = crossSystem;
            } // lib.optionalAttrs (crossSystem == "x86_64-freebsd13") {
              useLLVM = true;
            };
            overlays = [
              (overlayFor (p: p.${stdenv}))
            ];

            config.permittedInsecurePackages = [ "nix-2.13.6" ];
          };
          stdenvs = forAllStdenvs (make-pkgs null);
          native = stdenvs.stdenvPackages;
        in {
          inherit stdenvs native;
          static = native.pkgsStatic;
          cross = forAllCrossSystems (crossSystem: make-pkgs crossSystem "stdenv");
        });

      installScriptFor = systems:
        with nixpkgsFor.x86_64-linux.native;
        runCommand "installer-script"
          { buildInputs = [ nix ];
          }
          ''
            mkdir -p $out/nix-support

            # Converts /nix/store/50p3qk8k...-nix-2.4pre20201102_550e11f/bin/nix to 50p3qk8k.../bin/nix.
            tarballPath() {
              # Remove the store prefix
              local path=''${1#${builtins.storeDir}/}
              # Get the path relative to the derivation root
              local rest=''${path#*/}
              # Get the derivation hash
              local drvHash=''${path%%-*}
              echo "$drvHash/$rest"
            }

            substitute ${./scripts/install.in} $out/install \
              ${pkgs.lib.concatMapStrings
                (system: let
                    tarball = if builtins.elem system crossSystems then self.hydraJobs.binaryTarballCross.x86_64-linux.${system} else self.hydraJobs.binaryTarball.${system};
                  in '' \
                  --replace '@tarballHash_${system}@' $(nix --experimental-features nix-command hash-file --base16 --type sha256 ${tarball}/*.tar.xz) \
                  --replace '@tarballPath_${system}@' $(tarballPath ${tarball}/*.tar.xz) \
                  ''
                )
                systems
              } --replace '@nixVersion@' ${version}

            echo "file installer $out/install" >> $out/nix-support/hydra-build-products
          '';

      testNixVersions = pkgs: client: daemon: let
        nix = pkgs.callPackage ./package.nix {
          pname =
            "nix-tests"
            + lib.optionalString
            (lib.versionAtLeast daemon.version "2.4pre20211005" &&
            lib.versionAtLeast client.version "2.4pre20211005")
            "-${client.version}-against-${daemon.version}";

            inherit fileset;
        };
      in nix.overrideAttrs (prevAttrs: {
        NIX_DAEMON_PACKAGE = daemon;
        NIX_CLIENT_PACKAGE = client;

        dontBuild = true;
        doInstallCheck = true;

        configureFlags = [
          "RAPIDCHECK_HEADERS=${lib.getDev pkgs.rapidcheck}/extras/gtest/include"
          "--disable-build"
        ];

        installPhase = ''
          mkdir -p $out
        '';

        installCheckPhase = (lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
          export OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES
        '') + ''
          mkdir -p src/nix-channel
          make installcheck -j$NIX_BUILD_CORES -l$NIX_BUILD_CORES
        '';
      });

      binaryTarball = nix: pkgs:
        let
          inherit (pkgs) buildPackages;
          inherit (pkgs) cacert;
          installerClosureInfo = buildPackages.closureInfo { rootPaths = [ nix cacert ]; };
        in

        buildPackages.runCommand "nix-binary-tarball-${version}"
          { #nativeBuildInputs = lib.optional (system != "aarch64-linux") shellcheck;
            meta.description = "Distribution-independent Nix bootstrap binaries for ${pkgs.system}";
          }
          ''
            cp ${installerClosureInfo}/registration $TMPDIR/reginfo
            cp ${./scripts/create-darwin-volume.sh} $TMPDIR/create-darwin-volume.sh
            substitute ${./scripts/install-nix-from-closure.sh} $TMPDIR/install \
              --subst-var-by nix ${nix} \
              --subst-var-by cacert ${cacert}

            substitute ${./scripts/install-darwin-multi-user.sh} $TMPDIR/install-darwin-multi-user.sh \
              --subst-var-by nix ${nix} \
              --subst-var-by cacert ${cacert}
            substitute ${./scripts/install-systemd-multi-user.sh} $TMPDIR/install-systemd-multi-user.sh \
              --subst-var-by nix ${nix} \
              --subst-var-by cacert ${cacert}
            substitute ${./scripts/install-multi-user.sh} $TMPDIR/install-multi-user \
              --subst-var-by nix ${nix} \
              --subst-var-by cacert ${cacert}

            if type -p shellcheck; then
              # SC1090: Don't worry about not being able to find
              #         $nix/etc/profile.d/nix.sh
              shellcheck --exclude SC1090 $TMPDIR/install
              shellcheck $TMPDIR/create-darwin-volume.sh
              shellcheck $TMPDIR/install-darwin-multi-user.sh
              shellcheck $TMPDIR/install-systemd-multi-user.sh

              # SC1091: Don't panic about not being able to source
              #         /etc/profile
              # SC2002: Ignore "useless cat" "error", when loading
              #         .reginfo, as the cat is a much cleaner
              #         implementation, even though it is "useless"
              # SC2116: Allow ROOT_HOME=$(echo ~root) for resolving
              #         root's home directory
              shellcheck --external-sources \
                --exclude SC1091,SC2002,SC2116 $TMPDIR/install-multi-user
            fi

            chmod +x $TMPDIR/install
            chmod +x $TMPDIR/create-darwin-volume.sh
            chmod +x $TMPDIR/install-darwin-multi-user.sh
            chmod +x $TMPDIR/install-systemd-multi-user.sh
            chmod +x $TMPDIR/install-multi-user
            dir=nix-${version}-${pkgs.system}
            fn=$out/$dir.tar.xz
            mkdir -p $out/nix-support
            echo "file binary-dist $fn" >> $out/nix-support/hydra-build-products
            tar cvfJ $fn \
              --owner=0 --group=0 --mode=u+rw,uga+r \
              --mtime='1970-01-01' \
              --absolute-names \
              --hard-dereference \
              --transform "s,$TMPDIR/install,$dir/install," \
              --transform "s,$TMPDIR/create-darwin-volume.sh,$dir/create-darwin-volume.sh," \
              --transform "s,$TMPDIR/reginfo,$dir/.reginfo," \
              --transform "s,$NIX_STORE,$dir/store,S" \
              $TMPDIR/install \
              $TMPDIR/create-darwin-volume.sh \
              $TMPDIR/install-darwin-multi-user.sh \
              $TMPDIR/install-systemd-multi-user.sh \
              $TMPDIR/install-multi-user \
              $TMPDIR/reginfo \
              $(cat ${installerClosureInfo}/store-paths)
          '';

      overlayFor = getStdenv: final: prev:
        {
          nixStable = prev.nix;

          # Forward from the previous stage as we don’t want it to pick the lowdown override
          nixUnstable = prev.nixUnstable;

          changelog-d = final.buildPackages.callPackage ./misc/changelog-d.nix { };
          boehmgc-nix = (final.boehmgc.override {
            enableLargeConfig = true;
          }).overrideAttrs (o: {
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
            inherit (final) stdenv;
            inherit versionSuffix fileset;
            boehmgc = final.boehmgc-nix;
            busybox-sandbox-shell = final.busybox-sandbox-shell or final.default-busybox-sandbox-shell;
          };
        };

    in {
      # A Nixpkgs overlay that overrides the 'nix' and
      # 'nix.perl-bindings' packages.
      overlays.default = overlayFor (p: p.stdenv);

      hydraJobs = {

        # Binary package for various platforms.
        build = forAllSystems (system: self.packages.${system}.nix);

        # Perl bindings for various platforms.
        perlBindings = forAllSystems (system: nixpkgsFor.${system}.native.nix.perl-bindings);

        # Binary tarball for various platforms, containing a Nix store
        # with the closure of 'nix' package, and the second half of
        # the installation script.
        binaryTarball = forAllSystems (system: binaryTarball nixpkgsFor.${system}.native.nix nixpkgsFor.${system}.native);

        # docker image with Nix inside
        dockerImage = lib.genAttrs linux64BitSystems (system: self.packages.${system}.dockerImage);

        # API docs for Nix's unstable internal C++ interfaces.
        internal-api-docs = let
          nixpkgs = nixpkgsFor.x86_64-linux.native;
          inherit (nixpkgs) pkgs;

          nix = pkgs.callPackage ./package.nix {
            inherit versionSuffix fileset officialRelease buildUnreleasedNotes;
            inherit (pkgs) changelog-d;
            boehmgc = pkgs.boehmgc-nix;
            busybox-sandbox-shell = pkgs.busybox-sandbox-shell;
          };
        in
          nix.overrideAttrs (prev: {
            pname = "nix-internal-api-docs";

            outputs = [ "out" ];
            separateDebugInfo = false;

            nativeBuildInputs = prev.nativeBuildInputs ++ [ pkgs.doxygen ];

            # Depropagate the build inputs for the docs build.
            propagatedBuildInputs = [ ];
            buildInputs = prev.buildInputs ++ [ pkgs.buildPackages.doxygen ] ++ [
              pkgs.gtest
              pkgs.rapidcheck
            ] ++ prev.propagatedBuildInputs;

            configureFlags = prev.configureFlags ++ [
              "--enable-internal-api-docs"
              "RAPIDCHECK_HEADERS=${lib.getDev pkgs.rapidcheck}/extras/gtest/include"
            ];

            installTargets = [ "internal-api-html" ];

            # Convince the package.nix logic that we're building,
            # but don't actually run the build phase.
            env.dontBuild = true;

            doCheck = false;
            doInstallCheck = false;

            postInstall = ''
              mkdir -p $out/nix-support
              echo "doc internal-api-docs $out/share/doc/nix/internal-api/html" > $out/nix-support/hydra-build-products
              rm $out/lib -rf
            '';
          });

        # System tests.
        tests = import ./tests/nixos { inherit lib nixpkgs nixpkgsFor; } // {

          # Make sure that nix-env still produces the exact same result
          # on a particular version of Nixpkgs.
          evalNixpkgs =
            with nixpkgsFor.x86_64-linux.native;
            runCommand "eval-nixos" { buildInputs = [ nix ]; }
              ''
                type -p nix-env
                # Note: we're filtering out nixos-install-tools because https://github.com/NixOS/nixpkgs/pull/153594#issuecomment-1020530593.
                time nix-env --store dummy:// -f ${nixpkgs-regression} -qaP --drv-path | sort | grep -v nixos-install-tools > packages
                [[ $(sha1sum < packages | cut -c1-40) = ff451c521e61e4fe72bdbe2d0ca5d1809affa733 ]]
                mkdir $out
              '';

          nixpkgsLibTests =
            forAllSystems (system:
              import (nixpkgs + "/lib/tests/release.nix")
                { pkgs = nixpkgsFor.${system}.native;
                  nixVersions = [ self.packages.${system}.nix ];
                }
            );
        };

        installTests = forAllSystems (system:
          let pkgs = nixpkgsFor.${system}.native; in
          pkgs.runCommand "install-tests" {
            againstSelf = testNixVersions pkgs pkgs.nix pkgs.pkgs.nix;
            againstCurrentUnstable =
              # FIXME: temporarily disable this on macOS because of #3605.
              if system == "x86_64-linux"
              then testNixVersions pkgs pkgs.nix pkgs.nixUnstable
              else null;
            # Disabled because the latest stable version doesn't handle
            # `NIX_DAEMON_SOCKET_PATH` which is required for the tests to work
            # againstLatestStable = testNixVersions pkgs pkgs.nix pkgs.nixStable;
          } "touch $out");
      };

      checks = forAllSystems (system: {
        binaryTarball = self.hydraJobs.binaryTarball.${system};
        perlBindings = self.hydraJobs.perlBindings.${system};
        installTests = self.hydraJobs.installTests.${system};
        nixpkgsLibTests = self.hydraJobs.tests.nixpkgsLibTests.${system};
        rl-next =
          let pkgs = nixpkgsFor.${system}.native;
          in pkgs.buildPackages.runCommand "test-rl-next-release-notes" { } ''
          LANG=C.UTF-8 ${pkgs.changelog-d}/bin/changelog-d ${./doc/manual/rl-next} >$out
        '';
      } // (lib.optionalAttrs (builtins.elem system linux64BitSystems)) {
        dockerImage = self.hydraJobs.dockerImage.${system};
      });

      packages = forAllSystems (system: rec {
        inherit (nixpkgsFor.${system}.native) nix;
        default = nix;
      } // (lib.optionalAttrs (builtins.elem system linux64BitSystems) {
        nix-static = nixpkgsFor.${system}.static.nix;
        dockerImage =
          let
            pkgs = nixpkgsFor.${system}.native;
            image = import ./docker.nix { inherit pkgs; tag = version; };
          in
          pkgs.runCommand
            "docker-image-tarball-${version}"
            { meta.description = "Docker image with Nix for ${system}"; }
            ''
              mkdir -p $out/nix-support
              image=$out/image.tar.gz
              ln -s ${image} $image
              echo "file binary-dist $image" >> $out/nix-support/hydra-build-products
            '';
      } // builtins.listToAttrs (map
          (crossSystem: {
            name = "nix-${crossSystem}";
            value = nixpkgsFor.${system}.cross.${crossSystem}.nix;
          })
          crossSystems)
        // builtins.listToAttrs (map
          (stdenvName: {
            name = "nix-${stdenvName}";
            value = nixpkgsFor.${system}.stdenvs."${stdenvName}Packages".nix;
          })
          stdenvs)));

      devShells = let
        makeShell = pkgs: stdenv:
          let
            nix = pkgs.callPackage ./package.nix {
              inherit stdenv versionSuffix fileset;
              boehmgc = pkgs.boehmgc-nix;
              busybox-sandbox-shell = pkgs.busybox-sandbox-shell or pkgs.default-busybox-sandbox;
            };
          in
            nix.overrideAttrs (prev: {
              nativeBuildInputs = prev.nativeBuildInputs
                ++ lib.optional (stdenv.cc.isClang && !stdenv.buildPlatform.isDarwin) pkgs.buildPackages.bear
                ++ lib.optional
                  (stdenv.cc.isClang && stdenv.hostPlatform == stdenv.buildPlatform)
                  pkgs.buildPackages.clang-tools;
                src = null;

              doInstallCheck = false;
              doCheck = false;
              installFlags = "sysconfdir=$(out)/etc";
              strictDeps = false;

              shellHook = ''
                PATH=$prefix/bin:$PATH
                unset PYTHONPATH
                export MANPATH=$out/share/man:$MANPATH

                # Make bash completion work.
                XDG_DATA_DIRS+=:$out/share
              '';
            });
        in
        forAllSystems (system:
          let
            makeShells = prefix: pkgs:
              lib.mapAttrs'
              (k: v: lib.nameValuePair "${prefix}-${k}" v)
              (forAllStdenvs (stdenvName: makeShell pkgs pkgs.${stdenvName}));
          in
            (makeShells "native" nixpkgsFor.${system}.native) //
            (makeShells "static" nixpkgsFor.${system}.static) //
            (forAllCrossSystems (crossSystem: let pkgs = nixpkgsFor.${system}.cross.${crossSystem}; in makeShell pkgs pkgs.stdenv)) //
            {
              default = self.devShells.${system}.native-stdenvPackages;
            }
        );
  };
}
