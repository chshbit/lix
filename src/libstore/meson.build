schema_sql_gen = gen_header.process('schema.sql')
ca_specific_schema_gen = gen_header.process('ca-specific-schema.sql')

libstore_sources = files(
  'binary-cache-store.cc',
  'build-result.cc',
  'common-protocol.cc',
  'content-address.cc',
  'crypto.cc',
  'daemon.cc',
  'derivations.cc',
  'derived-path-map.cc',
  'derived-path.cc',
  'downstream-placeholder.cc',
  'dummy-store.cc',
  'export-import.cc',
  'filetransfer.cc',
  'gc.cc',
  'globals.cc',
  'http-binary-cache-store.cc',
  'legacy-ssh-store.cc',
  'local-binary-cache-store.cc',
  'local-fs-store.cc',
  'local-store.cc',
  'lock.cc',
  'log-store.cc',
  'machines.cc',
  'make-content-addressed.cc',
  'misc.cc',
  'names.cc',
  'nar-accessor.cc',
  'nar-info-disk-cache.cc',
  'nar-info.cc',
  'optimise-store.cc',
  'outputs-spec.cc',
  'parsed-derivations.cc',
  'path-info.cc',
  'path-references.cc',
  'path-with-outputs.cc',
  'path.cc',
  'pathlocks.cc',
  'profiles.cc',
  'realisation.cc',
  'remote-fs-accessor.cc',
  'remote-store.cc',
  's3-binary-cache-store.cc',
  'serve-protocol.cc',
  'sqlite.cc',
  'ssh-store.cc',
  'ssh.cc',
  'store-api.cc',
  'uds-remote-store.cc',
  'worker-protocol.cc',
  'build/derivation-goal.cc',
  'build/drv-output-substitution-goal.cc',
  'build/entry-points.cc',
  'build/goal.cc',
  'build/hook-instance.cc',
  'build/local-derivation-goal.cc',
  'build/personality.cc',
  'build/substitution-goal.cc',
  'build/worker.cc',
  'builtins/buildenv.cc',
  'builtins/fetchurl.cc',
  'builtins/unpack-channel.cc',
)

all_sources += {
  'libstore': libstore_sources,
}

# These variables are created pseudo-dynamically, near the beginning of
# the top-level meson.build. Aside from prefix itself, each of these was
# made into an absolute path by joining it with prefix, unless it was already
# an absolute path (which is the default for store-dir, state-dir, and log-dir).
cpp_str_defines = {
  'LSOF': lsof.full_path(),
  'NIX_PREFIX':    prefix,
  'NIX_STORE_DIR': store_dir,
  'NIX_DATA_DIR':  datadir,
  'NIX_STATE_DIR': state_dir,
  'NIX_LOG_DIR':   log_dir,
  'NIX_CONF_DIR':  sysconfdir,
  'NIX_BIN_DIR':   bindir,
  'NIX_MAN_DIR':   mandir,
}

cpp_args = []

foreach name, value : cpp_str_defines
  cpp_args += [
    '-D' + name + '=' + '"' + value + '"'
  ]
endforeach

libstore = library(
  'nixstore',
  schema_sql_gen,
  ca_specific_schema_gen,
  libstore_sources,
  dependencies : [
    libarchive,
    liblixutil, # Internal.
    seccomp,
    sqlite,
    sodium,
    seccomp,
    curl,
    openssl,
    aws_sdk,
    aws_s3,
    aws_sdk_transfer,
  ],
  cpp_args : cpp_args,
)

# Used by libfetchers.
liblixstore = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libstore,
)
