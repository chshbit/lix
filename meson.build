project('lix', 'cpp',
  version : run_command('bash', '-c', 'echo -n $(cat ./.version)$VERSION_SUFFIX', check : true).stdout().strip(),
  default_options : [
    'buildtype=debugoptimized',
    'cpp_std=c++20',
    'warning_level=1',
  ],
)

cxx = meson.get_compiler('cpp')

host_system = host_machine.cpu_family() + '-' + host_machine.system()
message('canonical Nix system name:', host_system)

all_sources = { }
all_deps = { }

deps = [ ]
configdata = { }

#
# Dependencies
#

boehm = dependency('bdw-gc', required : get_option('gc'))
if boehm.found()
  deps += boehm
endif
configdata += {
  'HAVE_BOEHMGC': boehm.found().to_int(),
}

boost = dependency('boost', required : true, modules : ['context', 'coroutine', 'container'])
deps += boost

cpuid = dependency('libcpuid', 'cpuid', required : get_option('cpuid'))
configdata += {
  'HAVE_LIBCPUID': cpuid.found().to_int(),
  }
deps += cpuid

seccomp = dependency('libseccomp', 'seccomp', required : get_option('seccomp-sandboxing'))
configdata += {
  'HAVE_SECCOMP': seccomp.found().to_int(),
  }

libarchive = dependency('libarchive', required : true)
deps += libarchive

brotli = [
  dependency('libbrotlicommon', required : true),
  dependency('libbrotlidec', required : true),
  dependency('libbrotlienc', required : true),
  ]
deps += brotli

openssl = dependency('libcrypto', 'openssl', required : true)
deps += openssl

aws_sdk = dependency('aws-cpp-sdk-core', required : true)
if aws_sdk.found()
  # The AWS pkg-config adds -std=c++11.
  aws_sdk = aws_sdk.partial_dependency(
    compile_args : false,
    includes : true,
    link_args : true,
    links : true,
    sources : true,
  )
  deps += aws_sdk
  s = aws_sdk.version().split('.')
  configdata += {
    'AWS_VERSION_MAJOR': s[0].to_int(),
    'AWS_VERSION_MINOR': s[1].to_int(),
    'AWS_VERSION_PATCH': s[2].to_int(),
  }
  aws_sdk_transfer = dependency('aws-cpp-sdk-transfer', required : true).partial_dependency(
    compile_args : false,
    includes : true,
    link_args : true,
    links : true,
    sources : true,
  )
endif
aws_s3 = dependency('aws-cpp-sdk-s3', required : true)
if aws_s3.found()
  # The AWS pkg-config adds -std=c++11.
  aws_s3 = aws_s3.partial_dependency(
    compile_args : false,
    includes : true,
    link_args : true,
    links : true,
    sources : true,
  )
  deps += aws_s3
endif

sqlite = dependency('sqlite3', 'sqlite', version : '>=3.6.19', required : true)
deps += sqlite

sodium = dependency('sodium', 'libsodium', required : true)
deps += sodium

curl = dependency('libcurl', 'curl', required : true)
deps += curl

editline = dependency('libeditline', 'editline', version : '>=1.14', required : true)
deps += editline

lowdown = dependency('lowdown', version : '>=0.9.0', required : true)
deps += lowdown

#
# Build-time tools
#
bash = find_program('bash')
lsof = find_program('lsof')

# This is how Nix does generated headers...
# FIXME(Qyriad): do we really need to use the shell for this?
gen_header = generator(
  bash,
  arguments : [
    '-c',
    'echo \'R"foo(\' | cat - @INPUT@ && echo \')foo"\'',
  ],
  capture : true,
  output : '@PLAINNAME@.gen.hh',
)

#
# Configuration
#

# FIXME(Qyriad): at least do this in the build directory
run_command('ln', '-s', 'bla', 'tmp_link', check : true)
can_link_symlink = run_command('ln', 'tmp_link', 'tmp_link2', check : false).returncode() == 0
run_command('rm', '-f', 'tmp_link', 'tmp_link2', check : true)
message('possible to create a link to a symlink:', can_link_symlink)
configdata += { 'CAN_LINK_SYMLINK': can_link_symlink.to_int() }

check_headers = [
  # I can't use dictionaries here as they can only contain identifier-strings.
  ['aws/s3/S3Client.h', aws_s3]
]

foreach headerspec : check_headers
  key = headerspec[0]
  value = headerspec[1]
  define_name = 'HAVE_' + key.underscorify().to_upper()
  define_value = cxx.check_header(key, dependencies : value).to_int()
  configdata += { define_name: define_value }
endforeach

check_funcs = [
  'lchown',
  'lutimes',
  'pipe2',
  'posix_fallocate',
  'statvfs',
  'strsignal',
  'sysconf',
]

foreach funcspec : check_funcs
  define_name = 'HAVE_' + funcspec.underscorify().to_upper()
  define_value = cxx.has_function(funcspec).to_int()
  configdata += {
    define_name: define_value,
  }
endforeach

has_pubsetbuf = cxx.compiles('''
  #include <iostream>
  using namespace std;
  static char buf[1024];
  decltype(cerr.rdbuf()->pubsetbuf(buf, sizeof(buf))) _;
''')
message('have function "\x1b[1mstd::basic_streambuf::pubsetbuf\x1b[0m" :', has_pubsetbuf)
configdata += {
  'HAVE_PUBSETBUF': has_pubsetbuf.to_int(),
}

configdata += {
  'ENABLE_S3': aws_s3.found().to_int(),
}

configure_file(
  configuration : {
    'PACKAGE_NAME': '"' + meson.project_name() + '"',
    'PACKAGE_VERSION': '"' + meson.project_version() + '"',
    'PACKAGE_TARNAME': '"' + meson.project_name() + '"',
    'PACKAGE_STRING': '"' + meson.project_name() + ' ' + meson.project_version() + '"',
    'HAVE_STRUCT_DIRENT_D_TYPE': 1, # FIXME: actually check this for solaris
    'SYSTEM': '"' + host_system + '"',
  } + configdata,
  output : 'config.h',
)

add_project_arguments(
  # TODO(Qyriad): Yes this is how the autoconf+Make system did it.
  # I would love to remove this.
  '-include', 'config.h',
  # TODO(Qyriad): would love to remove these
  '-Wno-deprecated-declarations',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wno-deprecated-copy',
  language : 'cpp',
)

subdir('src')
