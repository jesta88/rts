#pragma once

#include "core.h"

void deque_init(WC_Deque* deque);
void deque_push(WC_Deque* deque, WC_Job* job);
WC_Job* deque_pop(WC_Deque* deque);
WC_Job* deque_steal(WC_Deque* deque);
