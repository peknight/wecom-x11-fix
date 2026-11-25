# Maintainer: Peknight <JKpeknight@gmail.com>

pkgname=wecom-x11-fix
pkgver=1.0.0
pkgrel=21
pkgdesc='X11 workarounds for Wine WeCom: chrome windows that bleed through dialogs and swallow clicks, the lagging shadow border, and the opaque inner shadow band'
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
sha256sums=('83be2288a86c1a135f5507800039782538141a5f5447ff53a476e96f4f43fa09'
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
