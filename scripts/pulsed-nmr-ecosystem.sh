ecosystem=ecosystem-0.95-1-6deb253

rm -rf ${ecosystem}-pulsed-nmr

test -f ${ecosystem}.zip || curl -O http://downloads.redpitaya.com/downloads/${ecosystem}.zip

unzip -d ${ecosystem}-pulsed-nmr ${ecosystem}.zip

arm-linux-gnueabihf-gcc -static -O3 -march=armv7-a -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard projects/pulsed_nmr/server/pulsed-nmr.c -lm -o ${ecosystem}-pulsed-nmr/bin/pulsed-nmr
cp tmp/pulsed_nmr.bit ${ecosystem}-pulsed-nmr

rm -f ${ecosystem}-pulsed-nmr/u-boot.scr
cp ${ecosystem}-pulsed-nmr/u-boot.scr.buildroot ${ecosystem}-pulsed-nmr/u-boot.scr

cat <<- EOF_CAT >> ${ecosystem}-pulsed-nmr/sbin/discovery.sh

# start pulsed NMR server

devcfg=/sys/devices/soc0/amba/f8007000.devcfg
test -d \$devcfg/fclk/fclk0 || echo fclk0 > \$devcfg/fclk_export
echo 1 > \$devcfg/fclk/fclk0/enable
echo 143000000 > \$devcfg/fclk/fclk0/set_rate

cat /opt/redpitaya/pulsed_nmr.bit > /dev/xdevcfg

/opt/redpitaya/bin/pulsed-nmr &

EOF_CAT

cd ${ecosystem}-pulsed-nmr
zip -r ../${ecosystem}-pulsed-nmr.zip .
cd ..
