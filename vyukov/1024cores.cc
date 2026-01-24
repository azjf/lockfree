/**
   http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
   https://sites.google.com/site/1024cores/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue

   Advantages:
    + Intrusive. No need for additional internal nodes.
    + Wait-free and fast producers. One XCHG is maximum what one can get with
   multi-producer non-distributed queue.
    + Extremely fast consumer. On fast-path it's atomic-free, XCHG executed per
   node batch, in order to grab 'last item'.
    + No need for node order reversion. So pop operation is always O(1).
    + ABA-free.
    + No need for PDR. That is, one can use this algorithm out-of-the-box. No
   need for thread registration/deregistration, periodic activity, deferred
   garbage etc.

   Disadvantages:
    - Push function is blocking wrt consumer. I.e. if producer blocked in (*),
   then consumer is blocked too. Fortunately 'window of inconsistency' is
   extremely small - producer must be blocked exactly in (*). Actually it's
   disadvantage only as compared with totally lockfree algorithm. It's still
   much better lockbased algorithm.
 */

struct mpscq_node_t
{
    mpscq_node_t* volatile next;
};

struct mpscq_t
{
    mpscq_node_t* volatile head;
    mpscq_node_t* tail;
    // without stub, push() have to modify both head and tail when the queue is
    // empty!
    // CCIA P239 alse use dummy node in push() to seperate head and tail.
    mpscq_node_t stub;
};

#define MPSCQ_STATIC_INIT(self) {&self.stub, &self.stub, {0}}

void mpscq_create(mpscq_t* self)
{
    self->head = &self->stub;
    self->tail = &self->stub;
    self->stub.next = 0;
}

void mpscq_push(mpscq_t* self, mpscq_node_t* n)
{
    n->next = 0;
    mpscq_node_t* prev = XCHG(&self->head, n);
    // (*) if producer blocked here, then consumer is blocked too. Fortunately
    // 'window of inconsistency' is extremely small - producer must be blocked
    // exactly in (*).
    prev->next = n;
}

mpscq_node_t* mpscq_pop(mpscq_t* self)
{
    mpscq_node_t* tail = self->tail;
    mpscq_node_t* next = tail->next;
    if (tail == &self->stub) {
        if (0 == next) return 0;
        self->tail = next;  // skip stub
        tail = next;
        next = next->next;
    }
    if (next) {
        self->tail = next;
        return tail;
    }
    mpscq_node_t* head = self->head;
    if (tail != head) return 0;
    mpscq_push(self, &self->stub);
    next = tail->next;
    if (next) {
        self->tail = next;
        return tail;
    }
    // 1. reordering may happen between `next = tail->next;` and `prev->next =
    // n;` in mpscq_push()
    // 2. threadA if (tail != head) -> threadB push() -> threadB was preempted
    // -> threadA push(stub) -> threadA tail->next == nullptr
    return 0;
}
