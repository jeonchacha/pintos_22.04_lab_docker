/* Forks a thread whose name spans the boundary between two pages.
   This is valid, so it must succeed. */

#include <syscall.h>
#include "tests/userprog/boundary.h"
#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void) 
{
  pid_t pid = fork ("child-simple");
  if (pid == 0){
    /* 
      자식
      copy_string_across_boundary("child-simple")로 경계를 가로지르는 문자열 버퍼를 만든 뒤,
	    그 포인터를 그대로 exec()에 전달한다.
	    커널은 그 포인터가 가리키는 문자열을 두 페이지에 걸쳐서 안전하게 복사해야 하고, 실패 없이 해당 프로그램을 로드·실행해야 한다.
    */
    exec (copy_string_across_boundary ("child-simple"));
  } else {
    int exit_val = wait(pid);
    CHECK (pid > 0, "fork");
    CHECK (exit_val == 81, "wait");
  }
}
/* 테스트 목적: 사용자 프로그램에서 커널로 전달하는 문자열 인자(여기선 exec()의 프로그램 이름)가 페이지 경계를 가로질러 저장되어 있어도, 
커널이 이를 안전하고 정확하게 읽어야 한다는 걸 검증. */