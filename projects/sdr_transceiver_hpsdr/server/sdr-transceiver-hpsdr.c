/* 19.04.2016 DC2PD : add code for bandpass and antenna switching via I2C. */

/***************************************************************************/
/* 21/7/2016  ON3VNA : ALSA sound device becomes unavailable due to buffer */
/*                     underruns.                                          */
/***************************************************************************/

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <syslog.h>
 
#include <errno.h>
#include <poll.h>
#include <alsa/asoundlib.h>
	      
snd_pcm_t *playback_handle;
#define pcmframes 1024
char buf[pcmframes*4];
char buffernull[pcmframes*4];
int sound_avail=0;

#include "jack/ringbuffer.c"

#define I2C_SLAVE       0x0703 /* Use this slave address */
#define I2C_SLAVE_FORCE 0x0706 /* Use this slave address, even if it
                                  is already in use by a driver! */

#define ADDR_PENE 0x20 /* PCA9555 address 0 */
#define ADDR_ALEX 0x21 /* PCA9555 address 1 */

volatile uint32_t *rx_freq[4], *rx_rate, *tx_freq, *alex;
volatile uint16_t *rx_cntr, *tx_cntr;
volatile uint8_t *gpio_in, *gpio_out, *rx_rst, *tx_rst;
volatile uint64_t *rx_data;
volatile uint32_t *tx_data;

const uint32_t freq_min = 0;
const uint32_t freq_max = 61440000;

int receivers = 1;
int sound =0;

int sock_ep2;
struct sockaddr_in addr_ep6;

int enable_thread = 0;
int active_thread = 0;

void process_ep2(uint8_t *frame);
void *handler_ep6(void *arg);
void *handler_playback(void *arg);
int ini_sound();

jack_ringbuffer_t *playback_data = 0;
unsigned long jack_space;	/* ON3VNA : USB audio   */

/* variables to handle PCA9555 board */
int i2c_fd;
int i2c_pene = 0;
int i2c_alex = 0;
uint16_t i2c_pene_data = 0;
uint16_t i2c_alex_data = 0;

ssize_t i2c_write(int fd, uint8_t addr, uint16_t data)
{
  uint8_t buffer[3];
  buffer[0] = addr;
  buffer[1] = data;
  buffer[2] = data >> 8;
  return write(fd, buffer, 3);
}

uint16_t alex_data_rx = 0;
uint16_t alex_data_tx = 0;
uint16_t alex_data_0 = 0;
uint32_t alex_data_1 = 0;
uint32_t alex_data_2 = 0;
uint32_t alex_data_3 = 0;
uint16_t alex_data_4 = 0;

void alex_write()
{
  uint32_t max = alex_data_2 > alex_data_3 ? alex_data_2 : alex_data_3;
  uint16_t manual = (alex_data_4 >> 15) & 0x01;
  uint16_t preamp = manual ? (alex_data_4 >> 6) & 0x01 : max > 50000000;
  uint16_t ptt = alex_data_0 & 0x01;
  uint32_t freq = 0;
  uint16_t hpf = 0, lpf = 0, data = 0;

  freq = alex_data_2 < alex_data_3 ? alex_data_2 : alex_data_3;

  if(preamp) hpf = 0;
  else if(manual) hpf = alex_data_4 & 0x3f;
  else if(freq < 1416000) hpf = 0x20; /* bypass */
  else if(freq < 6500000) hpf = 0x10; /* 1.5 MHz HPF */
  else if(freq < 9500000) hpf = 0x08; /* 6.5 MHz HPF */
  else if(freq < 13000000) hpf = 0x04; /* 9.5 MHz HPF */
  else if(freq < 20000000) hpf = 0x01; /* 13 MHz HPF */
  else hpf = 0x02; /* 20 MHz HPF */

  data =
    ptt << 15 |
    ((alex_data_0 >> 1) & 0x01) << 14 |
    ((alex_data_0 >> 2) & 0x01) << 13 |
    ((hpf >> 5) & 0x01) << 12 |
    ((alex_data_0 >> 7) & 0x01) << 11 |
    (((alex_data_0 >> 5) & 0x03) == 0x01) << 10 |
    (((alex_data_0 >> 5) & 0x03) == 0x02) << 9 |
    (((alex_data_0 >> 5) & 0x03) == 0x03) << 8 |
    ((hpf >> 2) & 0x07) << 4 |
    preamp << 3 |
    (hpf & 0x03) << 1 |
    1;

  if(alex_data_rx != data)
  {
    alex_data_rx = data;
    *alex = 1 << 16 | data;
  }

  freq = ptt ? alex_data_1 : max;

  if(manual) lpf = (alex_data_4 >> 8) & 0x7f;
  else if(freq > 32000000) lpf = 0x10; /* bypass */
  else if(freq > 22000000) lpf = 0x20; /* 12/10 meters */
  else if(freq > 15000000) lpf = 0x40; /* 17/15 meters */
  else if(freq > 8000000) lpf = 0x01; /* 30/20 meters */
  else if(freq > 4500000) lpf = 0x02; /* 60/40 meters */
  else if(freq > 2400000) lpf = 0x04; /* 80 meters */
  else lpf = 0x08; /* 160 meters */

  data =
    ((lpf >> 4) & 0x07) << 13 |
    ptt << 12 |
    (~(alex_data_4 >> 7) & ptt) << 11 |
    (((alex_data_0 >> 8) & 0x03) == 0x02) << 10 |
    (((alex_data_0 >> 8) & 0x03) == 0x01) << 9 |
    (((alex_data_0 >> 8) & 0x03) == 0x00) << 8 |
    (lpf & 0x0f) << 4 |
    1 << 3;

  if(alex_data_tx != data)
  {
    alex_data_tx = data;
    *alex = 1 << 17 | data;
  }
}

int main(int argc, char *argv[])
{
  int fd, i;
  ssize_t size;
  pthread_t thread;
  volatile void *cfg, *sts;
  char *name = "/dev/mem";
  uint8_t buffer[1032];
  uint8_t reply[11] = {0xef, 0xfe, 2, 0, 0, 0, 0, 0, 0, 21, 0};
  struct ifreq hwaddr;
  struct sockaddr_in addr_ep2, addr_from;
  socklen_t size_from;
  int yes = 1;
 
  openlog ("sdr-transceiver-hpsdr-main", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL0);
  syslog (LOG_NOTICE, "Program sdr-transceiver-hpsdr started by User %d", getuid ());
  if((fd = open(name, O_RDWR)) < 0)
  {
    perror("open");
    return EXIT_FAILURE;
  }

  if((i2c_fd = open("/dev/i2c-0", O_RDWR)) >= 0)
  {
    if(ioctl(i2c_fd, I2C_SLAVE_FORCE, ADDR_PENE) >= 0)
    {
      /* set all pins to low */
      if(i2c_write(i2c_fd, 0x02, 0x0000) > 0)
      {
        i2c_pene = 1;
        /* configure all pins as output */
        i2c_write(i2c_fd, 0x06, 0x0000);
      }
    }
    if(ioctl(i2c_fd, I2C_SLAVE, ADDR_ALEX) >= 0)
    {
      /* set all pins to low */
      if(i2c_write(i2c_fd, 0x02, 0x0000) > 0)
      {
        i2c_alex = 1;
        /* configure all pins as output */
        i2c_write(i2c_fd, 0x06, 0x0000);
      }
    }
  }

  sts = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40000000);
  cfg = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40001000);
  alex = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40002000);
  rx_data = mmap(NULL, 8*sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40008000);
  tx_data = mmap(NULL, 16*sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40010000);

  rx_rst = ((uint8_t *)(cfg + 0));
  tx_rst = ((uint8_t *)(cfg + 1));
  gpio_out = ((uint8_t *)(cfg + 2));

  rx_rate = ((uint32_t *)(cfg + 4));

  rx_freq[0] = ((uint32_t *)(cfg + 8));
  rx_freq[1] = ((uint32_t *)(cfg + 12));
  rx_freq[2] = ((uint32_t *)(cfg + 16));
  rx_freq[3] = ((uint32_t *)(cfg + 20));

  tx_freq = ((uint32_t *)(cfg + 24));

  rx_cntr = ((uint16_t *)(sts + 12));
  tx_cntr = ((uint16_t *)(sts + 14));
  gpio_in = ((uint8_t *)(sts + 16));

  /* set all GPIO pins to low */
  *gpio_out = 0;

  /* set default rx phase increment */
  *rx_freq[0] = (uint32_t)floor(600000 / 125.0e6 * (1 << 30) + 0.5);
  *rx_freq[1] = (uint32_t)floor(600000 / 125.0e6 * (1 << 30) + 0.5);
  *rx_freq[2] = (uint32_t)floor(600000 / 125.0e6 * (1 << 30) + 0.5);
  *rx_freq[3] = (uint32_t)floor(600000 / 125.0e6 * (1 << 30) + 0.5);

  /* set default rx sample rate */
  *rx_rate = 1000;

  /* set default tx phase increment */
  *tx_freq = (uint32_t)floor(600000 / 125.0e6 * (1 << 30) + 0.5);

  /* reset tx fifo */
  *tx_rst |= 1;
  *tx_rst &= ~1;

  if((sock_ep2 = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
  {
    perror("socket");
    return EXIT_FAILURE;
  }

  strncpy(hwaddr.ifr_name, "eth0", IFNAMSIZ);
  ioctl(sock_ep2, SIOCGIFHWADDR, &hwaddr);
  for(i = 0; i < 6; ++i) reply[i + 3] = hwaddr.ifr_addr.sa_data[i];

  setsockopt(sock_ep2, SOL_SOCKET, SO_REUSEADDR, (void *)&yes , sizeof(yes));

  memset(&addr_ep2, 0, sizeof(addr_ep2));
  addr_ep2.sin_family = AF_INET;
  addr_ep2.sin_addr.s_addr = htonl(INADDR_ANY);
  addr_ep2.sin_port = htons(1024);

  if(bind(sock_ep2, (struct sockaddr *)&addr_ep2, sizeof(addr_ep2)) < 0)
  {
    perror("bind");
    return EXIT_FAILURE;
  }
  /*************************************************************************/
  /* ON3VNA :                                                              */
  /* We can recieve upto 1800UDP packets per second from the HPSDR program */
  /* running on the PC.                                                    */
  /*************************************************************************/
  playback_data = jack_ringbuffer_create(16000000L); 
  
  if(pthread_create(&thread, NULL, handler_playback, NULL) < 0)
  {
    perror("pthread_create");
    return EXIT_FAILURE;
  }
  pthread_detach(thread);

  while(1)
  {
    size_from = sizeof(addr_from);
    size = recvfrom(sock_ep2, buffer, 1032, 0, (struct sockaddr *)&addr_from, &size_from);
    if(size < 0)
    {
      perror("recvfrom");
      return EXIT_FAILURE;
    }

    switch(*(uint32_t *)buffer)
    {

	  case 0x0201feef:
		if (sound_avail)
		{
			/* BEGIN ON3VNA : Check if space in the jack buffer else drop this frames */
			/*                otherwhise we may overwrite the read ptr                */
			jack_space=jack_ringbuffer_write_space(playback_data);
			if (jack_space>2016L)
			{
				for(i = 0; i < 504; i += 8)
				{
					jack_ringbuffer_write(playback_data, buffer + 16 + i, 4);	
				}					
			}
			else
			{
				syslog (LOG_ERR, "not enough space to copy frames in jackbuffer \n");
			}
			jack_space=jack_ringbuffer_write_space(playback_data);
			if (jack_space>2016L)
			{
				for(i = 0; i < 504; i += 8) 
				{
					jack_ringbuffer_write(playback_data, buffer + 528 + i, 4);
				}
			}
			else
			{
				syslog (LOG_ERR, "not enough space to copy frames in jackbuffer \n");				
			}
			/* END ON3VNA : Check if space in the jack buffer else drop this frames */  
		}
		while(*tx_cntr > 16258) usleep(1000);
        if(*tx_cntr == 0) for(i = 0; i < 16258; ++i) *tx_data = 0;
      
		if((*gpio_out & 1) | (*gpio_in & 1))
        {
          for(i = 0; i < 504; i += 8) *tx_data = *(uint32_t *)(buffer + 20 + i);
          for(i = 0; i < 504; i += 8) *tx_data = *(uint32_t *)(buffer + 532 + i);
        }
        else
        {
          for(i = 0; i < 126; ++i) *tx_data = 0;
        }

        process_ep2(buffer + 11);
        process_ep2(buffer + 523);
        break;
      case 0x0002feef:
        reply[2] = 2 + active_thread;
        memset(buffer, 0, 60);
        memcpy(buffer, reply, 11);
        sendto(sock_ep2, buffer, 60, 0, (struct sockaddr *)&addr_from, size_from);
        break;
      case 0x0004feef:
        enable_thread = 0;
        while(active_thread) usleep(1000);
        break;
      case 0x0104feef:
      case 0x0204feef:
      case 0x0304feef:
        enable_thread = 0;
        while(active_thread) usleep(1000);
        memset(&addr_ep6, 0, sizeof(addr_ep6));
        addr_ep6.sin_family = AF_INET;
        addr_ep6.sin_addr.s_addr = addr_from.sin_addr.s_addr;
        addr_ep6.sin_port = addr_from.sin_port;
        enable_thread = 1;
        active_thread = 1;
        if(pthread_create(&thread, NULL, handler_ep6, NULL) < 0)
        {
          perror("pthread_create");
          return EXIT_FAILURE;
        }
        pthread_detach(thread);
        break;
    }
  }

  close(sock_ep2);
  closelog ();
  
  return EXIT_SUCCESS;
}

void process_ep2(uint8_t *frame)
{
  uint32_t freq;
  uint16_t data;

  switch(frame[0])
  {
    case 0:
    case 1:
      receivers = ((frame[4] >> 3) & 7) + 1;
      /* set PTT pin */
      if(frame[0] & 1) *gpio_out |= 1;
      else *gpio_out &= ~1;
      /* set preamp pin */
      if(frame[3] & 4) *gpio_out |= 2;
      else *gpio_out &= ~2;

      /* set rx sample rate */
      switch(frame[1] & 3)
      {
        case 0:
          *rx_rate = 1000;
          break;
        case 1:
          *rx_rate = 500;
          break;
        case 2:
          *rx_rate = 250;
          break;
        case 3:
          *rx_rate = 125;
          break;
      }

      data = (frame[4] & 0x03) << 8 | (frame[3] & 0xe0) | (frame[3] & 0x03) << 1 | (frame[0] & 0x01);
      if(alex_data_0 != data)
      {
        alex_data_0 = data;
        alex_write();
      }

      /* configure PENELOPE */
      if(i2c_pene)
      {
        data = (frame[4] & 0x03) << 11 | (frame[3] & 0x60) << 4 | (frame[3] & 0x03) << 7 | frame[2] >> 1;
        if(i2c_pene_data != data)
        {
          i2c_pene_data = data;
          ioctl(i2c_fd, I2C_SLAVE, ADDR_PENE);
          i2c_write(i2c_fd, 0x02, data);
        }
      }
      break;
    case 2:
    case 3:
      /* set tx phase increment */
      freq = ntohl(*(uint32_t *)(frame + 1));
      if(alex_data_1 != freq)
      {
        alex_data_1 = freq;
        alex_write();
      }
      if(freq < freq_min || freq > freq_max) break;
      *tx_freq = (uint32_t)floor(freq / 125.0e6 * (1 << 30) + 0.5);
      break;
    case 4:
    case 5:
      /* set rx phase increment */
      freq = ntohl(*(uint32_t *)(frame + 1));
      if(alex_data_2 != freq)
      {
        alex_data_2 = freq;
        alex_write();
      }
      if(freq < freq_min || freq > freq_max) break;
      *rx_freq[0] = (uint32_t)floor(freq / 125.0e6 * (1 << 30) + 0.5);
      break;
    case 6:
    case 7:
      /* set rx phase increment */
      freq = ntohl(*(uint32_t *)(frame + 1));
      if(alex_data_3 != freq)
      {
        alex_data_3 = freq;
        alex_write();
      }
      if(freq < freq_min || freq > freq_max) break;
      *rx_freq[1] = (uint32_t)floor(freq / 125.0e6 * (1 << 30) + 0.5);
      break;
    case 8:
    case 9:
      /* set rx phase increment */
      freq = ntohl(*(uint32_t *)(frame + 1));
      if(freq < freq_min || freq > freq_max) break;
      *rx_freq[2] = (uint32_t)floor(freq / 125.0e6 * (1 << 30) + 0.5);
      break;
    case 10:
    case 11:
      /* set rx phase increment */
      freq = ntohl(*(uint32_t *)(frame + 1));
      if(freq < freq_min || freq > freq_max) break;
      *rx_freq[3] = (uint32_t)floor(freq / 125.0e6 * (1 << 30) + 0.5);
      break;
    case 18:
    case 19:
      data = (frame[2] & 0x40) << 9 | frame[4] << 8 | frame[3];
      if(alex_data_4 != data)
      {
        alex_data_4 = data;
        alex_write();
      }

      /* configure ALEX */
      if(i2c_alex)
      {
        data = frame[4] << 8 | frame[3];
        if(i2c_alex_data != data)
        {
          i2c_alex_data = data;
          ioctl(i2c_fd, I2C_SLAVE, ADDR_ALEX);
          i2c_write(i2c_fd, 0x02, data);
        }
      }
      break;
  }
}

void *handler_ep6(void *arg)
{
  int i, j, n, m, size;
  int data_offset, header_offset, buffer_offset;
  uint32_t counter;
  uint8_t data0[4096];
  uint8_t data1[4096];
  uint8_t data2[4096];
  uint8_t data3[4096];
  uint8_t buffer[25][1032];
  struct iovec iovec[25][1];
  struct mmsghdr datagram[25];
  uint8_t header[40] =
  {
    127, 127, 127, 0, 0, 33, 17, 21,
    127, 127, 127, 8, 0, 0, 0, 0,
    127, 127, 127, 16, 0, 0, 0, 0,
    127, 127, 127, 24, 0, 0, 0, 0,
    127, 127, 127, 32, 66, 66, 66, 66
  };

  memset(iovec, 0, sizeof(iovec));
  memset(datagram, 0, sizeof(datagram));

  for(i = 0; i < 25; ++i)
  {
    *(uint32_t *)(buffer[i] + 0) = 0x0601feef;
    iovec[i][0].iov_base = buffer[i];
    iovec[i][0].iov_len = 1032;
    datagram[i].msg_hdr.msg_iov = iovec[i];
    datagram[i].msg_hdr.msg_iovlen = 1;
    datagram[i].msg_hdr.msg_name = &addr_ep6;
    datagram[i].msg_hdr.msg_namelen = sizeof(addr_ep6);
  }

  header_offset = 0;
  counter = 0;

  /* reset rx fifo */
  *rx_rst |= 1;
  *rx_rst &= ~1;

  while(1)
  {
    if(!enable_thread) break;

    size = receivers * 6 + 2;
    n = 504 / size;
    m = 256 / n;

    if(*rx_cntr >= 8192)
    {
      /* reset rx fifo */
      *rx_rst |= 1;
      *rx_rst &= ~1;
    }

    while(*rx_cntr < m * n * 16) usleep(1000);

    for(i = 0; i < m * n * 16; i += 8)
    {
      *(uint64_t *)(data0 + i) = *rx_data;
      *(uint64_t *)(data1 + i) = *rx_data;
      *(uint64_t *)(data2 + i) = *rx_data;
      *(uint64_t *)(data3 + i) = *rx_data;
    }

    data_offset = 0;
    for(i = 0; i < m; ++i)
    {
      *(uint32_t *)(buffer[i] + 4) = htonl(counter);

      memcpy(buffer[i] + 8, header + header_offset, 8);
      buffer[i][11] |= *gpio_in & 7;
      header_offset = header_offset >= 32 ? 0 : header_offset + 8;
      memset(buffer[i] + 16, 0, 504);

      buffer_offset = 16;
      for(j = 0; j < n; ++j)
      {
        memcpy(buffer[i] + buffer_offset, data0 + data_offset, 6);
        if(size > 8)
        {
          memcpy(buffer[i] + buffer_offset + 6, data1 + data_offset, 6);
        }
        if(size > 14)
        {
          memcpy(buffer[i] + buffer_offset + 12, data2 + data_offset, 6);
        }
        if(size > 20)
        {
          memcpy(buffer[i] + buffer_offset + 18, data3 + data_offset, 6);
        }
        data_offset += 8;
        buffer_offset += size;
      }

      memcpy(buffer[i] + 520, header + header_offset, 8);
      buffer[i][523] |= *gpio_in & 7;
      header_offset = header_offset >= 32 ? 0 : header_offset + 8;
      memset(buffer[i] + 528, 0, 504);

      buffer_offset = 528;
      for(j = 0; j < n; ++j)
      {
        memcpy(buffer[i] + buffer_offset, data0 + data_offset, 6);
        if(size > 8)
        {
          memcpy(buffer[i] + buffer_offset + 6, data1 + data_offset, 6);
        }
        if(size > 14)
        {
          memcpy(buffer[i] + buffer_offset + 12, data2 + data_offset, 6);
        }
        if(size > 20)
        {
          memcpy(buffer[i] + buffer_offset + 18, data3 + data_offset, 6);
        }
        data_offset += 8;
        buffer_offset += size;
      }

      ++counter;
    }

    sendmmsg(sock_ep2, datagram, m, 0);
  }

  active_thread = 0;

  return NULL;
}
int ini_sound()
{
		snd_pcm_hw_params_t *hw_params;
		snd_pcm_sw_params_t *sw_params;
		int nfds;
		int err;
		struct pollfd *pfds;
		static char *device = "default";                        /* playback device */
		unsigned int actualRate = 48000;
  /********************************************************************************/
  /* BEGIN ON3VNA : make shure we keep the ALSA alive and have 192000 bytes/sec   */
  /*                other whise we have buffer underrun from the USB sound device */
  /*                192000 bytes/sec is not feasable                              */
  /********************************************************************************/
		if ((err = snd_pcm_open (&playback_handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) 
		{
			syslog (LOG_ERR, "No USB audio sound device available\n");
			return (0);
		}   
		if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) 
		{
			syslog (LOG_ERR, "cannot allocate hardware parameter structure \n");
			return (0);
		}		 
		if ((err = snd_pcm_hw_params_any (playback_handle, hw_params)) < 0) 
		{
			syslog (LOG_ERR, "cannot initialize hardware parameter structure \n");
			return (0);
		}
		if ((err = snd_pcm_hw_params_set_access (playback_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) 
		{
			syslog (LOG_ERR, "cannot set access type \n");
			return (0);
		}	
		if ((err = snd_pcm_hw_params_set_format (playback_handle, hw_params, SND_PCM_FORMAT_S16_BE)) < 0) 
		{
						syslog (LOG_ERR, "cannot set sample format \n");
			return (0);
		}	
		if ((err = snd_pcm_hw_params_set_rate_near (playback_handle, hw_params, &actualRate, 0)) < 0) 
		{
			syslog (LOG_ERR, "cannot set sample rate \n");
			return (0);
		}	
		if ((err = snd_pcm_hw_params_set_channels (playback_handle, hw_params, 2)) < 0) 
		{
			syslog (LOG_ERR, "cannot set channel count \n");
			return (0);
		}
	
		if ((err = snd_pcm_hw_params (playback_handle, hw_params)) < 0) 
		{
			syslog (LOG_ERR, "cannot set hw_params parameters \n");
			return (0);
		}	
		snd_pcm_hw_params_free (hw_params);
		if ((err = snd_pcm_sw_params_malloc (&sw_params)) < 0) 
		{
			syslog (LOG_ERR, "cannot allocate software parameters structure \n");
			return (0);
		}
		if ((err = snd_pcm_sw_params_current (playback_handle, sw_params)) < 0) 
		{
			syslog (LOG_ERR, "cannot initialize software parameters structure \n");
			return (0);
		}
		if ((err = snd_pcm_sw_params_set_avail_min (playback_handle, sw_params, pcmframes)) < 0) 
		{
			syslog (LOG_ERR, "cannot set minimum available frames count \n");
			return (0);
		}
		if ((err = snd_pcm_sw_params_set_start_threshold (playback_handle, sw_params, 0U)) < 0) 
		{
			syslog (LOG_ERR, "cannot set start mode \n");
			return (0);
		}
		if ((err = snd_pcm_sw_params (playback_handle, sw_params)) < 0) 
		{
			syslog (LOG_ERR, "cannot set software parameters sw_params \n");
			return (0);
		}
		/* the interface will interrupt the kernel every pcmframes frames, and ALSA
		   will wake up this program very soon after that.
		*/
		if ((err = snd_pcm_prepare (playback_handle)) < 0) 
		{
			syslog (LOG_ERR, "cannot prepare audio interface for use \n");
			return (0);
		}
		return (1);
}	
void *handler_playback(void *arg)
{
		int err;
		openlog ("sdr-transceiver-hpsdr-handler-playback", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL0);
		syslog (LOG_NOTICE, "Program sdr-transceiver-hpsdr-handler-playback started by User %d", getuid ());
		
		snd_pcm_sframes_t frames_to_deliver;
		sound_avail=ini_sound();
		while (sound_avail) 
		{

			/* wait till the interface is ready for data, or 1 second
			   has elapsed.
			*/
				if ((err = snd_pcm_wait (playback_handle, 1000)) < 0) 
				{
					syslog (LOG_ERR, "poll failed\n");
			        break;
				}	           
			/* find out how much space is available for playback data */
	
				if ((frames_to_deliver = snd_pcm_avail_update (playback_handle)) < 0)
				{
					if (frames_to_deliver == -EPIPE) 
					{
						syslog (LOG_ERR, "an xrun occured\n");						
						break;
					} 
					else 
					{
						fprintf (stdout, "unknown ALSA avail update return value (%d)\n", 
						 frames_to_deliver);
						break;
					}
				}
				frames_to_deliver = frames_to_deliver > pcmframes ? pcmframes : frames_to_deliver;
				/* deliver the data */
				if (playback_callback (frames_to_deliver) != frames_to_deliver) 
				{
			        	syslog (LOG_ERR, "playback callback failed try recover \n");
						if ((err = snd_pcm_prepare (playback_handle)) < 0)
						{
							syslog (LOG_ERR, "recover failed\n");
						}	
					break;
				}
		}
  /**************/
  /*END ON3VNA  */
  /**************/
 
  return NULL;
}
int playback_callback (snd_pcm_sframes_t nframes)
{
	int err;
	long bytes_avail;
	
	bytes_avail=jack_ringbuffer_read_space(playback_data);
	if (bytes_avail >= 4*nframes)
	{
		printf ("playback callback called with %u frames\n", nframes);
	
		/* ... fill buf with data ... */
		jack_ringbuffer_read(playback_data, buf, 4*nframes);
		if ((err = snd_pcm_writei (playback_handle, buf, nframes)) < 0) 
		{
			syslog (LOG_ERR, "writei failed\n");
		}
		
	}
	else
	{
		/**********************************************/
		/* ON3VNA: no frames are comming from the PC  */
		/*         because we are stopped on the PC   */
		/*         or we changed config enable VAC    */
		/*         So keep the device alive and send  */
		/*         null to the sound device           */
		/*         If we dont do so we have cracking  */
		/*         noise until PC sends back frames   */
		/**********************************************/
		syslog (LOG_ERR, "Not enough data in jackbuffer send nullbuffer \n");
		if ((err = snd_pcm_writei (playback_handle, buffernull, nframes)) < 0) 
		{
			syslog (LOG_ERR, "writei null frames failed\n");
		}	
	}

	
	return err;
}
