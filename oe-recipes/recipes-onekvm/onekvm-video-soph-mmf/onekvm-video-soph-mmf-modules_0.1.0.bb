SUMMARY = "Sophgo SG2002 multimedia kernel modules"
HOMEPAGE = "https://github.com/onekvm/osdrv-sg200x"
LICENSE = "CLOSED"
PR = "r32"

FILESEXTRAPATHS:prepend := "${THISDIR}/files/mmf-modules:"

SRC_URI = " \
    git:///sources/osdrv-sg200x;protocol=file;branch=sg200x-dev \
    file://soph-clock-cooling.conf \
    file://load-onekvm-video-soph-mmf-modules \
    file://load-onekvm-video-soph-mmf-optional-modules \
    file://prepare-onekvm-device-nanokvm-hdmi.c \
    file://onekvm-video-soph-mmf-modules.service \
    file://onekvm-video-soph-mmf-optional-modules.service \
    file://onekvm-video-soph-mmf-optional-modules.timer \
    file://onekvm-video-soph-mmf-modules.conf \
"
SRCREV = "0f4152f3255a4670eacef56db2ba564022c7180c"
BB_GIT_SHALLOW = "1"
BB_GIT_SHALLOW_DEPTH = "1"
B = "${WORKDIR}/build"

inherit module-base systemd onekvm-kernel-module-depmod

COMPATIBLE_MACHINE = "^onekvm-nanokvm$"
PACKAGE_ARCH = "${MACHINE_ARCH}"
DEPENDS += "bc-native virtual/kernel"

# OSDRV launches a separate Kbuild for each module.  Running those module
# targets concurrently makes them race while Kbuild refreshes the shared
# RISC-V VDSO objects.  Serialize targets here; each individual Kbuild still
# uses BitBake's normal parallel make setting internally.
PARALLEL_MAKE = "-j1"

do_configure() {
    install -d "${B}/vendor-config" "${B}/modules"
    cat >"${B}/vendor-config/.config" <<EOF
CONFIG_ARCH="${ARCH}"
CONFIG_CROSS_COMPILE_KERNEL="${TARGET_PREFIX}"
CONFIG_NO_FB=y
CONFIG_NO_TP=y
EOF
}

do_compile() {
    export CVIARCH=CV181X
    export CHIP_ARCH=CV181X
    export LDDINCDIR="${STAGING_KERNEL_DIR}/drivers/staging/android/ion"
    # The vendor build invokes Kbuild directly and therefore does not consume
    # OE's target CFLAGS.  Preserve OE's reproducible debug/source paths for
    # the external modules while retaining the vendor warning policy.
    kernel_source_real=$(readlink -f "${STAGING_KERNEL_DIR}")
    export KCFLAGS="${DEBUG_PREFIX_MAP} -ffile-prefix-map=$kernel_source_real=/usr/src/debug/linux-sophgo/${KERNEL_VERSION} -Wno-error -include ${S}/interdrv/include/cvi_platform_compat.h"

    cd "${S}"
    # NanoKVM HDMI capture uses soph_vi. Skip vendor C906L fast_image/rtos_cmdqu.
    oe_runmake \
        BUILD_PATH="${B}/vendor-config" \
        KERNEL_DIR="${STAGING_KERNEL_BUILDDIR}" \
        INSTALL_DIR="${B}/modules" \
        CONFIG_ARCH="${ARCH}" \
        CONFIG_CROSS_COMPILE_KERNEL="${TARGET_PREFIX}" \
        CFLAGS_MODULE="${ONEKVM_KERNEL_MODULE_CFLAGS} -Wno-error" \
        CONFIG_NO_FB=y CONFIG_NO_TP=y \
        sys base vcodec jpeg cif snsr_i2c vi vpss \
        dwa vo rgn cvi_vc_drv ive tpu clock_cooling

    ${CC} ${CFLAGS} ${CPPFLAGS} \
        -ffile-prefix-map=${UNPACKDIR}=/usr/src/debug/${PN}/${PV} \
        ${LDFLAGS} \
        "${UNPACKDIR}/prepare-onekvm-device-nanokvm-hdmi.c" \
        -o "${B}/prepare-device-nanokvm-hdmi"
}
do_compile[lockfiles] = "${TMPDIR}/onekvm-nanokvm-kernel-module.lock"

do_install() {
    kernel_release=$(cat "${STAGING_KERNEL_BUILDDIR}/include/config/kernel.release")
    module_dir="${D}${nonarch_base_libdir}/modules/$kernel_release/extra/onekvm/nanokvm"
    install -d "$module_dir"
    for module in \
        soph_sys soph_base soph_vcodec soph_jpeg \
        soph_mipi_rx soph_snsr_i2c \
        soph_vi soph_vpss soph_dwa soph_vo soph_rgn soph_vc_driver soph_ive \
        soph_tpu soph_clock_cooling; do
        install -m 0644 "${B}/modules/$module.ko" "$module_dir/$module.ko"
    done

    install -Dm0755 "${UNPACKDIR}/load-onekvm-video-soph-mmf-modules" \
        "${D}${libexecdir}/onekvm/load-video-soph-mmf-modules"
    install -Dm0755 "${UNPACKDIR}/load-onekvm-video-soph-mmf-optional-modules" \
        "${D}${libexecdir}/onekvm/load-video-soph-mmf-optional-modules"
    ln -s load-video-soph-mmf-modules \
        "${D}${libexecdir}/onekvm/load-device-nanokvm-mmf-modules"
    ln -s load-video-soph-mmf-optional-modules \
        "${D}${libexecdir}/onekvm/load-device-nanokvm-mmf-optional-modules"
    install -Dm0755 "${B}/prepare-device-nanokvm-hdmi" \
        "${D}${libexecdir}/onekvm/prepare-device-nanokvm-hdmi"
    install -Dm0644 "${UNPACKDIR}/onekvm-video-soph-mmf-modules.service" \
        "${D}${systemd_system_unitdir}/onekvm-video-soph-mmf-modules.service"
    install -Dm0644 "${UNPACKDIR}/onekvm-video-soph-mmf-optional-modules.service" \
        "${D}${systemd_system_unitdir}/onekvm-video-soph-mmf-optional-modules.service"
    install -Dm0644 "${UNPACKDIR}/onekvm-video-soph-mmf-optional-modules.timer" \
        "${D}${systemd_system_unitdir}/onekvm-video-soph-mmf-optional-modules.timer"
    install -d "${D}${systemd_system_unitdir}/multi-user.target.wants"
    ln -snf ../onekvm-video-soph-mmf-optional-modules.timer \
        "${D}${systemd_system_unitdir}/multi-user.target.wants/onekvm-video-soph-mmf-optional-modules.timer"
    install -Dm0644 "${UNPACKDIR}/onekvm-video-soph-mmf-modules.conf" \
        "${D}${nonarch_libdir}/modprobe.d/onekvm-video-soph-mmf-modules.conf"
    install -Dm0644 "${UNPACKDIR}/soph-clock-cooling.conf" \
        "${D}${nonarch_libdir}/modules-load.d/onekvm-soph-clock-cooling.conf"
}

pkg_postinst:${PN}() {
    rm -f $D${systemd_system_unitdir}/onekvm.service.d/20-onekvm-device-nanokvm-mmf.conf
    rm -f $D${sysconfdir}/systemd/system/onekvm-device-nanokvm-mmf-modules.service
    rm -f $D${sysconfdir}/systemd/system/local-fs.target.wants/onekvm-device-nanokvm-mmf-modules.service
    rm -f $D${sysconfdir}/systemd/system/onekvm.service.wants/onekvm-device-nanokvm-mmf-modules.service
    if [ -z "$D" ]; then
        systemctl daemon-reload >/dev/null 2>&1 || true
    fi
}

SYSTEMD_SERVICE:${PN} = "onekvm-video-soph-mmf-modules.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
FILES:${PN} += " \
    ${nonarch_base_libdir}/modules/*/extra/onekvm/nanokvm \
    ${libexecdir}/onekvm \
    ${systemd_system_unitdir} \
    ${nonarch_libdir}/modprobe.d \
    ${nonarch_libdir}/modules-load.d \
"
RDEPENDS:${PN} += "kmod onekvm-core (>= 0.1.0-r49)"
RPROVIDES:${PN} += "onekvm-device-nanokvm-mmf-modules"
RREPLACES:${PN} += "onekvm-device-nanokvm-mmf-modules"
RCONFLICTS:${PN} += "onekvm-device-nanokvm-mmf-modules"
INHIBIT_PACKAGE_STRIP = "1"

