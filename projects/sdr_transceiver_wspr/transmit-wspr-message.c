#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <libconfig.h>

#include "wsprsim_utils.h"

int printdata = 0;

int main(int argc, char *argv[])
{
  int fd, i;
  volatile void *cfg, *mux;
  volatile uint8_t *rst;
  volatile uint32_t *fifo;
  unsigned char symbols[162];
  char *message, *hashtab;
  config_t config;
  double freq, corr, dphi;
  int chan;

  hashtab = malloc(sizeof(char) * 32768 * 13);
  memset(hashtab, 0, sizeof(char) * 32768 * 13);

  if(argc != 2)
  {
    fprintf(stderr, "Usage: transmit-wspr-message config_file.cfg\n");
    return EXIT_FAILURE;
  }

  config_init(&config);

  if(!config_read_file(&config, argv[1]))
  {
    fprintf(stderr, "Error on line %d in configuration file.\n", config_error_line(&config));
    return EXIT_FAILURE;
  }

  if(!config_lookup_float(&config, "freq", &freq))
  {
    fprintf(stderr, "No 'freq' setting in configuration file.\n");
    return EXIT_FAILURE;
  }

  if(freq < 0.1 || freq > 60.0)
  {
    fprintf(stderr, "Wrong 'freq' setting in configuration file.\n");
    return EXIT_FAILURE;
  }

  if(!config_lookup_float(&config, "corr", &corr))
  {
    fprintf(stderr, "No 'corr' setting in configuration file.\n");
    return EXIT_FAILURE;
  }

  if(corr < -100.0 || corr > 100.0)
  {
    fprintf(stderr, "Wrong 'corr' setting in configuration file.\n");
    return EXIT_FAILURE;
  }

  if(!config_lookup_int(&config, "chan", &chan))
  {
    fprintf(stderr, "No 'chan' setting in configuration file.\n", i);
    return EXIT_FAILURE;
  }

  if(chan < 1 || chan > 2)
  {
    fprintf(stderr, "Wrong 'chan' setting in configuration file.\n", i);
    return EXIT_FAILURE;
  }

  if(!config_lookup_string(&config, "message", &message))
  {
    fprintf(stderr, "No 'message' setting in configuration file.\n");
    return EXIT_FAILURE;
  }

  if(strlen(message) > 22)
  {
    fprintf(stderr, "Wrong 'message' setting in configuration file.\n");
    return EXIT_FAILURE;
  }

  if((fd = open("/dev/mem", O_RDWR)) < 0)
  {
    perror("open");
    return EXIT_FAILURE;
  }

  cfg = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40001000);
  fifo = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x4000B000);
  mux = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x4000C000);

  if(chan == 1)
  {
    *(uint32_t *)(mux + 64) = 0;
    *(uint32_t *)(mux + 68) = 1;
  }
  else
  {
    *(uint32_t *)(mux + 64) = 1;
    *(uint32_t *)(mux + 68) = 0;
  }

  *(uint32_t *)mux = 2;

  rst = (uint8_t *)(cfg + 1);

  *rst &= ~1;
  *rst |= 1;

  get_wspr_channel_symbols(message, hashtab, symbols);

  for(i = 0; i < 162; ++i)
  {
    dphi = (freq * 1.0e6 + ((double)symbols[i] - 1.5) * 375.0 / 256.0) / 125.0e6;
    *fifo = (uint32_t)floor((1.0 + 1.0e-6 * corr) * dphi * (1<<30) + 0.5);
  }

  return EXIT_SUCCESS;
}
