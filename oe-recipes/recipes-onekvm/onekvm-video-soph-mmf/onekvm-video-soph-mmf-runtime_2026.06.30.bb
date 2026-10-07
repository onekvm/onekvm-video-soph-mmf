SUMMARY = "Sophgo SG2002 multimedia runtime for OneKVM"
HOMEPAGE = "https://github.com/sophgo/cvi_mpi"
LICENSE = "CLOSED"
PR = "r3"

FILESEXTRAPATHS:prepend := "${THISDIR}/files/mmf-runtime:"

SRC_URI = " \
    git://github.com/sophgo/cvi_mpi.git;protocol=https;branch=sg200x-dev;name=mpi;destsuffix=cvi-mpi \
    git://github.com/sophgo/SensorSupportList.git;protocol=https;branch=sg200x-dev;name=sensors;destsuffix=sensors \
    git://github.com/sophgo/osdrv.git;protocol=https;branch=sg200x-dev;name=osdrv;destsuffix=osdrv \
    file://0001-vi-use-a-literal-snprintf-format.patch \
    file://0002-gdc-copy-wrap-attributes-before-free.patch \
    file://0003-modules-allow-a-minimal-directory-list.patch \
    file://0004-isp-use-a-literal-fifo-path-format.patch \
    file://0005-isp-tools-use-python3.patch \
    file://0006-libraries-leave-stripping-to-the-packager.patch \
    file://0007-sys-accept-oe-link-flags.patch \
"
SRCREV_mpi = "75c181ee6e25baca9729a4a9b415f36180b54f93"
SRCREV_sensors = "f064b02ba8a82746f3e87a2c5bb3bd683ff95db0"
SRCREV_osdrv = "aa542c41df94f7bc656cb740f6622a5dca7dc403"
SRCREV_FORMAT = "mpi_sensors_osdrv"
BB_GIT_SHALLOW = "1"
BB_GIT_SHALLOW_DEPTH = "1"

S = "${UNPACKDIR}/cvi-mpi"
B = "${WORKDIR}/build"

COMPATIBLE_MACHINE = "^onekvm-nanokvm$"
PACKAGE_ARCH = "${MACHINE_ARCH}"
DEPENDS += "patchelf-native python3-native virtual/kernel xxd-native"

do_configure() {
    # SensorSupportList is a standalone repository in current Sophgo sources;
    # cvi_mpi expects it at component/isp while building the selected sensor.
    install -d "${S}/component/isp" "${B}/vendor-config"
    # Do not copy the fetcher's nested .git directory.  Its read-only pack
    # files make do_configure non-idempotent when BitBake reruns the task.
    cp -R --no-preserve=ownership \
        "${UNPACKDIR}/sensors/common" \
        "${UNPACKDIR}/sensors/sensor" \
        "${UNPACKDIR}/sensors/Makefile" \
        "${UNPACKDIR}/sensors/sensor.mk" \
        "${S}/component/isp/"

    # cvi_mpi only consumes BUILD_PATH/.config.  Keep the minimal target
    # configuration here instead of importing LicheeRV's complete build tree.
    cat >"${B}/vendor-config/.config" <<EOF
CONFIG_CHIP_sg2002=y
CONFIG_CHIP="sg2002"
CONFIG_ARCH="${ARCH}"
CONFIG_CROSS_COMPILE="${TARGET_PREFIX}"
CONFIG_CROSS_COMPILE_KERNEL="${TARGET_PREFIX}"
CONFIG_CROSS_COMPILE_SDK="${TARGET_PREFIX}"
CONFIG_TOOLCHAIN_MUSL_RISCV64=y
CONFIG_SDK_VER="musl_riscv64"
CONFIG_SENSOR_LONTIUM_LT6911=y
EOF
}

do_compile() {
    cd "${S}"
    oe_runmake module \
        BUILD_PATH="${B}/vendor-config" \
        KERNEL_DIR="${STAGING_KERNEL_BUILDDIR}" \
        KERNEL_PATH="${STAGING_KERNEL_DIR}" \
        OSDRV_PATH="${UNPACKDIR}/osdrv" \
        CHIP_ARCH=CV181X CVIARCH=CV181X \
        TARGET_MACHINE=riscv64-unknown-linux-musl \
        ONEKVM_MODULE_DIRS="sys bin vi vo vpss rgn gdc venc vdec isp" \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        SYSROOT="${RECIPE_SYSROOT}" \
        CC="${CC}" CXX="${CXX}" CPP="${CPP}" \
        AR="${AR}" AS="${AS}" LD="${LD}" NM="${NM}" \
        OBJCOPY="${OBJCOPY}" RANLIB="${RANLIB}" STRIP="${STRIP}" \
        OPT_LEVEL="${CFLAGS} ${CPPFLAGS}" \
        ONEKVM_LDFLAGS="${LDFLAGS}" \
        WARNING_LEVEL="-Wall -Wextra"
}

do_install() {
    install -d "${D}${libdir}" "${D}${libdir}/3rd"
    for library in \
        libcvi_bin.so libae.so libaf.so libawb.so \
        libcvi_bin_isp.so libisp_algo.so libisp.so libsys.so \
        libvi.so libvpss.so libvo.so librgn.so libgdc.so \
        libvdec.so libvenc.so; do
        install -m 0755 "${S}/lib/$library" "${D}${libdir}/$library"
    done
    install -m 0755 "${S}/lib/3rd/libini.so" "${D}${libdir}/libini.so"
    ln -s ../libini.so "${D}${libdir}/3rd/libini.so"

    # libsys uses byte-sized compiler atomics but the vendor linker does not
    # record the dependency itself.
    patchelf --add-needed libatomic.so.1 "${D}${libdir}/libsys.so"

    # The backend builds a few vendor sample/common and LT6911 sources.  Stage
    # only those small development inputs, not the 300 MiB cvi_mpi worktree.
    mpi_root="${D}${datadir}/sophgo-cvi-mpi"
    install -d \
        "$mpi_root/component/panel" \
        "$mpi_root/component/isp/sensor/cv182x" \
        "$mpi_root/modules/isp/include" \
        "$mpi_root/sample" \
        "$mpi_root/3rdparty"
    cp -R --no-preserve=ownership "${S}/component/panel/cv181x" \
        "$mpi_root/component/panel/"
    cp -R --no-preserve=ownership "${S}/component/isp/common" \
        "$mpi_root/component/isp/"
    cp -R --no-preserve=ownership \
        "${S}/component/isp/sensor/cv182x/lontium_lt6911" \
        "$mpi_root/component/isp/sensor/cv182x/"
    cp -R --no-preserve=ownership "${S}/include" "$mpi_root/"
    cp -R --no-preserve=ownership "${S}/modules/isp/include/cv181x" \
        "$mpi_root/modules/isp/include/"
    cp -R --no-preserve=ownership "${S}/sample/common" "$mpi_root/sample/"
    install -Dm0644 "${S}/3rdparty/inih/ini.h" \
        "$mpi_root/3rdparty/inih/ini.h"
    ln -s ../../lib "$mpi_root/lib"
}

SYSROOT_DIRS += "${datadir}/sophgo-cvi-mpi"

RDEPENDS:${PN} += "libatomic sophgo-vcodec-firmware"
RPROVIDES:${PN} += "onekvm-device-nanokvm-mmf-runtime nanokvm-mmf-runtime"
RREPLACES:${PN} += "onekvm-device-nanokvm-mmf-runtime nanokvm-mmf-runtime"
RCONFLICTS:${PN} += "onekvm-device-nanokvm-mmf-runtime nanokvm-mmf-runtime"

SOLIBS = ".so"
FILES_SOLIBSDEV = ""
FILES:${PN} = "${libdir}/*.so ${libdir}/3rd"
FILES:${PN}-dev += "${datadir}/sophgo-cvi-mpi"
INSANE_SKIP:${PN} += "dev-so"
