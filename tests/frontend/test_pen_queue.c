#include "frontend/sdl/pen_queue.h"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
 mrc_pen_queue q={0};
 /* A delayed SDL batch must preserve the initial contact and final point,
  * without replaying hundreds of milliseconds of stale motion. */
 CHECK(mrc_pen_enqueue(&q,100,1000,60,1000,true,true,360,260));
 CHECK(mrc_pen_enqueue(&q,100,1000,60,1100,false,true,260,260));
 CHECK(mrc_pen_enqueue(&q,100,1000,60,1200,false,true,160,260));
 CHECK(q.head->press && q.head->at==100 && q.head->x==360);
 CHECK(q.tail->at==116 && q.tail->x==160 && q.head->next==q.tail);
 CHECK(mrc_pen_enqueue(&q,100,1000,60,1250,false,false,0,0));
 mrc_pen_pop(&q); CHECK(q.head->at==116 && q.head->x==160);
 mrc_pen_pop(&q); CHECK(q.head->at==160 && !q.head->down);
 mrc_pen_clear(&q);
 /* Sustained movement cannot starve the pending sample or grow the queue. */
 q=(mrc_pen_queue){0};
 CHECK(mrc_pen_enqueue(&q,0,1000,60,0,true,true,0,20));
 mrc_pen_pop(&q);
 for(unsigned i=1;i<1000;i++) {
  CHECK(mrc_pen_enqueue(&q,1,1000,60,i*10,false,true,i,20));
  CHECK(q.head==q.tail && q.head->at==10);
 }
 CHECK(q.head->x==999);
 CHECK(mrc_pen_enqueue(&q,2,1000,60,10000,false,false,0,0));
 CHECK(q.tail->at==60 && !q.tail->down);
 mrc_pen_clear(&q);
 /* After the press has been consumed, a late motion/release batch must
  * still allow the guest to sample the final point. Old event timestamps
  * must not place both deadlines behind the already executed guest time. */
 q=(mrc_pen_queue){0};
 CHECK(mrc_pen_enqueue(&q,0,1000,60,1000,true,true,360,260));
 mrc_pen_pop(&q);
 CHECK(mrc_pen_enqueue(&q,200,1000,60,1050,false,true,160,260));
 CHECK(mrc_pen_enqueue(&q,200,1000,60,1060,false,false,0,0));
 CHECK(q.head->at==200 && q.head->x==160);
 CHECK(q.tail->at==216 && !q.tail->down);
 mrc_pen_clear(&q);
 /* Coalescing an overdue pending motion must also re-anchor it. */
 q=(mrc_pen_queue){0};
 CHECK(mrc_pen_enqueue(&q,0,1000,60,1000,true,true,360,260));
 mrc_pen_pop(&q);
 CHECK(mrc_pen_enqueue(&q,0,1000,60,1010,false,true,300,260));
 CHECK(mrc_pen_enqueue(&q,200,1000,60,1020,false,true,160,260));
 CHECK(mrc_pen_enqueue(&q,200,1000,60,1030,false,false,0,0));
 CHECK(q.head->next==q.tail);
 CHECK(q.head->at==200 && q.tail->at==216);
 mrc_pen_clear(&q);
 /* Wrap and stale timestamps cannot create distant deadlines. */
 q=(mrc_pen_queue){0};
 CHECK(mrc_pen_enqueue(&q,1000,1000,60,0xfffffff0,true,true,10,20));
 CHECK(mrc_pen_enqueue(&q,1010,1000,60,0x54,false,false,0,0));
 CHECK(q.tail->at==1060);
 mrc_pen_clear(&q);
 CHECK(mrc_pen_enqueue(&q,5000,1000,60,0x100,true,true,10,20));
 CHECK(q.head->at==5000);
 CHECK(mrc_pen_enqueue(&q,5000,1000,60,0xff,false,false,0,0));
 CHECK(q.tail->at==5060);
 mrc_pen_clear(&q);
 /* The actual delivery path turns a coalesced 100-pixel jump into a
  * timed path, and cannot release before the final point is sampled. */
 q=(mrc_pen_queue){0};
 mrc_pen_event delivered;
 CHECK(mrc_pen_enqueue(&q,0,12000,720,1000,true,true,160,260));
 CHECK(mrc_pen_deliver(&q,0,12000,&delivered) && delivered.press);
 CHECK(mrc_pen_enqueue(&q,1200,12000,720,1050,false,true,260,260));
 CHECK(mrc_pen_enqueue(&q,1200,12000,720,1060,false,false,0,0));
 unsigned previous=160, points=0;
 uint64_t last_point=0, finished=0;
 while(q.head) {
  uint64_t now=q.head->at;
  if(!mrc_pen_deliver(&q,now,12000,&delivered)) continue;
  if(delivered.down) {
   CHECK(delivered.x>previous && delivered.x-previous<=16);
   CHECK(delivered.y==260);
   if(points) CHECK(now-last_point>=100);
   previous=delivered.x; last_point=now; points++;
  } else {
   CHECK(previous==260 && now-last_point>=200);
   finished=now;
  }
 }
 CHECK(points==7 && finished<=2200);
 /* A reversal updates the pending destination from the delivered point,
  * without finishing an obsolete excursion first. */
 q=(mrc_pen_queue){0};
 CHECK(mrc_pen_enqueue(&q,0,12000,720,0,true,true,160,160));
 CHECK(mrc_pen_deliver(&q,0,12000,&delivered));
 CHECK(mrc_pen_enqueue(&q,200,12000,720,20,false,true,360,300));
 CHECK(mrc_pen_deliver(&q,q.head->at,12000,&delivered));
 CHECK(delivered.x==176 && delivered.y>160);
 uint64_t next=q.head->at;
 CHECK(mrc_pen_enqueue(&q,next-1,12000,720,30,false,true,100,100));
 CHECK(q.head==q.tail && q.head->at==next);
 CHECK(mrc_pen_deliver(&q,next,12000,&delivered));
 CHECK(delivered.x==160 && delivered.y<176);
 /* Even an overdue deadline emits one step, not a burst at one slot. */
 CHECK(mrc_pen_deliver(&q,10000,12000,&delivered));
 CHECK(!mrc_pen_deliver(&q,10000,12000,&delivered));
 CHECK(q.head->at==10100);
 mrc_pen_clear(&q);
 /* A whole-panel diagonal has a fixed latency bound and exact endpoint. */
 q=(mrc_pen_queue){0};
 CHECK(mrc_pen_enqueue(&q,0,12000,720,0,true,true,0,0));
 CHECK(mrc_pen_enqueue(&q,0,12000,720,1,false,true,479,319));
 CHECK(mrc_pen_enqueue(&q,0,12000,720,2,false,false,0,0));
 unsigned px=0,py=0; points=0;
 while(q.head) {
  uint64_t now=q.head->at;
  if(!mrc_pen_deliver(&q,now,12000,&delivered)) continue;
  if(delivered.down) {
   CHECK(delivered.x>=px && delivered.x-px<=16);
   CHECK(delivered.y>=py && delivered.y-py<=16);
   px=delivered.x;py=delivered.y;points++;
  } else CHECK(px==479 && py==319 && now<=3400);
 }
 CHECK(points==31);
 /* A later queued press must not extend the preceding click's hold. */
 q=(mrc_pen_queue){0};
 CHECK(mrc_pen_enqueue(&q,0,12000,720,1000,true,true,10,10));
 CHECK(mrc_pen_enqueue(&q,0,12000,720,1010,false,false,0,0));
 CHECK(mrc_pen_enqueue(&q,0,12000,720,1020,true,true,20,20));
 CHECK(mrc_pen_enqueue(&q,0,12000,720,1030,false,false,0,0));
 uint64_t first_up=q.head->next->at;
 CHECK(mrc_pen_deliver(&q,0,12000,&delivered) && delivered.press);
 CHECK(mrc_pen_deliver(&q,first_up,12000,&delivered) && !delivered.down);
 CHECK(first_up==720);
 mrc_pen_clear(&q);
 return 0;
}
