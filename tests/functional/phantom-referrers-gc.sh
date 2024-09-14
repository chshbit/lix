source common.sh

startDaemon

requireDaemonNewerThan "2.92.0"
requireSqliteDatabase

clearStore

depOutPath=$(nix-build --no-out-link -E '
  with import ./config.nix;

  mkDerivation {
    name = "phantom";
    outputs = [ "out" ];
    buildCommand = "
      echo i will become a phantom soon > $out
    ";
  }
')

finalOutPath=$(nix-build --no-out-link -E '
  with import ./config.nix;

  let dep = mkDerivation {
    name = "phantom";
    outputs = [ "out" ];
    buildCommand = "
      echo i will become a phantom soon > $out
    ";
  }; in

  mkDerivation {
    name = "phantom-gc";
    outputs = [ "out" ];
    buildCommand = "
      echo UNUSED: ${dep} > $out
    ";
  }
')

echo "displaying all valid paths"
sqlite3 "$NIX_SQLITE_DATABASE" <<EOF
  select * from validpaths;
EOF


echo "displaying the relevant IDs..."
sqlite3 "$NIX_SQLITE_DATABASE" <<EOF
  select r.referrer, r.reference from Refs r join ValidPaths vp on r.referrer = vp.id where path = '$finalOutPath';
EOF

echo "corrupting the SQLite database manually..."
sqlite3 "$NIX_SQLITE_DATABASE" <<EOF
  pragma foreign_keys = off;
  delete from ValidPaths where path = '$finalOutPath';
  select * from Refs;
EOF

restartDaemon
# expect this to work and maybe warn about phantom referrers
expectStderr 0 nix-collect-garbage -vvvv | grepQuiet 'phantom referrers'
