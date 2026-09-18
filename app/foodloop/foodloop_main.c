/****************************************************************************
 * FoodLoop M1: explicit, one-shot food capture for ESP32-S3-EYE.
 *
 * The camera is never opened while this process is idle. A BOOT press is the
 * only production trigger; the optional "capture" argument is for bench
 * validation through NSH.
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/input/buttons.h>
#include <nuttx/video/video.h>

#include <syslog.h>

#include <netutils/cJSON.h>

#include "foodloop_ui.h"

#define FOODLOOP_BUTTON_DEVICE "/dev/buttons"
#define FOODLOOP_CAMERA_DEVICE "/dev/video0"
#define FOODLOOP_CAPTURE_PATH  "/tmp/foodloop-last.rgb565"
#define FOODLOOP_DRAFT_PATH    "/tmp/foodloop-draft.json"
#define FOODLOOP_GATEWAY_PATH  "/v1/foodloop/analyze-rgb565"

/* Confirmed records.  The microSD ledger persists across reboots; the tmpfs
 * /data store is a fallback when no SD card is mounted.  One JSON object per
 * line so M3 can parse records.jsonl without a container format.
 */
#define FOODLOOP_RECORDS_DIR       "/mnt/foodloop"
#define FOODLOOP_RECORDS_PATH      "/mnt/foodloop/records.jsonl"
#define FOODLOOP_RECORDS_FALLBACK  "/data/foodloop-records.jsonl"
#define FOODLOOP_CONFIRM_WAIT_MS   30000

/* Boot mode defaults.  The gateway IP is a compile-time default that can be
 * overridden at runtime by writing a plain-text IP to /data/foodloop-gw.
 * Wi-Fi credentials live in the NVS flash partition and are reused on every
 * boot, so the board reconnects without any serial session.
 */

#define FOODLOOP_GATEWAY_DEFAULT_IP "192.168.2.48"
#define FOODLOOP_GATEWAY_IP_FILE    "/data/foodloop-gw"
#define FOODLOOP_NETWORK_WAIT_MS    30000

#define FOODLOOP_IMAGE_WIDTH     320
#define FOODLOOP_IMAGE_HEIGHT    240
#define FOODLOOP_IMAGE_BYTES     (FOODLOOP_IMAGE_WIDTH * \
                                  FOODLOOP_IMAGE_HEIGHT * 2)
#define FOODLOOP_CAPTURE_WAIT_MS 5000
#define FOODLOOP_PREVIEW_WAIT_MS 15000
#define FOODLOOP_CONNECT_WAIT_MS 3000
/* The M2 gateway runs a two-stage MiMo pipeline (transcribe, then structure),
 * so the draft can take longer than one single-shot request.  Keep the board
 * socket wait comfortably above the gateway's worst case.
 */
#define FOODLOOP_GATEWAY_IO_WAIT_MS 60000
#define FOODLOOP_GATEWAY_PORT    8789
#define FOODLOOP_IO_BUFFER_SIZE  1024
#define FOODLOOP_RESPONSE_MAX    8192
#define FOODLOOP_DEMO_FRAME_MS   2500

struct foodloop_buffer_s
{
  FAR uint8_t *data;
  size_t length;
};

static int foodloop_wait_for_network(int timeout_ms);
static int foodloop_confirm_and_save_draft(void);

/* The demo loop exists only for an explicitly invoked local presentation.
 * Every frame carries a visible DEMO badge. It does not open the camera,
 * contact a gateway, or create a record.
 */

static int foodloop_demo_loop(void)
{
  static const enum foodloop_ui_state_e g_demo_states[] =
  {
    FOODLOOP_UI_HOME,
    FOODLOOP_UI_SCANNING,
    FOODLOOP_UI_ANALYZING,
    FOODLOOP_UI_DRAFT_READY,
    FOODLOOP_UI_CONFIRMED
  };
  size_t index;
  int ret;

  printf("FoodLoop: DEMO loop started; no camera, network, or records.\n");

  for (;;)
    {
      for (index = 0; index < sizeof(g_demo_states) / sizeof(g_demo_states[0]);
           index++)
        {
          ret = foodloop_ui_show_demo(g_demo_states[index]);
          if (ret < 0)
            {
              printf("FoodLoop: DEMO display failed (%d)\n", -ret);
              return ret;
            }

          usleep(FOODLOOP_DEMO_FRAME_MS * 1000);
        }
    }
}

static int foodloop_write_all(int fd, FAR const uint8_t *data, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t written = write(fd, data + offset, length - offset);
      if (written < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (written == 0)
        {
          return -EIO;
        }

      offset += written;
    }

  return OK;
}

static int foodloop_save_file(FAR const char *path, FAR const uint8_t *data,
                              size_t length)
{
  int fd;
  int ret;

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  ret = foodloop_write_all(fd, data, length);
  close(fd);
  return ret;
}

static int foodloop_save_capture(FAR const uint8_t *data, size_t length)
{
  return foodloop_save_file(FOODLOOP_CAPTURE_PATH, data, length);
}

static int foodloop_save_draft(FAR const char *draft, size_t length)
{
  return foodloop_save_file(FOODLOOP_DRAFT_PATH,
                            (FAR const uint8_t *)draft, length);
}

static int foodloop_append_line(FAR const char *path, FAR const char *line,
                                size_t length)
{
  int fd;
  int ret;

  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  ret = foodloop_write_all(fd, (FAR const uint8_t *)line, length);
  if (ret == OK)
    {
      ret = foodloop_write_all(fd, (FAR const uint8_t *)"\n", 1);
    }

  close(fd);
  return ret;
}

static int foodloop_save_record(FAR const char *body)
{
  FAR char *line;
  FAR cJSON *root;
  FAR cJSON *draft;
  FAR char *printed;
  size_t line_size;
  time_t now;
  int ret;

  /* Extract the inner draft object from the gateway response so the ledger
   * stores the food record itself, not the HTTP envelope.  Tolerate a plain
   * draft body (top-level items) as a fallback. */

  root = cJSON_Parse(body);
  if (root == NULL)
    {
      return -EINVAL;
    }

  draft = cJSON_GetObjectItem(root, "draft");
  if (!cJSON_IsObject(draft))
    {
      draft = root;
    }

  printed = cJSON_PrintUnformatted(draft);
  if (printed == NULL)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  now = time(NULL);
  line_size = strlen(printed) + 96;
  line = malloc(line_size);
  if (line == NULL)
    {
      free(printed);
      cJSON_Delete(root);
      return -ENOMEM;
    }

  ret = snprintf(line, line_size, "{\"confirmed\":true,\"ts\":%lld,\"draft\":%s}",
                 (long long)now, printed);
  if (ret < 0 || ret >= (int)line_size)
    {
      free(line);
      free(printed);
      cJSON_Delete(root);
      return -EINVAL;
    }

  ret = foodloop_append_line(FOODLOOP_RECORDS_PATH, line, strlen(line));
  if (ret != OK)
    {
      mkdir(FOODLOOP_RECORDS_DIR, 0700);
      ret = foodloop_append_line(FOODLOOP_RECORDS_PATH, line, strlen(line));
    }

  if (ret != OK)
    {
      ret = foodloop_append_line(FOODLOOP_RECORDS_FALLBACK, line,
                                 strlen(line));
    }

  free(line);
  free(printed);
  cJSON_Delete(root);
  return ret;
}

static int foodloop_read_all(int fd, FAR uint8_t *data, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t read_size = read(fd, data + offset, length - offset);
      if (read_size < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (read_size == 0)
        {
          return -EIO;
        }

      offset += read_size;
    }

  return OK;
}

static void foodloop_discard_capture(void)
{
  unlink(FOODLOOP_CAPTURE_PATH);
}

static int foodloop_show_capture_preview(void)
{
  struct stat statbuf;
  FAR uint16_t *frame = NULL;
  int fd = -1;
  int ret = ERROR;

  fd = open(FOODLOOP_CAPTURE_PATH, O_RDONLY);
  if (fd < 0 || fstat(fd, &statbuf) < 0 ||
      statbuf.st_size != FOODLOOP_IMAGE_BYTES)
    {
      goto cleanup;
    }

  frame = memalign(32, FOODLOOP_IMAGE_BYTES);
  if (frame == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  ret = foodloop_read_all(fd, (FAR uint8_t *)frame, FOODLOOP_IMAGE_BYTES);
  if (ret == OK)
    {
      ret = foodloop_ui_show_preview(frame, FOODLOOP_IMAGE_WIDTH,
                                     FOODLOOP_IMAGE_HEIGHT);
    }

cleanup:
  if (fd >= 0)
    {
      close(fd);
    }

  free(frame);
  return ret;
}

static int foodloop_wait_socket(int fd, short events, int timeout_ms)
{
  struct pollfd pollfd;
  int ret;

  memset(&pollfd, 0, sizeof(pollfd));
  pollfd.fd = fd;
  pollfd.events = events;

  do
    {
      ret = poll(&pollfd, 1, timeout_ms);
    }
  while (ret < 0 && errno == EINTR);

  if (ret == 0)
    {
      return -ETIMEDOUT;
    }

  if (ret < 0)
    {
      return -errno;
    }

  if ((pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
      (pollfd.revents & events) == 0)
    {
      return -EIO;
    }

  return OK;
}

static int foodloop_send_all(int fd, FAR const uint8_t *data, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t sent = send(fd, data + offset, length - offset, 0);
      if (sent < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              int ret = foodloop_wait_socket(fd, POLLOUT,
                                             FOODLOOP_GATEWAY_IO_WAIT_MS);
              if (ret == OK)
                {
                  continue;
                }

              return ret;
            }

          return -errno;
        }

      if (sent == 0)
        {
          return -EIO;
        }

      offset += sent;
    }

  return OK;
}

static int foodloop_parse_port(FAR const char *text, uint16_t *port)
{
  char *end;
  long value = strtol(text, &end, 10);

  if (*text == '\0' || *end != '\0' || value < 1 || value > 65535)
    {
      return -EINVAL;
    }

  *port = (uint16_t)value;
  return OK;
}

static int foodloop_connect_gateway(int fd,
                                    FAR const struct sockaddr_in *address)
{
  int flags;
  int socket_error = 0;
  socklen_t error_length = sizeof(socket_error);
  int ret;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      return -errno;
    }

  ret = connect(fd, (FAR const struct sockaddr *)address, sizeof(*address));
  if (ret == 0)
    {
      return OK;
    }

  if (errno != EINPROGRESS)
    {
      return -errno;
    }

  ret = foodloop_wait_socket(fd, POLLOUT, FOODLOOP_CONNECT_WAIT_MS);
  if (ret < 0)
    {
      return ret;
    }

  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0)
    {
      return -errno;
    }

  return socket_error == 0 ? OK : -socket_error;
}

/****************************************************************************
 * Parse the MiMo draft JSON and show the food items on the LCD.  The draft
 * is a draft, not a final record: items are listed with name and expiry;
 * any unreadable values are simply omitted from the screen.
 ****************************************************************************/

#define FOODLOOP_DRAFT_MAX_LINES 6
#define FOODLOOP_DRAFT_LINE_LEN  36

static void foodloop_show_draft(FAR const char *body)
{
  FAR cJSON *root;
  FAR cJSON *items;
  FAR cJSON *item;
  FAR char *lines[FOODLOOP_DRAFT_MAX_LINES];
  int line_count = 0;
  int index;

  memset(lines, 0, sizeof(lines));
  root = cJSON_Parse(body);
  if (root == NULL)
    {
      foodloop_ui_show_draft_result(NULL, 0);
      return;
    }

  items = cJSON_GetObjectItem(root, "draft");
  if (cJSON_IsObject(items))
    {
      items = cJSON_GetObjectItem(items, "items");
    }
  else
    {
      items = cJSON_GetObjectItem(root, "items");
    }

  if (cJSON_IsArray(items))
    {
      cJSON_ArrayForEach(item, items)
        {
          FAR cJSON *name;
          FAR cJSON *expiry;
          FAR cJSON *storage;
          FAR char *line;
          int written;

          if (line_count >= FOODLOOP_DRAFT_MAX_LINES)
            {
              break;
            }

          name = cJSON_GetObjectItem(item, "name");
          expiry = cJSON_GetObjectItem(item, "expiry_date");
          storage = cJSON_GetObjectItem(item, "storage");
          if (!cJSON_IsString(name))
            {
              continue;
            }

          line = malloc(FOODLOOP_DRAFT_LINE_LEN);
          if (line == NULL)
            {
              break;
            }

          if (cJSON_IsString(expiry) && strlen(expiry->valuestring) >= 10)
            {
              written = snprintf(line, FOODLOOP_DRAFT_LINE_LEN, "%s %s",
                                 name->valuestring,
                                 &expiry->valuestring[5]);
            }
          else if (cJSON_IsString(storage) &&
                   strcmp(storage->valuestring, "unknown") != 0)
            {
              written = snprintf(line, FOODLOOP_DRAFT_LINE_LEN, "%s [%s]",
                                 name->valuestring, storage->valuestring);
            }
          else
            {
              written = snprintf(line, FOODLOOP_DRAFT_LINE_LEN, "%s",
                                 name->valuestring);
            }

          if (written < 0 || written >= FOODLOOP_DRAFT_LINE_LEN)
            {
              line[FOODLOOP_DRAFT_LINE_LEN - 1] = '\0';
            }

          lines[line_count++] = line;
        }
    }

  foodloop_ui_show_draft_result((FAR const char *const *)lines, line_count);

  for (index = 0; index < line_count; index++)
    {
      free(lines[index]);
    }

  cJSON_Delete(root);
}

static int foodloop_upload_capture(FAR const char *host, uint16_t port)
{
  struct sockaddr_in address;
  struct stat statbuf;
  char header[384];
  uint8_t buffer[FOODLOOP_IO_BUFFER_SIZE];
  FAR char *response;
  FAR char *body;
  int image_fd = -1;
  int socket_fd = -1;
  int header_size;
  int response_size = 0;
  int ret = ERROR;

  /* 8 KiB on the stack would overflow the 8 KiB task stack; allocate it. */

  response = malloc(FOODLOOP_RESPONSE_MAX);
  if (response == NULL)
    {
      printf("FoodLoop: out of memory for gateway response\n");
      foodloop_ui_show(FOODLOOP_UI_ERROR, true);
      return -ENOMEM;
    }

  foodloop_ui_show(FOODLOOP_UI_ANALYZING, true);

  image_fd = open(FOODLOOP_CAPTURE_PATH, O_RDONLY);
  if (image_fd < 0)
    {
      printf("FoodLoop: no captured image (%d)\n", errno);
      goto cleanup;
    }

  if (fstat(image_fd, &statbuf) < 0 || statbuf.st_size != FOODLOOP_IMAGE_BYTES)
    {
      printf("FoodLoop: capture file is not one QVGA RGB565 frame\n");
      goto cleanup;
    }

  socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_fd < 0)
    {
      printf("FoodLoop: cannot create gateway socket (%d)\n", errno);
      goto cleanup;
    }

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
      printf("FoodLoop: gateway must be an IPv4 address\n");
      goto cleanup;
    }

  /* The 2.4 GHz link can drop for up to a few seconds after a capture
   * and re-associate (the driver reports transient reason=2/204
   * disconnects); connect then fails with ENETUNREACH/EHOSTUNREACH.
   * Retry with a fresh socket since a failed connect cannot be
   * re-attempted on the same one.
   */

  for (int attempt = 1; attempt <= 5; attempt++)
    {
      if (attempt > 1)
        {
          close(socket_fd);
          usleep(1000000);

          socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
          if (socket_fd < 0)
            {
              printf("FoodLoop: cannot create gateway socket (%d)\n", errno);
              goto cleanup;
            }
        }

      ret = foodloop_connect_gateway(socket_fd, &address);
      if (ret == OK)
        {
          break;
        }

      printf("FoodLoop: gateway connect attempt %d failed (%d)\n",
             attempt, -ret);
    }

  if (ret < 0)
    {
      printf("FoodLoop: cannot reach gateway (%d)\n", -ret);
      goto cleanup;
    }

  header_size = snprintf(
    header, sizeof(header),
    "POST " FOODLOOP_GATEWAY_PATH " HTTP/1.1\r\n"
    "Host: %s:%u\r\n"
    "Content-Type: application/octet-stream\r\n"
    "X-FoodLoop-Width: %d\r\n"
    "X-FoodLoop-Height: %d\r\n"
    "Content-Length: %d\r\n"
    "Connection: close\r\n\r\n",
    host, (unsigned int)port, FOODLOOP_IMAGE_WIDTH, FOODLOOP_IMAGE_HEIGHT,
    FOODLOOP_IMAGE_BYTES);

  if (header_size < 0 || header_size >= (int)sizeof(header) ||
      foodloop_send_all(socket_fd, (FAR const uint8_t *)header,
                         (size_t)header_size) < 0)
    {
      printf("FoodLoop: gateway request header failed\n");
      goto cleanup;
    }

  for (;;)
    {
      ssize_t nread = read(image_fd, buffer, sizeof(buffer));
      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          printf("FoodLoop: capture file read failed (%d)\n", errno);
          goto cleanup;
        }

      if (nread == 0)
        {
          break;
        }

      if (foodloop_send_all(socket_fd, buffer, (size_t)nread) < 0)
        {
          printf("FoodLoop: gateway upload failed\n");
          goto cleanup;
        }
    }

  for (;;)
    {
      ssize_t received = recv(socket_fd, response + response_size,
                              FOODLOOP_RESPONSE_MAX - (size_t)response_size - 1, 0);
      if (received < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              ret = foodloop_wait_socket(socket_fd, POLLIN,
                                         FOODLOOP_GATEWAY_IO_WAIT_MS);
              if (ret == OK)
                {
                  continue;
                }

              printf("FoodLoop: gateway response timed out\n");
              goto cleanup;
            }

          printf("FoodLoop: gateway response failed (%d)\n", errno);
          goto cleanup;
        }

      if (received == 0)
        {
          break;
        }

      response_size += received;
      if (response_size >= FOODLOOP_RESPONSE_MAX - 1)
        {
          printf("FoodLoop: gateway response is too large\n");
          goto cleanup;
        }
    }

  response[response_size] = '\0';
  body = strstr(response, "\r\n\r\n");
  if (body == NULL)
    {
      printf("FoodLoop: malformed gateway response\n");
      goto cleanup;
    }

  body += 4;
  if (strncmp(response, "HTTP/1.1 200", 12) != 0)
    {
      printf("FoodLoop: gateway rejected scan: %.240s\n", body);
      goto cleanup;
    }

  if (foodloop_save_draft(body, strlen(body)) < 0)
    {
      printf("FoodLoop: could not save draft\n");
      goto cleanup;
    }

  printf("FoodLoop: MiMo draft saved to %s\n", FOODLOOP_DRAFT_PATH);
  printf("FoodLoop draft: %s\n", body);
  syslog(LOG_NOTICE, "FoodLoop: MiMo draft saved (HTTP 200)\n");
  foodloop_show_draft(body);
  ret = OK;

cleanup:
  if (ret != OK)
    {
      foodloop_ui_show(FOODLOOP_UI_ERROR, true);
    }

  free(response);

  if (image_fd >= 0)
    {
      close(image_fd);
    }

  if (socket_fd >= 0)
    {
      close(socket_fd);
    }

  /* Upload succeeded and the draft was shown: hand the flow to the user for
   * confirmation.  Confirming stores the record; timing out discards the
   * draft and returns to the waiting state.  Either way the loop continues
   * with the camera closed. */

  if (ret == OK)
    {
      foodloop_confirm_and_save_draft();
    }

  return ret;
}

static int foodloop_capture_once(size_t *capture_size)
{
  struct v4l2_format format;
  struct v4l2_requestbuffers request;
  struct v4l2_buffer buffer;
  struct pollfd pollfd;
  struct foodloop_buffer_s frame;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int fd = -1;
  int ret = ERROR;
  int saved;

  memset(&frame, 0, sizeof(frame));
  *capture_size = 0;

  fd = open(FOODLOOP_CAMERA_DEVICE, O_RDWR);
  if (fd < 0)
    {
      printf("FoodLoop: cannot open camera (%d)\n", errno);
      goto cleanup;
    }

  memset(&format, 0, sizeof(format));
  format.type = type;
  format.fmt.pix.width = FOODLOOP_IMAGE_WIDTH;
  format.fmt.pix.height = FOODLOOP_IMAGE_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;

  if (ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      printf("FoodLoop: camera format rejected (%d)\n", errno);
      goto cleanup;
    }

  frame.length = FOODLOOP_IMAGE_BYTES;
  frame.data = memalign(32, frame.length);
  if (frame.data == NULL)
    {
      printf("FoodLoop: not enough memory for image\n");
      goto cleanup;
    }

  memset(&request, 0, sizeof(request));
  request.type = type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = 1;
  request.mode = V4L2_BUF_MODE_FIFO;

  if (ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      printf("FoodLoop: cannot reserve camera buffer (%d)\n", errno);
      goto cleanup;
    }

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = type;
  buffer.memory = V4L2_MEMORY_USERPTR;
  buffer.index = 0;
  buffer.m.userptr = (uintptr_t)frame.data;
  buffer.length = frame.length;

  if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0 ||
      ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("FoodLoop: cannot start one-shot capture (%d)\n", errno);
      goto cleanup;
    }

  memset(&pollfd, 0, sizeof(pollfd));
  pollfd.fd = fd;
  pollfd.events = POLLIN;
  if (poll(&pollfd, 1, FOODLOOP_CAPTURE_WAIT_MS) <= 0)
    {
      printf("FoodLoop: camera capture timed out\n");
      goto cleanup;
    }

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = type;
  buffer.memory = V4L2_MEMORY_USERPTR;
  if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0 ||
      buffer.bytesused == 0 || buffer.bytesused > frame.length)
    {
      printf("FoodLoop: invalid camera frame (%d)\n", errno);
      goto cleanup;
    }

  saved = foodloop_save_capture(frame.data, buffer.bytesused);
  if (saved < 0)
    {
      printf("FoodLoop: could not save capture (%d)\n", -saved);
      goto cleanup;
    }

  *capture_size = buffer.bytesused;
  ret = OK;

cleanup:
  if (fd >= 0)
    {
      ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
      memset(&request, 0, sizeof(request));
      request.type = type;
      request.memory = V4L2_MEMORY_USERPTR;
      ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&request);
      close(fd);
    }

  free(frame.data);
  return ret;
}

/****************************************************************************
 * Live viewfinder: keeps the camera streaming and refreshes the LCD with
 * each frame until a BOOT press.  The first press stops streaming, saves
 * the current frame, and returns OK; a second BOOT press then uploads.
 ****************************************************************************/

struct foodloop_live_s
{
  FAR uint8_t *frame;
  size_t capture_size;
  int fd;
};

static int foodloop_live_start(struct foodloop_live_s *live)
{
  struct v4l2_format format;
  struct v4l2_requestbuffers request;
  struct v4l2_buffer buffer;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int ret = ERROR;

  memset(live, 0, sizeof(*live));
  live->fd = -1;

  live->frame = memalign(32, FOODLOOP_IMAGE_BYTES);
  if (live->frame == NULL)
    {
      return -ENOMEM;
    }

  live->fd = open(FOODLOOP_CAMERA_DEVICE, O_RDWR);
  if (live->fd < 0)
    {
      ret = -errno;
      goto errout;
    }

  memset(&format, 0, sizeof(format));
  format.type = type;
  format.fmt.pix.width = FOODLOOP_IMAGE_WIDTH;
  format.fmt.pix.height = FOODLOOP_IMAGE_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;

  if (ioctl(live->fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      ret = -errno;
      goto errout;
    }

  memset(&request, 0, sizeof(request));
  request.type = type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = 1;
  request.mode = V4L2_BUF_MODE_FIFO;

  if (ioctl(live->fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      ret = -errno;
      goto errout;
    }

  /* STREAMON with an empty queue: the per-frame QBUF in next_frame() arms
   * the buffer, keeps the container out of the queue while the caller
   * renders, and restarts capture after each DQBUF releases it.
   */

  if (ioctl(live->fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      ret = -errno;
      goto errout;
    }

  return OK;

errout:
  if (live->fd >= 0)
    {
      close(live->fd);
      live->fd = -1;
    }

  free(live->frame);
  live->frame = NULL;
  return ret;
}

static int foodloop_live_next_frame(struct foodloop_live_s *live)
{
  struct v4l2_buffer buffer;
  struct pollfd pollfd;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int ret;

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = type;
  buffer.memory = V4L2_MEMORY_USERPTR;
  buffer.index = 0;
  buffer.m.userptr = (uintptr_t)live->frame;
  buffer.length = FOODLOOP_IMAGE_BYTES;

  /* Arm the buffer.  FIFO mode stops capture once a frame lands without a
   * queued buffer, so this QBUF must precede the wait on every cycle.
   */

  if (ioctl(live->fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
    {
      return -errno;
    }

  memset(&pollfd, 0, sizeof(pollfd));
  pollfd.fd = live->fd;
  pollfd.events = POLLIN;
  if (poll(&pollfd, 1, FOODLOOP_CAPTURE_WAIT_MS) <= 0)
    {
      return -ETIMEDOUT;
    }

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = type;
  buffer.memory = V4L2_MEMORY_USERPTR;
  ret = ioctl(live->fd, VIDIOC_DQBUF, (uintptr_t)&buffer);
  if (ret < 0)
    {
      return -errno;
    }

  live->capture_size = buffer.bytesused;
  return OK;
}

static int foodloop_live_stop(struct foodloop_live_s *live)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (live->fd >= 0)
    {
      ioctl(live->fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
      close(live->fd);
      live->fd = -1;
    }

  free(live->frame);
  live->frame = NULL;
  return OK;
}

static int foodloop_wait_for_boot_timeout(int timeout_ms)
{
  struct pollfd pollfd;
  btn_buttonset_t sample;
  ssize_t count;
  int fd;

  fd = open(FOODLOOP_BUTTON_DEVICE, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("FoodLoop: cannot open BOOT button (%d)\n", errno);
      return -errno;
    }

  memset(&pollfd, 0, sizeof(pollfd));
  pollfd.fd = fd;
  pollfd.events = POLLIN;

  for (;;)
    {
      int poll_result = poll(&pollfd, 1, timeout_ms);
      if (poll_result == 0)
        {
          close(fd);
          return -ETIMEDOUT;
        }

      if (poll_result < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          close(fd);
          return -errno;
        }

      count = read(fd, &sample, sizeof(sample));
      if (count == sizeof(sample) && sample != 0)
        {
          close(fd);
          return OK;
        }
    }
}

static int foodloop_wait_for_boot(void)
{
  return foodloop_wait_for_boot_timeout(-1);
}

static int foodloop_read_file(FAR const char *path, FAR char **body)
{
  struct stat statbuf;
  FAR char *data;
  int fd;
  int ret;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &statbuf) < 0 || statbuf.st_size <= 0 ||
      statbuf.st_size > FOODLOOP_RESPONSE_MAX)
    {
      close(fd);
      return -EINVAL;
    }

  data = malloc((size_t)statbuf.st_size + 1);
  if (data == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  ret = foodloop_read_all(fd, (FAR uint8_t *)data, (size_t)statbuf.st_size);
  close(fd);
  if (ret != OK)
    {
      free(data);
      return ret;
    }

  data[statbuf.st_size] = '\0';
  *body = data;
  return OK;
}

static int foodloop_read_draft(FAR char **body)
{
  return foodloop_read_file(FOODLOOP_DRAFT_PATH, body);
}

static void foodloop_discard_draft(void)
{
  unlink(FOODLOOP_DRAFT_PATH);
}

static int foodloop_confirm_and_save_draft(void)
{
  FAR char *body = NULL;
  int ret;

  ret = foodloop_read_draft(&body);
  if (ret != OK)
    {
      printf("FoodLoop: no draft to confirm (%d)\n", -ret);
      return ret;
    }

  foodloop_ui_show(FOODLOOP_UI_DRAFT_READY, true);
  printf("FoodLoop: draft ready. Press BOOT within %d seconds to confirm.\n",
         FOODLOOP_CONFIRM_WAIT_MS / 1000);

  ret = foodloop_wait_for_boot_timeout(FOODLOOP_CONFIRM_WAIT_MS);
  if (ret == -ETIMEDOUT)
    {
      printf("FoodLoop: draft not confirmed; discarded.\n");
      foodloop_discard_draft();
      foodloop_ui_show(FOODLOOP_UI_HOME, true);
      free(body);
      return OK;
    }

  if (ret < 0)
    {
      printf("FoodLoop: confirm wait failed (%d)\n", -ret);
      foodloop_discard_draft();
      free(body);
      return ret;
    }

  ret = foodloop_save_record(body);
  free(body);
  if (ret < 0)
    {
      printf("FoodLoop: record save failed (%d)\n", -ret);
      foodloop_ui_show(FOODLOOP_UI_ERROR, true);
      return ret;
    }

  printf("FoodLoop: record confirmed and saved.\n");
  syslog(LOG_NOTICE, "FoodLoop: record confirmed and saved\n");
  foodloop_ui_show(FOODLOOP_UI_CONFIRMED, true);
  return OK;
}

#define FOODLOOP_LIST_MAX_LINES 6
#define FOODLOOP_LIST_LINE_LEN  36

static int foodloop_list_records(void)
{
  FAR char *body;
  FAR char *lines[FOODLOOP_LIST_MAX_LINES];
  FAR char *line;
  FAR char *saveptr;
  int line_count = 0;
  int index;
  int ret;

  memset(lines, 0, sizeof(lines));

  ret = foodloop_read_file(FOODLOOP_RECORDS_PATH, &body);
  if (ret < 0)
    {
      ret = foodloop_read_file(FOODLOOP_RECORDS_FALLBACK, &body);
    }

  if (ret < 0)
    {
      printf("FoodLoop: no records yet (%d)\n", -ret);
      foodloop_ui_show_list_result("NO RECORDS", NULL, 0);
      return ret;
    }

  printf("FoodLoop: local records:\n");

  for (line = strtok_r(body, "\n", &saveptr); line != NULL;
       line = strtok_r(NULL, "\n", &saveptr))
    {
      FAR cJSON *root;
      FAR cJSON *draft;
      FAR cJSON *items;
      FAR cJSON *item;

      if (line_count >= FOODLOOP_LIST_MAX_LINES)
        {
          break;
        }

      root = cJSON_Parse(line);
      if (root == NULL)
        {
          continue;
        }

      draft = cJSON_GetObjectItem(root, "draft");
      items = cJSON_IsObject(draft) ?
              cJSON_GetObjectItem(draft, "items") : NULL;
      if (cJSON_IsArray(items))
        {
          cJSON_ArrayForEach(item, items)
            {
              FAR cJSON *name;
              FAR cJSON *expiry;
              FAR char *entry;
              int written;

              if (line_count >= FOODLOOP_LIST_MAX_LINES)
                {
                  break;
                }

              name = cJSON_GetObjectItem(item, "name");
              expiry = cJSON_GetObjectItem(item, "expiry_date");
              if (!cJSON_IsString(name))
                {
                  continue;
                }

              entry = malloc(FOODLOOP_LIST_LINE_LEN);
              if (entry == NULL)
                {
                  break;
                }

              if (cJSON_IsString(expiry) &&
                  strlen(expiry->valuestring) >= 10)
                {
                  written = snprintf(entry, FOODLOOP_LIST_LINE_LEN, "%s %s",
                                     name->valuestring,
                                     &expiry->valuestring[5]);
                }
              else
                {
                  written = snprintf(entry, FOODLOOP_LIST_LINE_LEN, "%s",
                                     name->valuestring);
                }

              if (written < 0 || written >= FOODLOOP_LIST_LINE_LEN)
                {
                  entry[FOODLOOP_LIST_LINE_LEN - 1] = '\0';
                }

              printf("  %s\n", entry);
              lines[line_count++] = entry;
            }
        }

      cJSON_Delete(root);
    }

  if (line_count == 0)
    {
      printf("  (no items)\n");
      foodloop_ui_show_list_result("EMPTY LIST", NULL, 0);
    }
  else
    {
      foodloop_ui_show_list_result("FOOD LIST",
                                   (FAR const char *const *)lines,
                                   line_count);
    }

  for (index = 0; index < line_count; index++)
    {
      free(lines[index]);
    }

  free(body);
  return OK;
}

static void foodloop_print_ready(FAR const char *gateway_host)
{
  printf("\nFoodLoop ready\n");
  printf("Privacy: camera closed until BOOT is pressed.\n");
  if (gateway_host)
    {
      printf("BOOT: preview -> confirm photo -> MiMo draft via %s\n\n",
             gateway_host);
    }
  else
    {
      printf("BOOT: preview -> confirm one QVGA RGB565 photo\n\n");
    }
}

/****************************************************************************
 * Expiry status for confirmed records.  Each item is labelled USE TODAY /
 * USE SOON / EXPIRED / OK against a reference date (today, or an explicit
 * YYYY-MM-DD argument for testing without a synchronized clock).  Items
 * that still need confirmation never produce a reminder (M2 boundary).
 ****************************************************************************/

#define FOODLOOP_STATUS_USE_SOON_DAYS 3

static long foodloop_days_from_civil(int year, int month, int day)
{
  /* Howard Hinnant's days_from_civil: days since 1970-01-01. */

  year -= month <= 2;
  long era = (year >= 0 ? year : year - 399) / 400;
  unsigned yoe = (unsigned)(year - era * 400);
  unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

static int foodloop_parse_date(FAR const char *text, long *days)
{
  int year;
  int month;
  int day;

  if (sscanf(text, "%d-%d-%d", &year, &month, &day) != 3 ||
      month < 1 || month > 12 || day < 1 || day > 31)
    {
      return -EINVAL;
    }

  *days = foodloop_days_from_civil(year, month, day);
  return OK;
}

static int foodloop_status_records(FAR const char *today_text)
{
  FAR char *body;
  FAR char *lines[FOODLOOP_LIST_MAX_LINES];
  FAR char *line;
  FAR char *saveptr;
  char today[16];
  long today_days;
  int line_count = 0;
  int index;
  int ret;

  memset(lines, 0, sizeof(lines));

  if (today_text != NULL && today_text[0] != '\0')
    {
      strlcpy(today, today_text, sizeof(today));
    }
  else
    {
      time_t now = time(NULL);
      struct tm tm;
      char full[64];

      if (gmtime_r(&now, &tm) == NULL)
        {
          printf("FoodLoop: cannot read clock\n");
          return -errno;
        }

      snprintf(full, sizeof(full), "%04d-%02d-%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
      strlcpy(today, full, sizeof(today));
    }

  if (foodloop_parse_date(today, &today_days) < 0)
    {
      printf("FoodLoop: invalid today '%s'\n", today);
      return -EINVAL;
    }

  ret = foodloop_read_file(FOODLOOP_RECORDS_PATH, &body);
  if (ret < 0)
    {
      ret = foodloop_read_file(FOODLOOP_RECORDS_FALLBACK, &body);
    }

  if (ret < 0)
    {
      printf("FoodLoop: no records yet (%d)\n", -ret);
      foodloop_ui_show_list_result("NO RECORDS", NULL, 0);
      return ret;
    }

  printf("FoodLoop: status today %s:\n", today);

  for (line = strtok_r(body, "\n", &saveptr); line != NULL;
       line = strtok_r(NULL, "\n", &saveptr))
    {
      FAR cJSON *root;
      FAR cJSON *draft;
      FAR cJSON *items;
      FAR cJSON *item;

      root = cJSON_Parse(line);
      if (root == NULL)
        {
          continue;
        }

      draft = cJSON_GetObjectItem(root, "draft");
      items = cJSON_IsObject(draft) ?
              cJSON_GetObjectItem(draft, "items") : NULL;
      if (cJSON_IsArray(items))
        {
          cJSON_ArrayForEach(item, items)
            {
              FAR cJSON *name;
              FAR cJSON *expiry;
              FAR cJSON *needs;
              FAR char *entry;
              long expiry_days;
              long delta;
              FAR const char *label;
              int written;

              if (line_count >= FOODLOOP_LIST_MAX_LINES)
                {
                  break;
                }

              name = cJSON_GetObjectItem(item, "name");
              if (!cJSON_IsString(name))
                {
                  continue;
                }

              /* M2 boundary: unconfirmed items never produce reminders. */

              needs = cJSON_GetObjectItem(item, "needs_confirmation");
              if (cJSON_IsTrue(needs))
                {
                  continue;
                }

              expiry = cJSON_GetObjectItem(item, "expiry_date");
              if (!cJSON_IsString(expiry) ||
                  foodloop_parse_date(expiry->valuestring, &expiry_days) < 0)
                {
                  printf("  %s [no date]\n", name->valuestring);
                  continue;
                }

              delta = expiry_days - today_days;
              if (delta < 0)
                {
                  label = "EXPIRED";
                }
              else if (delta == 0)
                {
                  label = "USE TODAY";
                }
              else if (delta <= FOODLOOP_STATUS_USE_SOON_DAYS)
                {
                  label = "USE SOON";
                }
              else
                {
                  label = "OK";
                }

              printf("  %-20s %s (%s)\n", name->valuestring, label,
                     expiry->valuestring);

              if (delta <= FOODLOOP_STATUS_USE_SOON_DAYS)
                {
                  entry = malloc(FOODLOOP_LIST_LINE_LEN);
                  if (entry == NULL)
                    {
                      break;
                    }

                  written = snprintf(entry, FOODLOOP_LIST_LINE_LEN, "%s %s",
                                     label, &expiry->valuestring[5]);
                  if (written < 0 || written >= FOODLOOP_LIST_LINE_LEN)
                    {
                      entry[FOODLOOP_LIST_LINE_LEN - 1] = '\0';
                    }

                  lines[line_count++] = entry;
                }
            }
        }

      cJSON_Delete(root);
    }

  if (line_count == 0)
    {
      printf("FoodLoop: nothing needs attention today.\n");
      foodloop_ui_show_list_result("ALL OK", NULL, 0);
    }
  else
    {
      foodloop_ui_show_list_result("USE SOON",
                                   (FAR const char *const *)lines,
                                   line_count);
    }

  for (index = 0; index < line_count; index++)
    {
      free(lines[index]);
    }

  free(body);
  return OK;
}

static int foodloop_capture_preview(bool uses_mimo)
{
  size_t capture_size;
  int ret;

  foodloop_ui_show(FOODLOOP_UI_SCANNING, uses_mimo);
  printf("FoodLoop: capturing local preview...\n");
  ret = foodloop_capture_once(&capture_size);
  if (ret != OK)
    {
      printf("FoodLoop: preview capture failed; camera is now closed.\n");
      foodloop_ui_show(FOODLOOP_UI_ERROR, uses_mimo);
      return ret;
    }

  ret = foodloop_show_capture_preview();
  if (ret != OK)
    {
      printf("FoodLoop: preview display failed (%d).\n", -ret);
      foodloop_discard_capture();
      foodloop_ui_show(FOODLOOP_UI_ERROR, uses_mimo);
      return ret;
    }

  printf("FoodLoop: preview ready (%zu bytes); camera is closed.\n",
         capture_size);
  return OK;
}

static int foodloop_capture_and_maybe_analyze(FAR const char *gateway_host,
                                               uint16_t gateway_port)
{
  size_t capture_size;
  int ret;

  foodloop_ui_show(FOODLOOP_UI_SCANNING, gateway_host != NULL);
  printf("FoodLoop: scanning one photo...\n");
  ret = foodloop_capture_once(&capture_size);
  if (ret != OK)
    {
      printf("FoodLoop: scan failed; camera is now closed.\n");
      foodloop_ui_show(FOODLOOP_UI_ERROR, gateway_host != NULL);
      return ret;
    }

  printf("FoodLoop: saved %zu bytes.\n", capture_size);
  if (gateway_host == NULL)
    {
      printf("FoodLoop: ready for MiMo analysis.\n");
      foodloop_ui_show(FOODLOOP_UI_CAPTURED, false);
      return OK;
    }

  /* The capture may have taken long enough for the link to drop and
   * re-associate; wait for the default route before connecting, the same
   * way foodloop_boot_watch() does before uploading.
   */

  printf("FoodLoop: requesting MiMo food draft...\n");
  ret = foodloop_wait_for_network(FOODLOOP_NETWORK_WAIT_MS);
  if (ret != OK)
    {
      printf("FoodLoop: network not ready (%d); upload skipped.\n", -ret);
      foodloop_ui_show(FOODLOOP_UI_ERROR, gateway_host != NULL);
      return ret;
    }

  return foodloop_upload_capture(gateway_host, gateway_port);
}

static int foodloop_watch(FAR const char *gateway_host, uint16_t gateway_port)
{
  int ret;

  foodloop_ui_show(FOODLOOP_UI_HOME, gateway_host != NULL);

  for (;;)
    {
      foodloop_print_ready(gateway_host);
      ret = foodloop_wait_for_boot();
      if (ret < 0)
        {
          return ERROR;
        }

      ret = foodloop_capture_preview(gateway_host != NULL);
      if (ret != OK)
        {
          continue;
        }

      printf("FoodLoop: press BOOT again within 15 seconds to confirm.\n");
      ret = foodloop_wait_for_boot_timeout(FOODLOOP_PREVIEW_WAIT_MS);
      if (ret == OK)
        {
          foodloop_capture_and_maybe_analyze(gateway_host, gateway_port);
          continue;
        }

      foodloop_discard_capture();
      if (ret == -ETIMEDOUT)
        {
          printf("FoodLoop: preview cancelled after 15 seconds.\n");
          foodloop_ui_show(FOODLOOP_UI_HOME, gateway_host != NULL);
          continue;
        }

      return ERROR;
    }
}

/****************************************************************************
 * Boot mode: power-on viewfinder.  The camera streams continuously and the
 * LCD shows a full-screen live view.  The first BOOT press captures the
 * current frame; the second BOOT press uploads it to the gateway and shows
 * the MiMo draft on the LCD.
 ****************************************************************************/

static int foodloop_read_gateway_ip(FAR char *out, size_t out_size)
{
  struct stat statbuf;
  ssize_t count;
  int fd;

  fd = open(FOODLOOP_GATEWAY_IP_FILE, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &statbuf) < 0)
    {
      close(fd);
      return -errno;
    }

  count = read(fd, out, out_size - 1);
  close(fd);
  if (count < 0)
    {
      return -errno;
    }

  out[count] = '\0';
  while (count > 0 && (out[count - 1] == '\n' || out[count - 1] == '\r'))
    {
      out[--count] = '\0';
    }

  /* inet_pton requires a writable dst; pass a scratch buffer even though
   * only validation is wanted.
   */

  struct in_addr scratch;
  return inet_pton(AF_INET, out, &scratch) == 1 ? OK : -EINVAL;
}

static int foodloop_wait_for_network(int timeout_ms)
{
  struct sockaddr_in address;
  int fd;
  int ret = -ETIMEDOUT;

  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(9);
  inet_pton(AF_INET, "8.8.8.8", &address.sin_addr);

  for (;;)
    {
      /* A sendto without connect reports ENETUNREACH until the default
       * route exists, so it doubles as a route probe.
       */

      if (sendto(fd, "x", 1, 0, (FAR struct sockaddr *)&address,
                 sizeof(address)) >= 0)
        {
          ret = OK;
          break;
        }

      if (errno != ENETUNREACH && errno != EHOSTUNREACH &&
          errno != EADDRNOTAVAIL)
        {
          ret = -errno;
          break;
        }

      if (timeout_ms <= 0)
        {
          break;
        }

      usleep(250000);
      timeout_ms -= 250;
    }

  close(fd);
  return ret;
}

static int foodloop_boot_watch(FAR const char *gateway_host,
                               uint16_t gateway_port)
{
  struct foodloop_live_s live;
  struct pollfd button_poll;
  btn_buttonset_t sample;
  FAR const char *effective_host;
  char gateway_ip[INET_ADDRSTRLEN];
  int button_fd = -1;
  int ret;

  foodloop_ui_show(FOODLOOP_UI_HOME, gateway_host != NULL);

  /* Resolve the gateway address: explicit argument, runtime file, or the
   * compile-time default.  The board reconnects to the saved Wi-Fi network
   * from NVS while the viewfinder waits for the first press.
   */

  effective_host = gateway_host;
  if (effective_host == NULL &&
      foodloop_read_gateway_ip(gateway_ip, sizeof(gateway_ip)) == OK)
    {
      effective_host = gateway_ip;
    }

  if (effective_host == NULL)
    {
      effective_host = FOODLOOP_GATEWAY_DEFAULT_IP;
    }

  for (;;)
    {
      /* Wait for the first BOOT press that enters the live viewfinder. */

      syslog(LOG_NOTICE, "FoodLoop: home; waiting for BOOT press\n");
      ret = foodloop_wait_for_boot();
      if (ret < 0)
        {
          return ERROR;
        }

      syslog(LOG_NOTICE, "FoodLoop: BOOT 1/2 - starting live viewfinder\n");
      foodloop_ui_show(FOODLOOP_UI_SCANNING, true);
      printf("FoodLoop: live viewfinder on; press BOOT to capture.\n");

      ret = foodloop_live_start(&live);
      if (ret != OK)
        {
          printf("FoodLoop: live viewfinder failed (%d).\n", -ret);
          syslog(LOG_NOTICE, "FoodLoop: live viewfinder failed (%d)\n", -ret);
          foodloop_ui_show(FOODLOOP_UI_ERROR, true);
          continue;
        }

      /* Refresh the LCD with each frame, watching for the BOOT press. */

      button_fd = open(FOODLOOP_BUTTON_DEVICE, O_RDONLY | O_NONBLOCK);
      if (button_fd < 0)
        {
          foodloop_live_stop(&live);
          foodloop_ui_show(FOODLOOP_UI_ERROR, true);
          continue;
        }

      memset(&button_poll, 0, sizeof(button_poll));
      button_poll.fd = button_fd;
      button_poll.events = POLLIN;

      for (;;)
        {
          int poll_result;

          ret = foodloop_live_next_frame(&live);
          if (ret != OK)
            {
              printf("FoodLoop: live frame failed (%d).\n", -ret);
              break;
            }

          foodloop_ui_show_live_frame((FAR uint16_t *)live.frame,
                                      FOODLOOP_IMAGE_WIDTH,
                                      FOODLOOP_IMAGE_HEIGHT);

          poll_result = poll(&button_poll, 1, 0);
          if (poll_result < 0)
            {
              if (errno == EINTR)
                {
                  continue;
                }

              printf("FoodLoop: button poll failed (%d).\n", errno);
              break;
            }

          if (poll_result > 0 &&
              read(button_fd, &sample, sizeof(sample)) == sizeof(sample) &&
              sample != 0)
            {
              /* First press: lock the current frame. */

              ret = foodloop_save_capture(live.frame, live.capture_size);
              if (ret < 0)
                {
                  printf("FoodLoop: could not save capture (%d)\n", -ret);
                  break;
                }

              printf("FoodLoop: captured %zu bytes; press BOOT to analyze.\n",
                     live.capture_size);
              foodloop_ui_show(FOODLOOP_UI_CAPTURED, true);
              break;
            }
        }

      close(button_fd);
      button_fd = -1;
      foodloop_live_stop(&live);

      if (ret != OK)
        {
          continue;
        }

      /* Second BOOT press: analyze the locked frame. */

      syslog(LOG_NOTICE, "FoodLoop: BOOT 2/2 - uploading to MiMo\n");
      ret = foodloop_wait_for_boot();
      if (ret < 0)
        {
          return ERROR;
        }

      if (effective_host != NULL)
        {
          printf("FoodLoop: waiting for network...\n");
          ret = foodloop_wait_for_network(FOODLOOP_NETWORK_WAIT_MS);
          if (ret != OK)
            {
              printf("FoodLoop: network not ready (%d); draft upload skipped.\n",
                     -ret);
              syslog(LOG_NOTICE,
                     "FoodLoop: network not ready (%d); upload skipped\n",
                     -ret);
              foodloop_ui_show(FOODLOOP_UI_ERROR, true);
              continue;
            }

          foodloop_upload_capture(effective_host, gateway_port);
          syslog(LOG_NOTICE, "FoodLoop: upload complete\n");
        }
      else
        {
          printf("FoodLoop: capture ready at %s\n", FOODLOOP_CAPTURE_PATH);
        }
    }
}

int main(int argc, FAR char *argv[])
{
  FAR const char *command;
  FAR const char *gateway_host;
  uint16_t gateway_port = FOODLOOP_GATEWAY_PORT;

  if (argc == 1)
    {
      return foodloop_boot_watch(NULL, 0);
    }

  command = argv[1];
  if (strcmp(command, "demo") == 0 && argc == 2)
    {
      return foodloop_demo_loop();
    }

  if (strcmp(command, "capture") == 0 && argc == 2)
    {
      return foodloop_capture_and_maybe_analyze(NULL, 0);
    }

  if (strcmp(command, "list") == 0 && argc == 2)
    {
      return foodloop_list_records();
    }

  if (strcmp(command, "status") == 0 && (argc == 2 || argc == 3))
    {
      return foodloop_status_records(argc == 3 ? argv[2] : NULL);
    }

  if ((strcmp(command, "analyze") == 0 || strcmp(command, "scan") == 0 ||
       strcmp(command, "watch") == 0 || strcmp(command, "boot") == 0) &&
      (argc == 2 || argc == 3 || argc == 4))
    {
      gateway_host = NULL;
      if (argc >= 3)
        {
          gateway_host = argv[2];
        }

      if (argc == 4 && foodloop_parse_port(argv[3], &gateway_port) < 0)
        {
          printf("FoodLoop: invalid gateway port\n");
          return ERROR;
        }

      if (strcmp(command, "analyze") == 0 && gateway_host != NULL)
        {
          return foodloop_upload_capture(gateway_host, gateway_port);
        }

      if (strcmp(command, "scan") == 0 && gateway_host != NULL)
        {
          return foodloop_capture_and_maybe_analyze(gateway_host,
                                                    gateway_port);
        }

      if (strcmp(command, "boot") == 0 || gateway_host == NULL)
        {
          return foodloop_boot_watch(gateway_host, gateway_port);
        }

      return foodloop_watch(gateway_host, gateway_port);
    }

  printf("Usage:\n");
  printf("  foodloop\n");
  printf("  foodloop capture\n");
  printf("  foodloop list\n");
  printf("  foodloop status [YYYY-MM-DD]\n");
  printf("  foodloop analyze <gateway-ip> [port]\n");
  printf("  foodloop scan <gateway-ip> [port]\n");
  printf("  foodloop watch <gateway-ip> [port]\n");
  printf("  foodloop boot [gateway-ip] [port]\n");
  return ERROR;
}
