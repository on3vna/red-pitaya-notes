devcfg=/sys/devices/soc0/amba/f8007000.devcfg
test -d $devcfg/fclk/fclk0 || echo fclk0 > $devcfg/fclk_export
echo 1 > $devcfg/fclk/fclk0/enable
echo 143000000 > $devcfg/fclk/fclk0/set_rate

/opt/redpitaya/www/apps/sdr_receiver_hpsdr/sdr-receiver-hpsdr 1 1 1 1 1 1 &
