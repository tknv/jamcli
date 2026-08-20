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

source=(
    "jamcli-${pkgver}.tar.gz"
)

sha256sums=('SKIP')

build() {
    cmake \
        -S "$srcdir/jamcli-${pkgver}" \
        -B "$srcdir/jamcli-${pkgver}/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_SYSCONFDIR=/etc

    cmake --build "$srcdir/jamcli-${pkgver}/build" --parallel
}

package() {
    DESTDIR="$pkgdir" cmake \
        --install "$srcdir/jamcli-${pkgver}/build"
}
