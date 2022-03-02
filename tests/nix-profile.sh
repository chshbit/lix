source common.sh

clearStore
clearProfiles

# Make a flake.
flake1Dir=$TEST_ROOT/flake1
mkdir -p $flake1Dir

cat > $flake1Dir/flake.nix <<EOF
{
  description = "Bla bla";

  outputs = { self }: with import ./config.nix; rec {
    packages.$system.default = mkDerivation {
      name = "simple-\${builtins.readFile ./version}";
      builder = builtins.toFile "builder.sh"
        ''
          mkdir -p \$out/bin
          cat > \$out/bin/hello <<EOF
          #! ${shell}
          echo Hello \${builtins.readFile ./who}
          EOF
          chmod +x \$out/bin/hello
        '';
    };
  };
}
EOF

printf World > $flake1Dir/who
printf 1.0 > $flake1Dir/version

cp ./config.nix $flake1Dir/

# Test upgrading from nix-env.
nix-env -f ./user-envs.nix -i foo-1.0
nix profile list | grep '0 - - .*-foo-1.0'
nix profile install $flake1Dir -L
[[ $($TEST_HOME/.nix-profile/bin/hello) = "Hello World" ]]
nix profile history
nix profile history | grep "packages.$system.default: ∅ -> 1.0"
nix profile diff-closures | grep 'env-manifest.nix: ε → ∅'

# Test upgrading a package.
printf NixOS > $flake1Dir/who
printf 2.0 > $flake1Dir/version
nix profile upgrade 1
[[ $($TEST_HOME/.nix-profile/bin/hello) = "Hello NixOS" ]]
nix profile history | grep "packages.$system.default: 1.0 -> 2.0"

# Test 'history', 'diff-closures'.
nix profile diff-closures

# Test rollback.
nix profile rollback
[[ $($TEST_HOME/.nix-profile/bin/hello) = "Hello World" ]]

# Test uninstall.
[ -e $TEST_HOME/.nix-profile/bin/foo ]
nix profile remove 0
(! [ -e $TEST_HOME/.nix-profile/bin/foo ])
nix profile history | grep 'foo: 1.0 -> ∅'
nix profile diff-closures | grep 'Version 3 -> 4'

# Test wipe-history.
nix profile wipe-history
[[ $(nix profile history | grep Version | wc -l) -eq 1 ]]
