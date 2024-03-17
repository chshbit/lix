pkg = import('pkgconfig')

libexprc_sources = files(
  'nix_api_expr.cc',
  'nix_api_external.cc',
  'nix_api_value.cc',
)

all_sources += {
  'libexprc': libexprc_sources
}

libexprc = library(
  'nix-expr-c',
  libexprc_sources,
  dependencies : [
    liblixutil,
    liblixstore,
    liblixexpr,
    liblixutilc,
    liblixstorec,
    boehm,
  ],
)

pkg.generate(libexprc, requires: [ 'nix-store-c' ])

liblixexprc = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libexprc,
)
