#!/bin/bash
# Apply URFAVF3MBOY/quest2-steamos kernel/linux72 onto a clean overpressure/linux tree, exactly as its build-linux72.yml does.
# usage: apply_upstream_layer.sh <linux tree> <upstream repo checkout>
set -e
L=$1; U=$2; K=$U/kernel/linux72; DTB=sm8250-oculus-hollywood; Q=arch/arm64/boot/dts/qcom
cp $K/$DTB.dts $K/$DTB-dtbo-sink.dtsi $L/$Q/
grep -q "$DTB.dtb" $L/$Q/Makefile || echo "dtb-\$(CONFIG_ARCH_QCOM) += $DTB.dtb" >> $L/$Q/Makefile
cp $K/hollywood.config $L/arch/arm64/configs/hollywood.config
for p in $K/patches/*.patch; do git -C $L apply --recount "$p" && echo "applied $(basename $p)"; done
P=$L/drivers/gpu/drm/panel
cp $K/panel-oculus-hollywood-jdi.c $P/
grep -q OCULUS_HOLLYWOOD_JDI $P/Kconfig || cat $K/Kconfig.panel >> $P/Kconfig
grep -q oculus-hollywood-jdi $P/Makefile || echo 'obj-$(CONFIG_DRM_PANEL_OCULUS_HOLLYWOOD_JDI) += panel-oculus-hollywood-jdi.o' >> $P/Makefile
rm -rf $L/drivers/misc/oculus; cp -r $K/drivers/oculus $L/drivers/misc/
grep -q 'oculus/' $L/drivers/misc/Makefile || echo 'obj-y += oculus/' >> $L/drivers/misc/Makefile
mkdir -p $L/drivers/misc/q2android && cp -r $K/drivers/android/. $L/drivers/misc/q2android/
grep -q 'q2android/' $L/drivers/misc/Makefile || echo 'obj-y += q2android/' >> $L/drivers/misc/Makefile
rm -rf $L/techpack/camera; mkdir -p $L/techpack && cp -r $K/camera $L/techpack/camera
grep -q 'techpack/camera/' $L/Kbuild || echo 'obj-y += techpack/camera/' >> $L/Kbuild
sed -i 's/"pico4,compatible"/"quest2,compatible"/; s/"pico4,model"/"quest2,model"/' $L/drivers/of/fdt.c
F=$L/drivers/gpu/drm/drm_fb_helper.c
sed -i 's/^	drm_fb_helper_restore_fbdev_mode_unlocked(dev->fb_helper);$/	drm_fb_helper_restore_fbdev_mode_unlocked(dev->fb_helper, false);/' $F
echo layer applied
