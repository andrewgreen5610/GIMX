#include "pcprog.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#ifdef WIN32
#define LINE_MAX 1024
#endif

#define USB_IDS_FILE "gpp.txt"

#define MAX_GPP_DEVICES 8

#define GPPKG_INPUT_REPORT      0x01
#define GPPKG_OUTPUT_REPORT     0x04
#define GPPKG_ENTER_CAPTURE     0x07
#define GPPKG_LEAVE_CAPTURE     0x08

#define REPORT_SIZE 64

typedef struct GIMX_PACKED
{
  uint8_t type;
  uint16_t length;
  uint8_t first;
} s_gppReportHeader;

typedef struct GIMX_PACKED
{
  uint8_t reportId;
  s_gppReportHeader header;
  uint8_t data[REPORT_SIZE-sizeof(s_gppReportHeader)];
} s_gppReport;

/*
 * https://www.consoletuner.com/kbase/compatible_devices_print.htm
 * http://controllermax.com/manual/Content/compatible_devices.htm
 */
GCAPI_USB_IDS usb_ids[16] =
{
    { .vid = 0x2508, .pid = 0x0001, .name = "GPP"                 }, //GPP, Cronus, CronusMAX, CronusMAX v2
    { .vid = 0x2508, .pid = 0x0002, .name = "Cronus"              }, //Cronus
    { .vid = 0x2508, .pid = 0x0003, .name = "Titan"               }, //Titan
    { .vid = 0x2508, .pid = 0x0004, .name = "CronusMAX v2"        }, //CronusMAX v2
    { .vid = 0x2008, .pid = 0x0001, .name = "CronusMAX PLUS (v3)" }, //CronusMAX PLUS (v3)

    // remaining space is for user-defined values!
};

static unsigned int nb_usb_ids = 5;

/*
 * Read user-defined usb ids.
 */
void gpppcprog_read_user_ids(const char * user_directory, const char * app_directory)
{
  char file_path[PATH_MAX];
  char line[LINE_MAX];
  FILE * fp;
  unsigned short vid;
  unsigned short pid;

  if(nb_usb_ids == sizeof(usb_ids) / sizeof(*usb_ids))
  {
    fprintf(stderr, "%s:%d no space for any user defined usb ids!\n", __FILE__, __LINE__);
    return;
  }

  snprintf(file_path, sizeof(file_path), "%s/%s/%s", user_directory, app_directory, USB_IDS_FILE);

  fp = fopen(file_path, "r");
  if (fp)
  {
    while (fgets(line, LINE_MAX, fp))
    {
      if(sscanf(line, "%hx %hx", &vid, &pid) == 2)
      {
        if(nb_usb_ids == sizeof(usb_ids) / sizeof(*usb_ids))
        {
          fprintf(stderr, "%s:%d no more space for user defined usb ids!\n", __FILE__, __LINE__);
          break;
        }
        if(vid && pid)
        {
          usb_ids[nb_usb_ids].vid = vid;
          usb_ids[nb_usb_ids].pid = pid;
          usb_ids[nb_usb_ids].name = "user-defined";
          ++nb_usb_ids;
        }
      }
    }
    fclose(fp);
  }
}

const GCAPI_USB_IDS * gpppcprog_get_ids(unsigned int * nb)
{
  *nb = nb_usb_ids;
  return usb_ids;
}

static struct
{
  int device;
  char * path;
  ASYNC_READ_CALLBACK fp_read;
  ASYNC_WRITE_CALLBACK fp_write;
  ASYNC_CLOSE_CALLBACK fp_close;
} gpp_devices[MAX_GPP_DEVICES] = {};

void gpppcprog_init(void) __attribute__((constructor (101)));
void gpppcprog_init(void)
{
    int i;
    for (i = 0; i < MAX_GPP_DEVICES; ++i) {
        gpp_devices[i].device = -1;
    }
}

void gpppcprog_clean(void) __attribute__((destructor (101)));
void gpppcprog_clean(void)
{
    int i;
    for (i = 0; i < MAX_GPP_DEVICES; ++i) {
        if(gpp_devices[i].device >= 0) {
            hidasync_close(gpp_devices[i].device);
        }
    }
}

int8_t gpppcprog_send(int id, uint8_t type, uint8_t * data, uint16_t length);

static uint8_t is_device_opened(const char* device)
{
  unsigned int i;
  for(i = 0; i < sizeof(gpp_devices) / sizeof(*gpp_devices); ++i)
  {
    if(gpp_devices[i].path && !strcmp(gpp_devices[i].path, device))
    {
      return 1;
    }
  }
  return 0;
}

int8_t gppcprog_connect(int id, const char * path)
{
  int r = 0;

  gppcprog_disconnect(id);

  if(path)
  {
    // Connect to the given device

    if(is_device_opened(path))
    {
      return -1;
    }

    gpp_devices[id].device = hidasync_open_path(path);

    if(gpp_devices[id].device >= 0)
    {
      gpp_devices[id].path = strdup(path);
    }
    else
    {
      fprintf(stderr, "failed to open %s\n", path);
    }
  }
  else
  {
    // Connect to any GPP/Cronus/Titan

    s_hid_dev *devs, *cur_dev;

    devs = hidasync_enumerate(0x0000, 0x0000);
    for(cur_dev = devs; cur_dev != NULL; ++cur_dev)
    {
      unsigned int i;
      for(i = 0; i < sizeof(usb_ids) / sizeof(*usb_ids); ++i)
      {
        if(cur_dev->vendor_id == usb_ids[i].vid && cur_dev->product_id == usb_ids[i].pid)
        {
          if(!is_device_opened(cur_dev->path))
          {
            if ((gpp_devices[id].device = hidasync_open_path(cur_dev->path)) >= 0)
            {
              gpp_devices[id].path = strdup(cur_dev->path);
              break;
            }
          }
        }
      }
      if(gpp_devices[id].device >= 0)
      {
        break;
      }
      if(cur_dev->next == 0) {
          break;
      }
    }
    hidasync_free_enumeration(devs);
  }

  if (gpp_devices[id].device < 0)
  {
    return -1;
  }

  // Enter Capture Mode
  r = gpppcprog_send(id, GPPKG_ENTER_CAPTURE, NULL, 0);
  if (r <= 0)
  {
    gppcprog_disconnect(id);
    return r;
  }

  return 1;
}

int8_t gppcprog_connected(int id)
{
  return gpp_devices[id].device < 0;
}

void gppcprog_disconnect(int id)
{
  if (gpp_devices[id].device >= 0)
  {
    //make sure the write is synchronous
    gpp_devices[id].fp_write = NULL;

    // Leave Capture Mode
    gpppcprog_send(id, GPPKG_LEAVE_CAPTURE, NULL, 0);

    // Disconnect to GPP
    hidasync_close(gpp_devices[id].device);
    gpp_devices[id].device = -1;
    free(gpp_devices[id].path);
    gpp_devices[id].path = NULL;
  }
  return;
}

int8_t gpppcprog_input(int id, GCAPI_REPORT *report, int timeout)
{
  int bytesReceived;
  uint8_t rcvBuf[65] = {};

  if (gpp_devices[id].device < 0 || report == NULL)
    return (-1);
  bytesReceived = hidasync_read_timeout(gpp_devices[id].device, rcvBuf, sizeof(rcvBuf), timeout);
  if (bytesReceived < 0)
  {
    gppcprog_disconnect(id);
    return (-1);
  }
  else if (bytesReceived && rcvBuf[0] == GPPKG_INPUT_REPORT)
  {
    *report = *(GCAPI_REPORT *)(rcvBuf + 7);
    return (1);
  }
  return (0);
}

static int read_callback(int user, const void * buf, unsigned int count)
{
  if(((unsigned char *)buf)[0] == GPPKG_INPUT_REPORT) {
    return gpp_devices[user].fp_read(user, buf, count);
  }
  return 0;
}

static int write_callback(int user, int transfered)
{
  if(gpp_devices[user].fp_write)
  {
    return gpp_devices[user].fp_write(user, transfered);
  }
  return 0;
}

static int close_callback(int user)
{
  return gpp_devices[user].fp_close(user);
}

int8_t gpppcprog_start_async(int id, ASYNC_READ_CALLBACK fp_read, ASYNC_WRITE_CALLBACK fp_write, ASYNC_CLOSE_CALLBACK fp_close, ASYNC_REGISTER_SOURCE fp_register)
{
  gpp_devices[id].fp_read = fp_read;
  gpp_devices[id].fp_write = fp_write;
  gpp_devices[id].fp_close = fp_close;

  return hidasync_register(gpp_devices[id].device, id, read_callback, write_callback, close_callback, fp_register);
}

int8_t gpppcprog_output(int id, int8_t output[GCAPI_INPUT_TOTAL])
{
  return gpppcprog_send(id, GPPKG_OUTPUT_REPORT, (uint8_t *)output, GCAPI_INPUT_TOTAL);
}

int8_t gpppcprog_send(int id, uint8_t type, uint8_t * data, uint16_t length)
{
  s_gppReport report =
  {
    .reportId = 0x00,
    .header =
    {
      .type = type,
      .length = length,
      .first = 1
    }
  };
  uint16_t sndLen;
  uint16_t i = 0;

  do
  {										// Data
    if (length)
    {
      sndLen = (((i + sizeof(report.data)) < length) ? sizeof(report.data) : (uint16_t)(length - i));
      memcpy(report.data, data + i, sndLen);
      i += sndLen;
    }
    if(gpp_devices[id].fp_write) {
      if (hidasync_write(gpp_devices[id].device, (unsigned char*)&report, sizeof(report)) == -1)
        return (0);
    }
    else {
      if (hidasync_write_timeout(gpp_devices[id].device, (unsigned char*)&report, sizeof(report), 1) == -1)
        return (0);
    }
    report.header.first = 0;
  }
  while (i < length);
  return (1);
}

