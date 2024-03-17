libutil_sources = files(
  'archive.cc',
  'args.cc',
  'canon-path.cc',
  'cgroup.cc',
  'compression.cc',
  'compute-levels.cc',
  'config.cc',
  'english.cc',
  'error.cc',
  'experimental-features.cc',
  'filesystem.cc',
  'git.cc',
  'hash.cc',
  'hilite.cc',
  'json-utils.cc',
  'logging.cc',
  'namespaces.cc',
  'position.cc',
  'references.cc',
  'serialise.cc',
  'source-path.cc',
  'suggestions.cc',
  'tarfile.cc',
  'thread-pool.cc',
  'url.cc',
  'util.cc',
  'xml-writer.cc',
)

all_sources += {
  'libutil': libutil_sources,
}

libutil = library(
  'nixutil',
  libutil_sources,
  dependencies : [
    aws_sdk,
    aws_s3,
    boehm,
    boost,
    cpuid,
    seccomp,
    libarchive,
    brotli,
    openssl,
  ],
  implicit_include_directories : true,
)

# Used by libstore and libfetchers.
liblixutil = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libutil
)

subdir('c')

