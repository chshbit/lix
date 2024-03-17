pkg = import('pkgconfig')

libutilc_sources = files(
  'nix_api_util.cc',
)

all_sources += {
  'libutilc': libutilc_sources,
}

libutilc = library(
  'nix-util-c',
  libutilc_sources,
  dependencies : [
    liblixutil,
  ],
  implicit_include_directories : true,
)

pkg.generate(libutilc)

liblixutilc = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libutilc
)
