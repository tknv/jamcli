pkgname=jamcli
pkgver=1.0.0
pkgrel=1
pkgdesc='Headless command-line client for Jami'
arch=('x86_64')
url='https://github.com/tknv/jamcli'
license=('GPL-3.0-or-later')

depends=(
    'jami-daemon'
    'ffmpeg'
)

makedepends=(
    'cmake'
    'gcc'
    'pkgconf'
)

source=()

build() {
    cmake \
        -S . \
        -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_SYSCONFDIR=/etc

    cmake --build build
}

package() {
    DESTDIR="$pkgdir" cmake \
        --install build
}
