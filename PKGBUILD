# Maintainer: Peknight <JKpeknight@gmail.com>

pkgname=wecom-x11-fix
pkgver=1.0.0
pkgrel=1
pkgdesc='X11 workarounds for Deepin-Wine WeCom: click-through title bar and lagging shadow border'
arch=('x86_64')
url='https://aur.archlinux.org/packages/wecom-x11-fix'
license=('MIT')
depends=('libx11' 'libxfixes' 'libxext')
makedepends=('gcc' 'make')
source=(
    "$pkgname.c"
    'Makefile'
    "$pkgname.desktop"
    'LICENSE'
)
sha256sums=('fa305282a0973c75bfb7e6cd81ebd9530a9560aa8ac9d29bd559bc9b486489cc'
            '2d6a9156999e1dc9b03e73be5316251cd37b825febb74730c1e2ceae28741861'
            'cbb1198d2e81e77fee29db96bc1144d9e13912f15988469091f460832441cddd'
            'af1cf152f25f065779033a30ee9d087a65fc10bb0e037ba6b51132084a68622d')

build() {
    make -C "$srcdir"
}

package() {
    make -C "$srcdir" DESTDIR="$pkgdir" install
    install -Dm644 "$srcdir/$pkgname.desktop" \
        "$pkgdir/etc/xdg/autostart/$pkgname.desktop"
    install -Dm644 "$srcdir/LICENSE" \
        "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
