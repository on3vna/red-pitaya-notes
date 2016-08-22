DATE=`date +%Y%m%d`

source /opt/Xilinx/Vivado/2016.2/settings64.sh
source /opt/Xilinx/SDK/2016.2/settings64.sh

make NAME=led_blinker all

sudo sh scripts/image.sh scripts/debian.sh red-pitaya-debian-8.5-armhf-$DATE.img
zip red-pitaya-debian-8.5-armhf-$DATE.zip red-pitaya-debian-8.5-armhf-$DATE.img

sudo sh scripts/image.sh scripts/debian-ecosystem.sh red-pitaya-ecosystem-0.95-debian-8.5-armhf-$DATE.img 1024
zip red-pitaya-ecosystem-0.95-debian-8.5-armhf-$DATE.zip red-pitaya-ecosystem-0.95-debian-8.5-armhf-$DATE.img

make NAME=sdr_transceiver_emb all

sudo sh scripts/image.sh scripts/debian-gnuradio.sh red-pitaya-gnuradio-debian-8.5-armhf-$DATE.img 1024
zip red-pitaya-gnuradio-debian-8.5-armhf-$DATE.zip red-pitaya-gnuradio-debian-8.5-armhf-$DATE.img

make NAME=sdr_transceiver_wspr all

sudo sh scripts/image.sh scripts/debian-wspr.sh red-pitaya-wspr-debian-8.5-armhf-$DATE.img 1024
zip red-pitaya-wspr-debian-8.5-armhf-$DATE.zip red-pitaya-wspr-debian-8.5-armhf-$DATE.img

make NAME=sdr_transceiver tmp/sdr_transceiver.bit
make NAME=sdr_transceiver_hpsdr tmp/sdr_transceiver_hpsdr.bit
make NAME=sdr_receiver_hpsdr tmp/sdr_receiver_hpsdr.bit
make NAME=sdr_transceiver_wide tmp/sdr_transceiver_wide.bit

source scripts/sdr-transceiver-ecosystem.sh
source scripts/sdr-transceiver-bazaar.sh

source scripts/sdr-transceiver-hpsdr-ecosystem.sh
source scripts/sdr-transceiver-hpsdr-bazaar.sh

source scripts/sdr-receiver-hpsdr-ecosystem.sh
source scripts/sdr-receiver-hpsdr-bazaar.sh

source scripts/sdr-transceiver-wide-ecosystem.sh
source scripts/sdr-transceiver-wide-bazaar.sh

make NAME=mcpha tmp/mcpha.bit

source scripts/mcpha-ecosystem.sh
source scripts/mcpha-bazaar.sh

make NAME=pulsed_nmr tmp/pulsed_nmr.bit

source scripts/pulsed-nmr-ecosystem.sh

make NAME=scanner tmp/scanner.bit

source scripts/scanner-ecosystem.sh

make NAME=vna tmp/vna.bit

source scripts/vna-ecosystem.sh
source scripts/vna-bazaar.sh
