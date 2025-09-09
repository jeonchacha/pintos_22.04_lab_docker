#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

/* ----- Alarm Clock additions ----- */
static struct list sleep_list; /* 블록된(잠자는) 스레드들의 정렬 리스트 */

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* ----- Alarm Clock additions ----- */
static bool wakeup_less (const struct list_elem *a,
			 			 const struct list_elem *b,
			 			 void *aux);
static void wake_ready_threads_by_timer (void);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");

	/* ----- Alarm Clock additions ----- */
	list_init(&sleep_list);
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
/* busy-wait 금지 */
void
timer_sleep (int64_t ticks) {
	/* 음수/0 처리: 바로 반환 */
	if (ticks <= 0) return;

	/* 절대 깨울 시간 계산 */
	int64_t wake_at = timer_ticks () + ticks;

	enum intr_level old_level = intr_disable (); /* 크리티컬 섹션 시작 */

	struct thread *cur = thread_current();
	cur->wakeup_tick = wake_at;

	/* wakeup_tick 오름차순 유지되도록 정렬 삽입 */
	list_insert_ordered(&sleep_list, &cur->elem, wakeup_less, NULL);

	/* 현재 스레드를 블록: 이후 타이머 인터럽트에서 언블록됨 
	   상태 전이: RUNNING -> BLOCKED. 
	   이 스레드는 타이머 핸들러에서 깨워줄 때까지 CPU를 쓰지 않음(busy-wait 제거).
	*/
	thread_block();

	/* 크리티컬 섹션 종료: 인터럽트 상태 복원. 
	   thread_block() 직전까지 반드시 비활성화 상태 유지해야 "lost wakeup"을 피함.
	*/
	intr_set_level (old_level); 
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler.
   매 틱마다 깨울 스레드가 있는지 즉시 처리.
*/
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();

	wake_ready_threads_by_timer ();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}

/* 정렬 비교 함수: wakeup_tick 오름차순, 동률이면 FIFO(뒤에 삽입) 또는 우선순위 */
static bool 
wakeup_less (const struct list_elem *a,
			 const struct list_elem *b,
			 void *aux) 
{
	const struct thread *ta = list_entry(a, struct thread, elem);
	const struct thread *tb = list_entry(b, struct thread, elem);

	if (ta->wakeup_tick != tb->wakeup_tick) {
		return ta->wakeup_tick < tb->wakeup_tick; // 오름차순
	}

	/* 동률이면 우선순위 큰 스레드를 앞에 */
	return ta->priority > tb->priority;
}

/* wakeup loop */
static void
wake_ready_threads_by_timer (void)
{
	/* 핸들러 문맥: 이미 인터럽트 비활성화 상태.
	   더 높은 우선순위 스레드를 깨웠는지 추적.
	*/
	bool need_yield = false;

	while (!list_empty(&sleep_list)) {
		/* 정렬 리스트의 맨 앞이 가장 이른 wakeup_tick */
		struct thread *t = list_entry (list_front (&sleep_list), struct thread, elem);

		/* 지금 깨울 차례가 아니면 종료 (정렬 리스트이므로 맨 앞만 보면 됨) */
		if (t->wakeup_tick > timer_ticks ()) {
			break;
		}

		/* 깨울 시각이 지났다면 리스트에서 빼고 ready로 전환 */
		list_pop_front (&sleep_list);
		thread_unblock (t);

		/* 우선순위 스케줄링 대비: 더 높은 우선순위를 깨웠다면 반환 직후 양보 */
		if (t->priority > thread_current()->priority) {
			need_yield = true;
		}
	}

	if (need_yield) {
		intr_yield_on_return (); /* 핸들러 '리턴 후' 양보 (인터럽트 안에서 yield 금지 -> 문맥상 불가) */
	}
}