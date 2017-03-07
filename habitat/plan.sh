pkg_name=reconnoiter
pkg_origin=smartb
pkg_version="master"
pkg_maintainer="smartB Engineering <dev@smartb.eu>"
pkg_license=('All Rights')
pkg_source="https://github.com/circonus-labs/${pkg_name}"
pkg_lib_dirs=(lib)
pkg_include_dirs=(include)
pkg_bin_dirs=(bin)
pkg_build_deps=(
  bixu/LuaJIT
  core/apr
  core/autoconf
  core/gcc
  core/jdk8
  core/libxml2
  core/libxslt
  core/lz4
  core/make
  core/man-pages
  core/mysql
  core/ncurses
  core/openssh
  core/openssl
  core/pcre
  core/postgresql
  core/protobuf
  core/protobuf-c
  core/python
  core/util-linux
  core/zlib
  paytmlabs/hostname
  smartb/ck
  smartb/hwloc
  smartb/jlog
  smartb/libcircllhist
  smartb/libcircmetrics
  smartb/libmtev/master/20170306140105
  smartb/udns
  smartb/yajl
)

do_verify() {
  return 0
}

do_download() {
  return 0
}

do_clean() {
  return 0
}

do_unpack() {
  return 0
}

do_build() {
  cd /src
  autoconf
  # FIXME: it seems unlikely that we need so many invocations of -Wl,rpath - can this be improved?
  # Habitat should be providing these for us...
  export LDFLAGS="$LDFLAGS -ldl -lm \
  -Wl,-rpath=$(hab pkg path core/libxml2)/lib \
  -Wl,-rpath=$(hab pkg path core/libxslt)/lib \
  -Wl,-rpath=$(hab pkg path core/lz4)/lib \
  -Wl,-rpath=$(hab pkg path core/openssl)/lib \
  -Wl,-rpath=$(hab pkg path core/pcre)/lib \
  -Wl,-rpath=$(hab pkg path core/zlib)/lib \
  -Wl,-rpath=$(hab pkg path smartb/ck)/lib \
  -Wl,-rpath=$(hab pkg path smartb/hwloc)/lib \
  -Wl,-rpath=$(hab pkg path smartb/jlog)/lib \
  -Wl,-rpath=$(hab pkg path smartb/libcircllhist)/lib \
  -Wl,-rpath=$(hab pkg path core/glibc)/lib \
  -Wl,-rpath=$(hab pkg path core/protobuf)/lib \
  -Wl,-rpath=$(hab pkg path smartb/libcircmetrics)/lib"
  ./configure --prefix=$pkg_prefix
  ln -s $(hab pkg path core/perl)/bin/perl /usr/bin/perl
  #sed -i "s@#!/usr/bin/perl@#!$(hab pkg path core/perl)/bin/perl@" /src/buildtools/xml2h
  make
  return $?
}

do_install() {
  return 0
}
