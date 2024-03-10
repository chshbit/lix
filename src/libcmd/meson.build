libcmd_sources = files(
  'built-path.cc',
  'command-installable-value.cc',
  'command.cc',
  'common-eval-args.cc',
  'editor-for.cc',
  'installable-attr-path.cc',
  'installable-derived-path.cc',
  'installable-flake.cc',
  'installable-value.cc',
  'installables.cc',
  'legacy.cc',
  'markdown.cc',
  'repl.cc',
  'repl-interacter.cc',
)

libcmd = library(
  'nixcmd',
  libcmd_sources,
  dependencies : [
    liblixutil,
    liblixstore,
    liblixexpr,
    liblixfetchers,
    liblixmain,
    boehm,
    editline,
    lowdown,
  ],
)

liblixcmd = declare_dependency(
  include_directories : '.',
  link_with : libcmd,
)
