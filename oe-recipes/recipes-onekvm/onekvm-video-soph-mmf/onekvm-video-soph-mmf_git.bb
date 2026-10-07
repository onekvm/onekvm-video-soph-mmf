SUMMARY = "Sophgo MMF native video backend for OneKVM"
HOMEPAGE = "https://github.com/onekvm/onekvm-video-soph-mmf"
LICENSE = "CLOSED"
PV = "0.1.0+git"
# The migration source pin only adds OE metadata to the old ef7ea2a2d2 tree.
# Keep its installed version; bump PV and remove this when releasing new code.
PKGV = "${PV}0+ef7ea2a2d2"
PR = "r64"
OPKGBUILDCMD = "opkg-build -Z gzip"

ONEKVM_COMPONENT_GIT_URI ??= "git://github.com/onekvm/onekvm-video-soph-mmf.git;protocol=https;branch=main"
ONEKVM_COMPONENT_SRCREV ??= ""
SRC_URI = "${ONEKVM_COMPONENT_GIT_URI}"
SRCREV = "${ONEKVM_COMPONENT_SRCREV}"
BB_GIT_SHALLOW = "1"
BB_GIT_SHALLOW_DEPTH = "1"

inherit cmake

COMPATIBLE_MACHINE = "^onekvm-nanokvm$"
PACKAGE_ARCH = "${MACHINE_ARCH}"
DEPENDS += "onekvm-video-soph-mmf-runtime"

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DONEKVM_USE_VENDOR_TUNE_FLAGS=OFF \
    -DCVI_MPI_ROOT=${RECIPE_SYSROOT}${datadir}/sophgo-cvi-mpi \
"

do_install:append() {
    # Pinned tree installs OUTPUT_NAME nanokvm-mmf. Newer trees install
    # soph-mmf and still need the alias onekvm-core dlopens.
    backend_dir="${D}${libdir}/onekvm/video-backends"
    if [ -e "${backend_dir}/soph-mmf.so" ] && [ ! -e "${backend_dir}/nanokvm-mmf.so" ]; then
        ln -s soph-mmf.so "${backend_dir}/nanokvm-mmf.so"
    fi
}

# Live upgrades can leave the old helper running after its unit disappears.
pkg_postinst:${PN}() {
    if [ -z "$D" ] && command -v systemctl >/dev/null 2>&1; then
        systemctl disable --now onekvm-device-nanokvm.service >/dev/null 2>&1 || true
        rm -f \
            /usr/libexec/onekvm/onekvm-device-nanokvm \
            /usr/lib/systemd/system/onekvm-device-nanokvm.service \
            /usr/lib/systemd/system-preset/98-onekvm-device-nanokvm.preset \
            /usr/share/dbus-1/system-services/org.onekvm.Device.NanoKVM1.service \
            /usr/share/dbus-1/system.d/org.onekvm.Device.NanoKVM1.conf \
            /etc/systemd/system/multi-user.target.wants/onekvm-device-nanokvm.service
        systemctl daemon-reload >/dev/null 2>&1 || true
        systemctl reload dbus.service >/dev/null 2>&1 || true
    fi
}

RDEPENDS:${PN} += " \
    onekvm-machine-nanokvm \
    onekvm-video-soph-mmf-runtime \
    onekvm-video-soph-mmf-modules \
"
RPROVIDES:${PN} += " \
    onekvm-device-nanokvm \
    onekvm-video-backend onekvm-hid-backend \
    onekvm-srtp-tx-offload-backend onekvm-plugins-nanokvm \
"
RREPLACES:${PN} += "onekvm-device-nanokvm onekvm-plugins-nanokvm"
RCONFLICTS:${PN} += "onekvm-device-nanokvm onekvm-plugins-nanokvm"
SOLIBS = ".so"
FILES_SOLIBSDEV = ""
FILES:${PN} += " \
    ${datadir}/onekvm/device-backends \
    ${datadir}/onekvm/system-plugins \
    ${libdir}/onekvm/video-backends/*.so \
"
INSANE_SKIP:${PN} += "dev-so"
