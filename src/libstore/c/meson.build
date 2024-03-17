pkg = import('pkgconfig')

libstorec_sources = files(
  'nix_api_store.cc',
)

all_sources += {
  'libstorec': libstorec_sources,
}

libstorec = library(
  'nix-store-c',
  libstorec_sources,
  dependencies : [
    liblixutil, # Internal.
    liblixstore, # Internal.
    liblixutilc, # Internal.
  ],
)

pkg.generate(libstorec, requires : [ 'nix-util-c' ])

liblixstorec = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libstorec,
)
