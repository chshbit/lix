find_program('lsof')

# Yes this is really what it does.
# FIXME: do we. really need to rely on the shell for this?
gen_script = '''
  echo 'R"foo(' >> @OUTPUT@.tmp &&
  cat < @INPUT@ >> @OUTPUT@.tmp &&
  echo ')foo"' >> @OUTPUT@.tmp &&
  mv @OUTPUT@.tmp @OUTPUT
'''.replace('\n', ' ')

schema_sql_gen = custom_target(
  input : 'schema.sql',
  output : 'schema.sql.gen.hh',
  command : [
    'bash', '-c',
    gen_script,
  ],
)

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

cpp_str_defines = {
  'NIX_PREFIX': get_option('prefix'),
  'LSOF': lsof.full_path(),
  'NIX_STORE_DIR': get_option('store-dir'),
  'NIX_DATA_DIR':  get_option('prefix') / 'share', # FIXME: make separately-configurable
  'NIX_STATE_DIR': get_option('prefix') / 'nix', # FIXME: same
  'NIX_LOG_DIR':   get_option('prefix') / 'log' / 'nix', # FIXME: same
  'NIX_CONF_DIR':  get_option('prefix') / 'etc', # FIXME: same
  'NIX_BIN_DIR':   get_option('prefix') / 'bin', # FIXME: same
  'NIX_MAN_DIR':   get_option('prefix') / 'share' / 'man', # FIXME: same
}

cpp_args = []

foreach name, value : cpp_str_defines
  cpp_args += [
    '-D' + name + '=' + '"' + value + '"'
  ]
endforeach

library(
  'nixstore',
  libstore_sources,
  schema_sql_gen,
  dependencies : [
    libarchive,
    liblixutil, # Internal.
    sqlite,
    sodium,
    curl,
    openssl,
    aws_sdk,
    aws_s3,
    aws_sdk_transfer,
  ],
  cpp_args : cpp_args,
)
