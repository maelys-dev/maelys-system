#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
version=$(sed -n '1p' VERSION)
case "$version" in
    *[!0-9.]*|.*|*..*|*.) printf '%s\n' "invalid VERSION" >&2; exit 1 ;;
esac
host_os=$(uname -s)
host_arch=$(uname -m)
case "$host_os:$host_arch" in
    Linux:x86_64) target=linux-x86_64; deb_arch=amd64; rpm_arch=x86_64 ;;
    Linux:aarch64|Linux:arm64) target=linux-arm64; deb_arch=arm64; rpm_arch=aarch64 ;;
    Darwin:arm64) target=macos-arm64; deb_arch=; rpm_arch= ;;
    *) printf '%s\n' "unsupported native package target" >&2; exit 1 ;;
esac
linux_packages=0
if test "$host_os" = Linux; then linux_packages=1; fi
# maelys-release calls this script with the target name; --linux-packages is
# the historical spelling and both accept only the native target.
if test "$#" -gt 0; then
    case $1 in
        --linux-packages) linux_packages=1 ;;
        linux-x86_64|linux-arm64) linux_packages=1; test "$1" = "$target" || { printf '%s\n' "target $1 does not match this host" >&2; exit 65; } ;;
        macos-arm64) test "$1" = "$target" || { printf '%s\n' "target $1 does not match this host" >&2; exit 65; } ;;
        *) exit 2 ;;
    esac
fi

temp_base=$(printenv TMPDIR || printf '%s' /tmp)
work=$(mktemp -d "$temp_base/maelys-system-package.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p dist
stage="$work/stage"
make clean check WERROR=
make install DESTDIR="$stage" PREFIX=/usr/local
tar_name="maelys-system-$version-$target.tar.gz"
tar -czf "dist/$tar_name" -C "$stage" .

if command -v sha256sum >/dev/null 2>&1; then
    (cd dist && sha256sum "$tar_name" >"$tar_name.sha256")
else
    (cd dist && shasum -a 256 "$tar_name" >"$tar_name.sha256")
fi
if test "$linux_packages" -eq 0; then exit 0; fi
test "$host_os" = Linux
command -v dpkg-deb >/dev/null
command -v rpmbuild >/dev/null

linux_stage="$work/linux-stage"
make install DESTDIR="$linux_stage" PREFIX=/usr
deb_root="$work/deb"
cp -a "$linux_stage" "$deb_root"
mkdir -p "$deb_root/DEBIAN"
installed_size=$(du -sk "$deb_root/usr" | awk '{print $1}')
cat >"$deb_root/DEBIAN/control" <<EOF
Package: libmaelys-sys-dev
Version: $version
Section: libdevel
Priority: optional
Architecture: $deb_arch
Installed-Size: $installed_size
Maintainer: Maelys Developers <noreply@maelys.dev>
Depends: libc6-dev
Description: minimal callback-free POSIX systems foundation for C
 Static library, public headers and pkg-config metadata.
EOF
deb_name="libmaelys-sys-dev_${version}_${deb_arch}.deb"
dpkg-deb --root-owner-group --build "$deb_root" "dist/$deb_name" >/dev/null
dpkg-deb --info "dist/$deb_name" >/dev/null
dpkg-deb --contents "dist/$deb_name" | grep -Fq './usr/lib/libmaelys_sys.a'

rpm_top="$work/rpmbuild"
mkdir -p "$rpm_top/BUILD" "$rpm_top/BUILDROOT" "$rpm_top/RPMS" \
    "$rpm_top/SOURCES" "$rpm_top/SPECS" "$rpm_top/SRPMS"
cat >"$rpm_top/SPECS/maelys-system.spec" <<EOF
Name:           maelys-system-devel
Version:        $version
Release:        1
Summary:        Minimal callback-free POSIX systems foundation for C
License:        MPL-2.0
URL:            https://github.com/maelys-dev/maelys-system
BuildArch:      $rpm_arch

%description
Static library, public headers and pkg-config metadata.

%prep
%build
%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a $linux_stage/. %{buildroot}/

%files
/usr/include/maelys/sys.h
/usr/include/maelys/sys/
/usr/lib/libmaelys_sys.a
/usr/lib/pkgconfig/maelys-sys.pc
EOF
rpmbuild --define "_topdir $rpm_top" -bb \
    "$rpm_top/SPECS/maelys-system.spec" >/dev/null
rpm_source=$(find "$rpm_top/RPMS" -type f -name '*.rpm' -print -quit)
test -n "$rpm_source"
cp "$rpm_source" dist/
rpm -qpl "dist/$(basename "$rpm_source")" | grep -Fq '/usr/lib/libmaelys_sys.a'

if command -v sha256sum >/dev/null 2>&1; then
    (cd dist && sha256sum "$deb_name" "$(basename "$rpm_source")" > \
        "maelys-system-$version-$target-packages.sha256")
fi
