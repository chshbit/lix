libfetchers_sources = files(
  'attrs.cc',
  'cache.cc',
  'fetch-settings.cc',
  'fetch-to-store.cc',
  'fetchers.cc',
  'git.cc',
  'github.cc',
  'indirect.cc',
  'mercurial.cc',
  'path.cc',
  'registry.cc',
  'tarball.cc',
)

libfetchers = library(
  'nixfetchers',
  libfetchers_sources,
  dependencies : [
    liblixstore,
    liblixutil,
  ],
)

liblixfetchers = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libfetchers,
)
