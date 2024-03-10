project('lix', 'cpp',
  version : run_command('bash', '-c', 'echo -n $(cat ./.version)$VERSION_SUFFIX', check : true).stdout().strip(),
  default_options : [
    'buildtype=debugoptimized',
    'cpp_std=c++20',
  ],
)

cxx = meson.get_compiler('cpp')

host_system = host_machine.cpu_family() + '-' + host_machine.system()
message('canonical Nix system name:', host_system)

deps = [ ]
configdata = { }
#'AWS_VERSION_MAJOR': aws_sdk_version_major,
#'AWS_VERSION_MINOR': aws_sdk_version_minor,
#'AWS_VERSION_PATCH': aws_sdk_version_patch,
#'CAN_LINK_SYMLINK': can_link_symlink.to_int(),

aws_sdk = dependency('aws-cpp-sdk-core', required : false)
if aws_sdk.found()
  deps += aws_sdk
  s = aws_sdk.version().split('.')
  configdata += {
    'AWS_VERSION_MAJOR': s[0].to_int(),
    'AWS_VERSION_MINOR': s[1].to_int(),
    'AWS_VERSION_PATCH': s[2].to_int(),
  }
endif
aws_s3 = dependency('aws-cpp-sdk-s3', required : false)
configdata += {
  'ENABLE_S3': aws_s3.found().to_int(),
}
if aws_s3.found()
  deps += aws_s3
endif

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

boehm = dependency('bdw-gc', required : get_option('enable-gc'))
if boehm.found()
  deps += boehm
endif
configdata += {
  'HAVE_BOEHMGC': boehm.found().to_int(),
}

boost = dependency('boost', required : true)
deps += boost

cpuid = dependency('libcpuid', required : get_option('enable-cpuid'))
configdata += {
  'HAVE_LIBCPUID': cpuid.found().to_int(),
}
deps += cpuid

seccomp = dependency('libseccomp', required : get_option('enable-seccomp-sandboxing'))
configdata += {
  'HAVE_SECCOMP': seccomp.found().to_int(),
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

libutil_srcs = [
  'src/libutil/archive.cc',
  'src/libutil/args.cc',
  'src/libutil/canon-path.cc',
  'src/libutil/cgroup.cc',
  'src/libutil/compression.cc',
  'src/libutil/compute-levels.cc',
  'src/libutil/config.cc',
  'src/libutil/english.cc',
  'src/libutil/error.cc',
  'src/libutil/experimental-features.cc',
  'src/libutil/filesystem.cc',
  'src/libutil/git.cc',
  'src/libutil/hash.cc',
  'src/libutil/hilite.cc',
  'src/libutil/json-utils.cc',
  'src/libutil/logging.cc',
  'src/libutil/namespaces.cc',
  'src/libutil/position.cc',
  'src/libutil/references.cc',
  'src/libutil/serialise.cc',
  'src/libutil/source-path.cc',
  'src/libutil/suggestions.cc',
  'src/libutil/tarfile.cc',
  'src/libutil/thread-pool.cc',
  'src/libutil/url.cc',
  'src/libutil/util.cc',
  'src/libutil/xml-writer.cc',
]

library('nixutil', libutil_srcs,
  implicit_include_directories : true,
)
