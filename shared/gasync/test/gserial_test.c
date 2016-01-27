/*
 Copyright (c) 2016 Mathieu Laurendeau <mat.lau@laposte.net>
 License: GPLv3
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include <sys/time.h>

#include <gserial.h>
#include <ginput.h>
#include <gpoll.h>
#include <gtimer.h>
#include <gprio.h>
#include "common.h"

#define PERIOD 10000//microseconds

#ifdef WIN32
#define REGISTER_FUNCTION gpoll_register_handle
#else
#define REGISTER_FUNCTION gpoll_register_fd
#endif

static void terminate(int sig) {
  done = 1;
}

static char * port = NULL;

static int serial = -1;

static unsigned char * packet;
static unsigned char * result;
static unsigned int read = 0;
static unsigned int count = 0;

static unsigned int baudrate = 0;
static unsigned int samples = 0;
static unsigned short packet_size = 0;

static unsigned int verbose = 0;

static struct timeval t0, t1;

static unsigned int * tRead = NULL;
static unsigned int wread = 0;

void results(unsigned int * tdiff, unsigned int cpt) {
  unsigned int sum = 0;
  unsigned int average;
  unsigned int temp = 0;
  unsigned int nbval;
  unsigned int worst = 0;

  unsigned int i;
  for (i = 0; i < cpt && tdiff[i] > 0; i++) {
    sum = sum + tdiff[i];
    if (tdiff[i] > worst) {
      worst = tdiff[i];
    }
  }

  nbval = i;

  printf("%d\t", worst);

  if (nbval < 2) {
    return;
  }

  average = sum / nbval;

  printf("%d\t", average);

  for (i = 0; i < nbval; i++) {
    temp = temp + pow(abs(tdiff[i] - average), 2);
  }

  temp = pow(temp / (nbval - 1), 0.5);

  printf("%d\t", temp);
}

static void usage() {
  fprintf(stderr, "Usage: ./serial_bench [-p port] [-b baudrate] [-n samples] [-s packet size] -v\n");
  exit(EXIT_FAILURE);
}

/*
 * Reads command-line arguments.
 */
static int read_args(int argc, char* argv[]) {

  int opt;
  while ((opt = getopt(argc, argv, "b:n:p:s:v")) != -1) {
    switch (opt) {
    case 'b':
      baudrate = atoi(optarg);
      break;
    case 'n':
      samples = atoi(optarg);
      break;
    case 'p':
      port = optarg;
      break;
    case 's':
      packet_size = atoi(optarg);
      break;
    case 'v':
      verbose = 1;
      break;
    default: /* '?' */
      usage();
      break;
    }
  }
  return 0;
}

int serial_read(int user, const void * buf, int status) {

  if (status < 0) {
    done = 1;
    return 1;
  }

  memcpy(result + read, buf, status);
  read += status;

  gserial_set_read_size(serial, packet_size - read);

  if (read < packet_size) {
    return 0;
  }

  gettimeofday(&t1, NULL );

  int ret = 0;

  if (memcmp(result, packet, packet_size)) {

    fprintf(stderr, "bad packet content\n");
    done = 1;
    ret = -1;

  } else {

    unsigned int i;
    for (i = 0; i < packet_size; ++i) {
      packet[i]++;
    }

    tRead[count] = (t1.tv_sec * 1000000 + t1.tv_usec) - (t0.tv_sec * 1000000 + t0.tv_usec);

    if (tRead[count] > wread) {
      wread = tRead[count];
    }

    ++count;
    if (count == samples) {

      done = 1;

    } else {

      int status = gserial_write(serial, packet, packet_size);
      if (status < 0) {
        done = 1;
      }

      gettimeofday(&t0, NULL );
    }

    read = 0;
  }

  if (done) {
    ret = -1;
  }

  return ret;
}

int serial_close(int user) {
  done = 1;
  return 1;
}

int main(int argc, char* argv[]) {

  (void) signal(SIGINT, terminate);

  read_args(argc, argv);

  if(port == NULL || baudrate == 0 || samples == 0 || packet_size == 0)
  {
    usage();
    return -1;
  }

  packet = calloc(packet_size, sizeof(*packet));
  result = calloc(packet_size, sizeof(*result));

  tRead = calloc(samples, sizeof(*tRead));

  if(packet == NULL || result == NULL || tRead == NULL) {
    fprintf(stderr, "can't allocate memory to store samples\n");
    return -1;
  }

  if (gprio() < 0) {
    return -1;
  }

  serial = gserial_open(port, baudrate);
  if (serial < 0) {
    return -1;
  }

  gserial_register(serial, 42, serial_read, NULL, serial_close, REGISTER_FUNCTION);

  gserial_set_read_size(serial, packet_size);

  int ret = gserial_write(serial, packet, packet_size);
  if (ret < 0) {
    done = 1;
  }

  gettimeofday(&t0, NULL );

  while (!done) {

    gpoll();
  }

  gserial_close(serial);

  if(verbose) {
    printf("baudrate: %d ", baudrate);
    printf("samples: %d ", count);
    printf("packet size: %d\n", packet_size);
    printf("worst\tavg\tstdev\n");
  }
  results(tRead, count);
  printf("\n");

  return 0;
}