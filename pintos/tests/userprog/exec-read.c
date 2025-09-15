/* Read document for exec() carefully... */

#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include "tests/userprog/boundary.h"
#include "tests/userprog/sample.inc"
#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void) 
{
  pid_t pid;
  int handle;
  int byte_cnt;
  char *buffer;

  CHECK ((handle = open ("sample.txt")) > 1, "open \"sample.txt\""); 
  buffer = get_boundary_area () - sizeof sample / 2; /* 일부러 시작 주소를 페이지 끝 근처로 잡음 */
  CHECK ((byte_cnt = read (handle, buffer, 20)) == 20, /* 첫 페이지 말단 */
         "read \"sample.txt\" first 20 bytes");

  
  if ((pid = fork ("child-read"))){
    wait (pid);

    byte_cnt = read (handle, buffer + 20, sizeof sample - 21); /* 다음 페이지로 이어지도록 */
    if (byte_cnt != sizeof sample - 21)
      fail ("read() returned %d instead of %zu", byte_cnt, sizeof sample - 21);
    else if (strcmp (sample, buffer)) {
        msg ("expected text:\n%s", sample);
        msg ("text actually read:\n%s", buffer);
        fail ("expected text differs from actual");
    } else {
      msg ("Parent success");
    }

    close (handle);
  } else {
    char cmd_line[128];
    snprintf (cmd_line, sizeof cmd_line, "%s %d", "child-read", handle);
    exec (cmd_line);
  }
}
/* 유저 버퍼가 페이지 경계를 가로질러도 read()가 안전하게 복사해야 한다. */
