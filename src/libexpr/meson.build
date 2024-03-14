bison = find_program('bison')
flex = find_program('flex')

parser_tab = custom_target(
  input : 'parser.y',
  output : [
    'parser-tab.cc',
    'parser-tab.hh',
  ],
  command : [
    'bison',
    '-v',
    '-o',
    '@OUTPUT0@',
    '@INPUT@',
    '-d',
  ],
)

lexer_tab = custom_target(
  input : [
    'lexer.l',
    parser_tab,
  ],
  output : [
    'lexer-tab.cc',
    'lexer-tab.hh',
  ],
  command : [
    'flex',
    '--outfile',
    '@OUTPUT0@',
    '--header-file=' + '@OUTPUT1@',
    '@INPUT0@',
  ],
)

libexpr_sources = files(
  'attr-path.cc',
  'attr-set.cc',
  'eval-cache.cc',
  'eval-error.cc',
  'eval-settings.cc',
  'eval.cc',
  'function-trace.cc',
  'get-drvs.cc',
  'json-to-value.cc',
  'nixexpr.cc',
  'paths.cc',
  'primops.cc',
  'print-ambiguous.cc',
  'print.cc',
  'search-path.cc',
  'value-to-json.cc',
  'value-to-xml.cc',
  'flake/config.cc',
  'flake/flake.cc',
  'flake/flakeref.cc',
  'flake/lockfile.cc',
  'primops/context.cc',
  'primops/fetchClosure.cc',
  'primops/fetchMercurial.cc',
  'primops/fetchTree.cc',
  'primops/fromTOML.cc',
  'value/context.cc',
)

all_sources += {
  'libexpr': libexpr_sources
}

libexpr = library(
  'nixexpr',
  libexpr_sources,
  parser_tab,
  lexer_tab,
  dependencies : [
    liblixutil,
    liblixstore,
    liblixfetchers,
    boehm,
    boost,
  ],
)
