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
  core/apr
  core/autoconf
  core/gcc
  core/jdk8
  core/libxml2
  core/libxslt
  core/mysql
  core/ncurses
  core/openssh
  core/openssl
  core/pcre
  core/postgresql
  core/protobuf
  core/python
  core/util-linux
  core/zlib
  smartb/ck
  smartb/hwloc
  smartb/libmtev/master/20170303174901
  smartb/udns
)

do_verify() {
  return 0
}

do_download()
{
  return 0
}

do_clean()
{
    return 0
}

do_unpack()
{
    return 0
}

do_build()
{
  export LDFLAGS="-ldl -lm $LDFLAGS"
  cd /src
  autoconf
  ./configure
  make
  return $?
}

do_install() {
  return 0
}
