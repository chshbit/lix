libmain_sources = files(
  'common-args.cc',
  'loggers.cc',
  'progress-bar.cc',
  'shared.cc',
  'stack.cc',
)

libmain = library(
  'main',
  libmain_sources,
  dependencies : [
    liblixutil,
    liblixstore,
  ],
)
